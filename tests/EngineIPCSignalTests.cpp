#include "config.h"
#include "ArgumentCoders.h"
#include "HaikuProcess.h"
#include "IPCEvent.h"
#include "IPCSemaphore.h"
#include "IPCSemaphoreSharedHaiku.h"
#include "SharedMemory.h"
#include <wtf/MainThread.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

using namespace IPC;
using Clock = std::chrono::steady_clock;
static unsigned checks, failures;
static void Check(bool result, const char* label)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static unsigned descriptorCount()
{
    unsigned count = 0;
    for (int fd = 0; fd < 4096; ++fd)
        count += fcntl(fd, F_GETFD) >= 0;
    return count;
}

static std::unique_ptr<Decoder> decode(Encoder& encoder)
{
    auto attachments = encoder.releaseAttachments();
    // This is the same wire-order reversal used by MessagePacketHaiku.
    std::reverse(attachments.begin(), attachments.end());
    return Decoder::create(encoder.span(), WTF::move(attachments));
}

static bool drain(Semaphore& semaphore, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (!semaphore.waitFor(Timeout::now()))
            return false;
    }
    return !semaphore.waitFor(Timeout::now());
}

static void countAndMoveTests()
{
    Semaphore original;
    Check(!!original, "allocate a native process-shared semaphore");
    auto descriptor = original.duplicateDescriptor();
    Check(!!descriptor && (fcntl(descriptor.value(), F_GETFD) & FD_CLOEXEC), "duplicate semaphore descriptor has close-on-exec");
    Semaphore duplicate(WTF::move(descriptor));
    Check(!!duplicate, "import an actual production semaphore descriptor");
    for (unsigned i = 0; i < 200000; ++i)
        original.signal();
    Check(drain(duplicate, 200000), "200000 queued signals retain their exact count without a socket buffer");
    original.signal();
    Semaphore moved(WTF::move(original));
    Check(!original && !original.waitFor(Timeout::now()) && moved.waitFor(Timeout::now()), "move construction transfers the counter and invalidates its source");
    Semaphore assigned;
    moved.signal();
    assigned = WTF::move(moved);
    Check(!moved && assigned.waitFor(Timeout::now()), "move assignment releases old resources and preserves the incoming count");
    assigned = WTF::move(assigned);
    assigned.signal();
    Check(duplicate.waitFor(Timeout::now()), "self move and duplicated lifetime preserve the shared object");
    Encoder encoder(MessageName::LegacySessionState, 0);
    encoder << assigned;
    auto decoder = decode(encoder);
    auto restored = decoder ? decoder->decode<Semaphore>() : std::nullopt;
    assigned.signal();
    Check(restored && restored->waitFor(Timeout::now()), "generated Semaphore serializer transfers its single descriptor");
}

static void concurrentTests()
{
    Semaphore semaphore;
    std::atomic<unsigned> received { 0 };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (unsigned j = 0; j < 5000; ++j) {
                if (!semaphore.waitFor(5_s))
                    return;
                ++received;
            }
        });
        threads.emplace_back([&] {
            for (unsigned j = 0; j < 5000; ++j)
                semaphore.signal();
        });
    }
    for (auto& thread : threads)
        thread.join();
    Check(received == 20000 && !semaphore.waitFor(Timeout::now()), "four producers and four waiters consume exactly 20000 signals");
    std::thread signaler([&] { usleep(30000); semaphore.signal(); });
    Check(semaphore.wait(), "unbounded wait is woken by a real semaphore signal");
    signaler.join();
}

static volatile sig_atomic_t interruptions;
static void interruptHandler(int) { interruptions = interruptions + 1; }

static void deadlineTests()
{
    Semaphore semaphore;
    auto start = Clock::now();
    Check(!semaphore.waitFor(Timeout::now()), "an empty semaphore respects an immediate timeout");
    Check(Clock::now() - start < std::chrono::milliseconds(30), "zero timeout does not enter a blocking wait");
    struct sigaction action { }, previous { };
    action.sa_handler = interruptHandler;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR1, &action, &previous);
    auto mainThread = pthread_self();
    std::thread interrupter([mainThread] {
        for (unsigned i = 0; i < 35; ++i) {
            usleep(10000);
            pthread_kill(mainThread, SIGUSR1);
        }
    });
    start = Clock::now();
    bool timedOut = !semaphore.waitFor(150_ms);
    auto elapsed = Clock::now() - start;
    interrupter.join();
    sigaction(SIGUSR1, &previous, nullptr);
    Check(interruptions > 5 && timedOut, "a real signal storm interrupts native waits without granting a count");
    Check(elapsed >= std::chrono::milliseconds(120) && elapsed < std::chrono::milliseconds(300), "EINTR retries preserve the original monotonic deadline");
    auto pair = createEventSignalPair();
    start = Clock::now();
    Check(pair && !pair->event.waitFor(45_ms), "Event respects a finite timeout while its Signal remains alive");
    Check(Clock::now() - start < std::chrono::milliseconds(150), "Event timeout is shorter than its peer-death polling interval");
}

