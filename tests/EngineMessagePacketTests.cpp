#include "config.h"
#include "MessageInfo.h"
#include "MessagePacketHaiku.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <wtf/MainThread.h>

static int checks = 0, failures = 0;
static void Check(bool ok, const char* label)
{
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
struct Pair {
    UnixFileDescriptor reader, writer;
    Pair()
    {
        int descriptors[2];
        RELEASE_ASSERT(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, descriptors));
        reader = { descriptors[0], UnixFileDescriptor::Adopt };
        writer = { descriptors[1], UnixFileDescriptor::Adopt };
    }
};
static int DescriptorCount()
{
    int result = 0;
    for (int i = 0; i < 2048; ++i) result += fcntl(i, F_GETFD) >= 0;
    return result;
}
static std::vector<uint8_t> Frame(size_t declaredBody, std::vector<uint8_t> metadata,
    std::string body, bool outOfLine = false)
{
    IPC::MessageInfo info(declaredBody, metadata.size() - (outOfLine ? 1 : 0));
    if (outOfLine) info.setBodyOutOfLine();
    std::vector<uint8_t> bytes(sizeof(info) + metadata.size() + body.size());
    std::memcpy(bytes.data(), &info, sizeof(info));
    if (!metadata.empty()) std::memcpy(bytes.data() + sizeof(info), metadata.data(), metadata.size());
    if (!body.empty()) std::memcpy(bytes.data() + sizeof(info) + metadata.size(), body.data(), body.size());
    return bytes;
}
static void Send(int socket, const std::vector<uint8_t>& bytes, std::vector<int> descriptors = {})
{
    iovec vector { const_cast<uint8_t*>(bytes.data()), bytes.size() };
    IPC::FileDescriptorMessageHaiku control;
    msghdr message { };
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    RELEASE_ASSERT(control.setDescriptors(descriptors));
    control.attachTo(message);
    auto sent = sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent != static_cast<ssize_t>(bytes.size())) {
        std::fprintf(stderr, "Fixture send failed (%zu descriptors): %s\n", descriptors.size(), std::strerror(errno));
        std::exit(1);
    }
}
static std::string Body(const IPC::MessagePacketHaiku& packet)
{
    return { reinterpret_cast<const char*>(packet.body().data()), packet.body().size() };
}
int main()
{
    WTF::initializeMainThread();
    using Result = IPC::MessagePacketHaiku::Result;
    Pair pair;
    IPC::MessagePacketHaiku packet;
    Check(packet.receive(pair.reader.value()) == Result::WouldBlock, "idle receive returns without blocking");
    Send(pair.writer.value(), Frame(5, {}, "hello"));
    Check(packet.receive(pair.reader.value()) == Result::Message && Body(packet) == "hello",
        "inline packet preserves its exact body");
    Send(pair.writer.value(), Frame(3, {}, "one"));
    Send(pair.writer.value(), Frame(3, {}, "two"));
    Check(packet.receive(pair.reader.value()) == Result::Message && Body(packet) == "one"
        && packet.receive(pair.reader.value()) == Result::Message && Body(packet) == "two",
        "queued sequenced packets remain distinct and ordered");

    UnixFileDescriptor nullFile { open("/dev/null", O_RDONLY | O_CLOEXEC), UnixFileDescriptor::Adopt };
    RELEASE_ASSERT(nullFile);
    // Initialize the process-wide random device used to name shared mappings
    // before measuring packet descriptor ownership.
    { auto warmup = WebCore::SharedMemory::allocate(1); RELEASE_ASSERT(warmup); }
    int baseline = DescriptorCount();
    auto Reject = [&](std::vector<uint8_t> bytes, const char* label, bool sendDescriptor = false) {
        Send(pair.writer.value(), bytes, sendDescriptor ? std::vector<int> { nullFile.value() } : std::vector<int> { });
        Check(packet.receive(pair.reader.value()) == Result::Error, label);
        Check(DescriptorCount() == baseline, "rejected packet releases every received descriptor");
    };
    Reject({ 1, 2 }, "truncated message header is rejected", true);
    Reject(Frame(3, {}, "abc"), "undeclared descriptor is rejected", true);
    Reject(Frame(3, { 0 }, "abc"), "missing attachment descriptor is rejected");
    Reject(Frame(3, { 2 }, "abc"), "invalid attachment flag is rejected", true);
    auto invalidFlag = Frame(3, {}, "abc");
    invalidFlag[sizeof(size_t) * 2] = 2;
    Reject(invalidFlag, "invalid out-of-line flag is rejected", true);
    auto excessiveCount = Frame(3, {}, "abc");
    size_t impossible = std::numeric_limits<size_t>::max();
    std::memcpy(excessiveCount.data() + sizeof(size_t), &impossible, sizeof(impossible));
    Reject(excessiveCount, "overflowing attachment count is rejected", true);
    Reject(Frame(4, {}, "abc"), "short inline body is rejected", true);
    Reject(Frame(2, {}, "abc"), "unexpected trailing body bytes are rejected", true);
    Reject(Frame(0, {}, ""), "empty encoded message is rejected", true);
    auto missingBodyHandle = Frame(3, {}, "");
    missingBodyHandle[sizeof(size_t) * 2] = 1;
    Reject(missingBodyHandle, "out-of-line body without metadata is rejected", true);
    Reject(Frame(3, { 1 }, "", true), "null out-of-line body attachment is rejected");
    Reject(Frame(3, { 0 }, "extra", true), "inline data attached to an out-of-line body is rejected", true);
    Reject(Frame(4096, { 0 }, "", true), "out-of-line descriptor smaller than the declared body is rejected", true);
    Reject(Frame(5000, {}, std::string(5000, 'x')), "truncated oversized packet is rejected", true);
    Send(pair.writer.value(), Frame(2, {}, "ok"));
    Check(packet.receive(pair.reader.value()) == Result::Message && Body(packet) == "ok",
        "rejected data cannot contaminate a subsequent packet");

    Send(pair.writer.value(), Frame(3, { 1, 0 }, "abc"), { nullFile.value() });
    Check(packet.receive(pair.reader.value()) == Result::Message, "null and real attachments are accepted together");
    auto attachments = packet.takeAttachments();
    Check(attachments.size() == 2 && attachments[0] && !attachments[1],
        "attachment order matches the decoder's reverse-consumption convention");
    Check(attachments.size() == 2 && (fcntl(attachments[0].value(), F_GETFD) & FD_CLOEXEC),
        "received descriptors have close-on-exec set atomically");
    attachments.clear();
    Check(DescriptorCount() == baseline && fcntl(nullFile.value(), F_GETFD) >= 0,
        "attachment ownership does not close the sender's descriptor");

    auto memory = WebCore::SharedMemory::allocate(8192);
    RELEASE_ASSERT(memory);
    std::memset(memory->mutableSpan().data(), 'w', memory->size());
    auto handle = memory->createHandle(WebCore::SharedMemory::Protection::ReadOnly);
    RELEASE_ASSERT(handle);
    auto descriptor = handle->releaseHandle();
    Send(pair.writer.value(), Frame(8192, { 0 }, "", true), { descriptor.value() });
    descriptor = { };
    memory = nullptr;
    Check(packet.receive(pair.reader.value()) == Result::Message && Body(packet) == std::string(8192, 'w'),
        "out-of-line data survives destruction of the sender's mapping and handle");
    Check(packet.takeAttachments().isEmpty(), "out-of-line body handle is not exposed as a message attachment");
    Check(packet.receive(pair.reader.value()) == Result::WouldBlock && DescriptorCount() == baseline,
        "reusing the packet releases its previous shared-memory mapping");

    constexpr auto maximumCount = IPC::attachmentMaxAmountHaiku;
    Send(pair.writer.value(), Frame(1, std::vector<uint8_t>(maximumCount, 0), "x"), std::vector<int>(maximumCount, nullFile.value()));
    bool acceptedMaximum = packet.receive(pair.reader.value()) == Result::Message;
    auto maximum = packet.takeAttachments();
    Check(acceptedMaximum && maximum.size() == maximumCount, "maximum native descriptor count is received intact");
    maximum.clear();
    Send(pair.writer.value(), Frame(1, std::vector<uint8_t>(maximumCount + 1, 0), "x"), std::vector<int>(maximumCount, nullFile.value()));
    Check(packet.receive(pair.reader.value()) == Result::Error && DescriptorCount() == baseline,
        "excess attachment metadata is rejected without leaking descriptors");
    for (int i = 0; i < 100; ++i) {
        Send(pair.writer.value(), Frame(3, { 2 }, "abc"), { nullFile.value() });
        RELEASE_ASSERT(packet.receive(pair.reader.value()) == Result::Error);
    }
    Check(DescriptorCount() == baseline, "repeated malformed packets do not exhaust the descriptor table");
    pair.writer = { };
    Check(packet.receive(pair.reader.value()) == Result::Closed, "peer shutdown is distinguished from a pending read");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
