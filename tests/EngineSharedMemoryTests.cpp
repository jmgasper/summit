#include "config.h"
#include "SharedMemory.h"
#include "HaikuProcess.h"
#include <Application.h>
#include <Bitmap.h>
#include <wtf/MainThread.h>
#include <wtf/RefPtr.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

using WebCore::SharedMemory;
using Protection = SharedMemory::Protection;
static int checks = 0, failures = 0;
static void Check(bool passed, const char* label)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static bool Readable(int socket)
{
    pollfd pollDescriptor { socket, POLLIN, 0 };
    int result;
    do { result = poll(&pollDescriptor, 1, 5000); } while (result < 0 && errno == EINTR);
    return result == 1 && (pollDescriptor.revents & POLLIN);
}

static int Child()
{
    WTF::initializeMainThread();
    area_id originalArea = -1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
    iovec data { &originalArea, sizeof(originalArea) };
    msghdr message { };
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (!Readable(3) || recvmsg(3, &message, MSG_CMSG_CLOEXEC) != sizeof(originalArea) || (message.msg_flags & MSG_CTRUNC))
        return 1;
    auto* header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int)))
        return 1;
    int descriptor;
    std::memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    SharedMemory::Handle handle(UnixFileDescriptor { descriptor, UnixFileDescriptor::Adopt }, 4096);
    SharedMemory::Handle writable(handle);
    std::array<int, 4> results { };
    results[0] = (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0;
    auto mapping = SharedMemory::map(WTF::move(handle), Protection::ReadOnly);
    results[1] = mapping && mapping->span()[0] == 8;
    results[2] = !SharedMemory::map(WTF::move(writable), Protection::ReadWrite);
    void* address = nullptr;
    area_id cloned = clone_area("summit-memory-test", &address, B_ANY_ADDRESS, B_READ_AREA, originalArea);
    results[3] = cloned < 0;
    if (cloned >= 0) delete_area(cloned);
    return send(3, results.data(), sizeof(results), 0) == sizeof(results) ? 0 : 1;
}

static void TestChild(const char* executable, SharedMemory& memory)
{
    char* path = realpath(executable, nullptr);
    if (!path) { Check(false, "resolve shared-memory child executable"); return; }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets)) {
        Check(false, "create shared-memory IPC connection");
        free(path);
        return;
    }
    fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
    fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    auto child = WebKit::spawnHaikuProcess(path, 7388001, sockets[0]);
    free(path);
    close(sockets[0]);
    Check(!child.error && child.process > 0, "launch a separate native memory consumer");
    if (child.error) { close(sockets[1]); return; }
    auto handle = memory.createHandle(Protection::ReadOnly);
    if (!handle) {
        Check(false, "create the child's read-only memory handle");
        kill(child.process, SIGKILL);
        close(sockets[1]);
        waitpid(child.process, nullptr, 0);
        return;
    }
    auto descriptor = handle->releaseHandle();
    area_id originalArea = area_for(memory.mutableSpan().data());
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
    iovec data { &originalArea, sizeof(originalArea) };
    msghdr message { };
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    int file = descriptor.value();
    std::memcpy(CMSG_DATA(header), &file, sizeof(file));
    Check(sendmsg(sockets[1], &message, 0) == sizeof(originalArea), "transfer a production shared-memory handle to another process");
    std::array<int, 4> results { };
    Check(Readable(sockets[1]) && recv(sockets[1], results.data(), sizeof(results), 0) == sizeof(results), "child returns its actual memory-access results");
    const char* labels[] = { "received memory descriptor is close-on-exec", "child maps the production handle and reads the owner's bytes",
        "child cannot write through the read-only capability", "child cannot bypass descriptor access by cloning the owner's area" };
    for (size_t i = 0; i < results.size(); ++i) Check(results[i], labels[i]);
    close(sockets[1]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child.process, &status, 0); } while (waited < 0 && errno == EINTR);
    Check(waited == child.process && WIFEXITED(status) && !WEXITSTATUS(status), "reap the memory consumer after its checks");
}