static void malformedTests()
{
    Semaphore invalid(UnixFileDescriptor { });
    Check(!invalid && !invalid.waitFor(Timeout::now()), "an invalid imported descriptor produces an unusable semaphore");
    int pipeFD[2];
    pipe(pipeFD);
    Semaphore pipeImport(UnixFileDescriptor { pipeFD[0], UnixFileDescriptor::Adopt });
    close(pipeFD[1]);
    Check(!pipeImport, "a pipe cannot masquerade as shared semaphore memory");
    int sockets[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    Semaphore socketImport(UnixFileDescriptor { sockets[0], UnixFileDescriptor::Adopt });
    close(sockets[1]);
    Check(!socketImport, "a socket cannot masquerade as shared semaphore memory");
    Semaphore valid;
    auto descriptor = valid.duplicateDescriptor();
    struct stat info;
    fstat(descriptor.value(), &info);
    auto memory = WebCore::SharedMemory::allocate(info.st_size);
    Check(!!memory, "allocate malformed import fixtures");
    if (!memory)
        return;
    auto zero = memory->createHandle(WebCore::SharedMemory::Protection::ReadWrite);
    Semaphore zeroImport(zero->releaseHandle());
    Check(!zeroImport, "an exact-size zero-filled file fails format validation");
    pread(descriptor.value(), memory->mutableSpan().data(), memory->size(), 0);
    auto readOnly = memory->createHandle(WebCore::SharedMemory::Protection::ReadOnly);
    Semaphore readOnlyImport(readOnly->releaseHandle());
    Check(!readOnlyImport, "a read-only descriptor is rejected before mapping the semaphore");
    auto& forged = *reinterpret_cast<IPC::SharedSemaphoreHaiku*>(memory->mutableSpan().data());
    auto writable = memory->createHandle(WebCore::SharedMemory::Protection::ReadWrite);
    auto writableFD = writable->releaseHandle();
    Semaphore liveImport(writableFD.duplicate());
    Check(!!liveImport, "a copied valid object can be imported before corruption");
    forged.count = std::numeric_limits<uint64_t>::max();
    Check(!Semaphore::isValidDescriptor(writableFD), "an imported overflowing count is rejected");
    liveImport.signal();
    Check(forged.count == std::numeric_limits<uint64_t>::max() && !liveImport.waitFor(Timeout::now()), "count corruption after import is rejected without receiver abort or wraparound");
    forged.count = 0;
    forged.wake.u.unnamed_sem = std::numeric_limits<int32_t>::max();
    Check(!Semaphore::isValidDescriptor(writableFD), "an imported forged native wake counter is rejected");
    liveImport.signal();
    Check(forged.count == 0 && !liveImport.waitFor(Timeout::now()), "wake corruption after import cannot overflow native sem_post");
    forged.wake.u.unnamed_sem = 0;
    // Simulate a peer dying after publishing a permit and claiming the wake,
    // before entering sem_post. Future waits must still consume that permit.
    forged.wakePending = 1;
    std::thread publisher([&] { usleep(30000); forged.count = 1; });
    Check(liveImport.waitFor(300_ms), "a published count survives a peer dying before its wake syscall");
    publisher.join();
    memory->mutableSpan()[0] ^= 1;
    auto corrupted = memory->createHandle(WebCore::SharedMemory::Protection::ReadWrite);
    auto corruptedFD = corrupted->releaseHandle();
    Check(!Semaphore::isValidDescriptor(corruptedFD), "the descriptor validator rejects a corrupted format marker");
    Encoder encoder(MessageName::LegacySessionState, 0);
    encoder << WTF::move(corruptedFD);
    auto decoder = decode(encoder);
    Check(decoder && !decoder->decode<Semaphore>() && !decoder->isValid(), "generated decoding rejects malformed semaphore attachments");
    auto shortMemory = WebCore::SharedMemory::allocate(1);
    auto shortHandle = shortMemory->createHandle(WebCore::SharedMemory::Protection::ReadWrite);
    Semaphore shortImport(shortHandle->releaseHandle());
    Check(!shortImport, "a truncated object is rejected without touching beyond its file");
}

static void eventTests()
{
    auto pair = createEventSignalPair();
    Check(!!pair, "createEventSignalPair allocates working native endpoints");
    if (!pair)
        return;
    for (unsigned i = 0; i < 1000; ++i)
        pair->signal.signal();
    bool all = true;
    for (unsigned i = 0; i < 1000; ++i)
        all &= pair->event.waitFor(Timeout::now());
    Check(all && !pair->event.waitFor(Timeout::now()), "Event receives every queued signal exactly once");
    Encoder encoder(MessageName::LegacySessionState, 0);
    encoder << WTF::move(pair->signal);
    auto attachments = encoder.releaseAttachments();
    Check(attachments.size() == 2, "Signal serialization carries a counter FD and a lifetime FD");
    std::reverse(attachments.begin(), attachments.end());
    auto decoder = Decoder::create(encoder.span(), WTF::move(attachments));
    auto signal = decoder ? decoder->decode<Signal>() : std::nullopt;
    Check(!!signal, "the generated Signal decoder imports both native capabilities");
    if (!signal)
        return;
    signal->signal();
    signal->signal();
    signal.reset();
    Check(pair->event.waitFor(Timeout::now()) && pair->event.waitFor(Timeout::now()), "published signals drain after the final Signal owner is destroyed");
    auto start = Clock::now();
    Check(!pair->event.wait(), "destroying the final Signal interrupts an otherwise unbounded Event wait");
    Check(Clock::now() - start < std::chrono::milliseconds(200), "local peer destruction is detected promptly");
    auto abandoned = createEventSignalPair();
    Encoder queued(MessageName::LegacySessionState, 0);
    queued << WTF::move(abandoned->signal);
    abandoned.reset();
    auto abandonedDecoder = decode(queued);
    auto abandonedSignal = abandonedDecoder ? abandonedDecoder->decode<Signal>() : std::nullopt;
    Check(!!abandonedSignal, "a queued Signal still decodes after its Event has been destroyed");
    if (abandonedSignal)
        abandonedSignal->signal();
    Check(!!abandonedSignal, "signalling after Event cancellation preserves normal lifetime semantics");
    Semaphore validSemaphore;
    int pipes[2];
    pipe(pipes);
    Encoder malformed(MessageName::LegacySessionState, 0);
    malformed << validSemaphore << UnixFileDescriptor { pipes[0], UnixFileDescriptor::Adopt };
    close(pipes[1]);
    auto malformedDecoder = decode(malformed);
    Check(malformedDecoder && !malformedDecoder->decode<Signal>() && !malformedDecoder->isValid(), "a forged Signal lifetime descriptor fails generated decoding");
    auto corrupted = createEventSignalPair();
    auto corruptionMemory = WebCore::SharedMemory::map(
        { corrupted->signal.semaphore().duplicateDescriptor(), sizeof(SharedSemaphoreHaiku) },
        WebCore::SharedMemory::Protection::ReadWrite);
    auto& corruptionState = *reinterpret_cast<SharedSemaphoreHaiku*>(corruptionMemory->mutableSpan().data());
    std::thread corruptor([&] { usleep(30000); corruptionState.count = std::numeric_limits<uint64_t>::max(); });
    start = Clock::now();
    Check(!corrupted->event.wait(), "corruption during Event wait fails without an infinite retry loop");
    auto corruptionElapsed = Clock::now() - start;
    corruptor.join();
    Check(corruptionElapsed < std::chrono::milliseconds(300), "Event rejects corrupt shared bookkeeping within its next bounded check");
    auto live = createEventSignalPair();
    std::thread closer([signal = std::optional<Signal>(WTF::move(live->signal))]() mutable { usleep(30000); signal.reset(); });
    start = Clock::now();
    Check(!live->event.wait(), "peer destruction interrupts a wait already blocked in the native semaphore");
    auto elapsed = Clock::now() - start;
    closer.join();
    Check(elapsed < std::chrono::milliseconds(250), "blocked peer-death detection remains bounded");
}

static bool readable(int fd)
{
    pollfd descriptor { fd, POLLIN, 0 };
    return poll(&descriptor, 1, 5000) == 1 && (descriptor.revents & POLLIN);
}

static int child()
{
    std::array<uint8_t, 4096> bytes;
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(3 * sizeof(int))> control { };
    iovec data { bytes.data(), bytes.size() };
    msghdr message { };
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    if (!readable(3))
        return 1;
    auto size = recvmsg(3, &message, MSG_CMSG_CLOEXEC);
    auto* header = CMSG_FIRSTHDR(&message);
    if (size <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || !header
        || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS
        || header->cmsg_len != CMSG_LEN(3 * sizeof(int)))
        return 2;
    std::array<int, 3> fds;
    std::memcpy(fds.data(), CMSG_DATA(header), sizeof(fds));
    Vector<Attachment> attachments;
    for (auto fd : fds) {
        if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC))
            return 3;
        attachments.append(UnixFileDescriptor { fd, UnixFileDescriptor::Adopt });
    }
    std::reverse(attachments.begin(), attachments.end());
    auto decoder = Decoder::create(std::span<const uint8_t> { bytes.data(), static_cast<size_t>(size) }, WTF::move(attachments));
    if (!decoder)
        return 4;
    auto semaphore = decoder->decode<Semaphore>();
    auto signal = decoder->decode<Signal>();
    if (!semaphore || !signal || !semaphore->waitFor(5_s))
        return 5;
    for (unsigned i = 0; i < 5; ++i)
        semaphore->signal();
    for (unsigned i = 0; i < 20000; ++i)
        signal->signal();
    char ready = 'R';
    if (send(3, &ready, 1, 0) != 1)
        return 6;
    for (;;)
        pause();
}