int main(int argc, char** argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "7388001") && !std::strcmp(argv[2], "3")) return Child();
    BApplication app("application/x-vnd.Kunanyi-Summit-MemoryTest");
    WTF::initializeMainThread();
    auto one = SharedMemory::allocate(1);
    Check(one && one->size() == 1 && one->mutableSpan().data(), "allocate a single byte without page alignment");
    if (!one) return 1;
    Check(!SharedMemory::allocate(0), "reject empty allocation");
    Check(!SharedMemory::allocate(std::numeric_limits<size_t>::max()), "reject an overflowing allocation size");
    auto memory = SharedMemory::allocate(5003);
    Check(memory && memory->size() == 5003 && memory->mutableSpan().data(), "preserve an unaligned allocation's exact logical size");
    if (!memory) return 1;
    auto bytes = memory->mutableSpan();
    Check(bytes.front() == 0 && bytes.back() == 0, "new shared memory is zero initialized");
    std::memcpy(bytes.data(), "original", 9);
    bytes.back() = 42;
    auto writable = memory->createHandle(Protection::ReadWrite);
    Check(writable.has_value(), "create a writable owned memory handle");
    if (!writable) return 1;
    SharedMemory::Handle duplicate(*writable);
    memory = nullptr;
    auto first = SharedMemory::map(WTF::move(*writable), Protection::ReadWrite);
    Check(first && !std::memcmp(first->span().data(), "original", 9) && first->span().back() == 42,
        "handle keeps bytes alive after the original allocation is destroyed");
    if (!first) return 1;
    auto second = SharedMemory::map(WTF::move(duplicate), Protection::ReadWrite);
    Check(second && second->span().data() != first->span().data(), "map a copied handle to an independent virtual address");
    if (!second) return 1;
    second->mutableSpan()[0] = 'X';
    Check(first->span()[0] == 'X', "separate mappings observe shared writes");
    second = nullptr;
    Check(first->span()[0] == 'X' && first->span().back() == 42, "destroying one mapping preserves the other mapping");

    auto protectedMemory = SharedMemory::allocate(4096);
    if (!protectedMemory) return 1;
    protectedMemory->mutableSpan()[0] = 7;
    area_info info;
    Check(get_area_info(area_for(protectedMemory->mutableSpan().data()), &info) == B_OK && !(info.protection & B_CLONEABLE_AREA),
        "ordinary memory is not globally cloneable by another team");
    auto readOnly = protectedMemory->createHandle(Protection::ReadOnly);
    Check(readOnly.has_value(), "create a read-only memory capability");
    if (!readOnly) return 1;
    SharedMemory::Handle rejectedWrite(*readOnly);
    Check(!SharedMemory::map(WTF::move(rejectedWrite), Protection::ReadWrite), "read-only handle cannot create a writable shared mapping");
    SharedMemory::Handle flagHandle(*readOnly);
    auto descriptor = flagHandle.releaseHandle();
    Check((fcntl(descriptor.value(), F_GETFL) & O_ACCMODE) == O_RDONLY, "read-only handle contains a read-only file descriptor");
    Check((fcntl(descriptor.value(), F_GETFD) & FD_CLOEXEC) != 0, "copied handle retains close-on-exec");
    Check(pwrite(descriptor.value(), "X", 1, 0) == -1, "read-only descriptor rejects direct writes");
    SharedMemory::Handle oversized(descriptor.duplicate(), 4097);
    Check(!SharedMemory::map(WTF::move(oversized), Protection::ReadOnly), "reject handle size beyond its backing object");
    SharedMemory::Handle privateHandle(*readOnly);
    auto readMapping = SharedMemory::map(WTF::move(*readOnly), Protection::ReadOnly);
    Check(readMapping && readMapping->span()[0] == 7, "read-only mapping contains the shared data");
    Check(readMapping && !readMapping->createHandle(Protection::ReadWrite), "read-only mapping cannot export a writable capability");
    if (!readMapping) return 1;
    Check(get_area_info(area_for(readMapping->mutableSpan().data()), &info) == B_OK && !(info.protection & B_WRITE_AREA),
        "native virtual memory enforces the read-only mapping");
    protectedMemory->mutableSpan()[0] = 8;
    Check(readMapping->span()[0] == 8, "read-only mapping observes the owner's later writes");
    TestChild(argv[0], *protectedMemory);

    auto privateMapping = SharedMemory::map(WTF::move(privateHandle), Protection::ReadWrite, SharedMemory::CopyOnWrite::Yes);
    Check(privateMapping && privateMapping->span()[0] == 8, "create a writable private mapping from a read-only capability");
    if (!privateMapping) return 1;
    privateMapping->mutableSpan()[0] = 99;
    Check(protectedMemory->span()[0] == 8, "private writes do not change shared bytes");
    auto privateExport = privateMapping->createHandle(Protection::ReadOnly);
    auto privateSnapshot = privateExport ? SharedMemory::map(WTF::move(*privateExport), Protection::ReadOnly) : nullptr;
    Check(privateSnapshot && privateSnapshot->span()[0] == 99, "export private mapping's actual bytes");

    std::array<uint8_t, 5> content { 1, 3, 5, 7, 9 };
    auto copied = SharedMemory::Handle::createCopy(content, Protection::ReadOnly);
    auto copiedMapping = copied ? SharedMemory::map(WTF::move(*copied), Protection::ReadOnly) : nullptr;
    Check(copiedMapping && copiedMapping->size() == content.size() && !std::memcmp(copiedMapping->span().data(), content.data(), content.size()),
        "generic createCopy retains data after its temporary allocation is destroyed");

    void* raw = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, descriptor.value(), 0);
    auto wrapped = raw == MAP_FAILED ? nullptr : SharedMemory::wrapMap(raw, 4096, descriptor.value());
    Check(wrapped && wrapped->span()[0] == 8, "wrap an existing mapping without taking ownership");
    wrapped = nullptr;
    Check(raw != MAP_FAILED && fcntl(descriptor.value(), F_GETFD) >= 0 && static_cast<uint8_t*>(raw)[0] == 8,
        "destroying a wrapper preserves its caller's descriptor and mapping");
    if (raw != MAP_FAILED) munmap(raw, 4096);

    auto bitmapMemory = SharedMemory::allocate(4 * 4 * 4);
    if (!bitmapMemory) return 1;
    area_id bitmapArea = bitmapMemory->area();
    Check(bitmapArea >= 0, "explicit bitmap transport exposes a native shared area");
    {
        BBitmap bitmap(bitmapArea, 0, BRect(0, 0, 3, 3), 0, B_RGBA32, 16);
        Check(bitmapArea >= 0 && bitmap.InitCheck() == B_OK, "native app_server imports the file-backed bitmap area");
        if (bitmapArea >= 0 && bitmap.InitCheck() == B_OK) {
            static_cast<uint8_t*>(bitmap.Bits())[0] = 123;
            Check(bitmapMemory->span()[0] == 123, "native bitmap and WebCore share the same pixel bytes");
        }
    }
    Check(bitmapMemory->span()[0] == 123, "destroying a native bitmap preserves its backing memory");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