static void processTests(const char* executable)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) {
        Check(false, "create native child transport");
        return;
    }
    char* path = realpath(executable, nullptr);
    auto process = WebKit::spawnHaikuProcess(path, 7388123, sockets[0]);
    free(path);
    close(sockets[0]);
    Check(!process.error && process.process > 0, "spawn a separate native process for actual FD transfer");
    if (process.error) { close(sockets[1]); return; }
    Semaphore semaphore;
    auto pair = createEventSignalPair();
    Encoder encoder(MessageName::LegacySessionState, 0);
    encoder << semaphore << WTF::move(pair->signal);
    auto attachments = encoder.releaseAttachments();
    alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(3 * sizeof(int))> control { };
    auto bytes = encoder.span();
    iovec data { const_cast<uint8_t*>(bytes.data()), bytes.size() };
    msghdr message { };
    message.msg_iov = &data; message.msg_iovlen = 1;
    message.msg_control = control.data(); message.msg_controllen = control.size();
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(3 * sizeof(int));
    std::array<int, 3> fds { attachments[0].value(), attachments[1].value(), attachments[2].value() };
    std::memcpy(CMSG_DATA(header), fds.data(), sizeof(fds));
    Check(sendmsg(sockets[1], &message, 0) == static_cast<ssize_t>(bytes.size()), "transfer production serialized semaphore and signal with SCM_RIGHTS");
    attachments.clear();
    semaphore.signal();
    char ready = 0;
    bool childReady = readable(sockets[1]) && recv(sockets[1], &ready, 1, 0) == 1 && ready == 'R';
    Check(childReady, "child decodes close-on-exec capabilities and wakes from the shared semaphore");
    if (childReady) {
        Check(drain(semaphore, 5), "cross-process count updates remain exact in both directions");
        bool all = true;
        for (unsigned i = 0; i < 20000; ++i)
            all &= pair->event.waitFor(Timeout::now());
        Check(all && !pair->event.waitFor(Timeout::now()), "20000 cross-process Event signals arrive without loss");
    }
    std::thread killer([pid = process.process] { usleep(30000); kill(pid, SIGKILL); });
    auto start = Clock::now();
    Check(!pair->event.wait(), "SIGKILL of the sole remote Signal owner interrupts Event wait");
    auto elapsed = Clock::now() - start;
    killer.join();
    int status = 0;
    waitpid(process.process, &status, 0);
    close(sockets[1]);
    Check(WIFSIGNALED(status) && elapsed < std::chrono::milliseconds(300), "actual peer-death interruption completes within its bounded polling interval");
}

int main(int argc, char** argv)
{
    WTF::initializeMainThread();
    if (argc > 2)
        return child();
    // SharedMemory's first allocation lazily opens the process-wide RNG.
    // Establish the resource baseline after that real, persistent singleton.
    auto startupDescriptors = descriptorCount();
    { Semaphore warmup; Check(!!warmup, "initialize the production shared-memory random source"); }
    auto before = descriptorCount();
    std::printf("Descriptor baseline: startup=%u after RNG initialization=%u\n", startupDescriptors, before);
    countAndMoveTests();
    concurrentTests();
    deadlineTests();
    malformedTests();
    eventTests();
    processTests(argv[0]);
    auto after = descriptorCount();
    std::printf("Descriptor cleanup: before=%u after=%u\n", before, after);
    Check(after == before, "all semaphore mappings and transport descriptors are released");
    std::printf("Native IPC signal checks: %u passed, %u failed\n", checks - failures, failures);
    return failures ? 1 : 0;
}
