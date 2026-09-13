#include "HaikuProcess.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

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
    pollfd descriptor { socket, POLLIN, 0 };
    int result;
    do { result = poll(&descriptor, 1, 5000); } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & POLLIN);
}

static int Child(int argc, char** argv)
{
    std::array<int, 6> results { };
    results[0] = argc == 4 && !std::strcmp(argv[2], "3") && !std::strcmp(argv[3], "argument with spaces and 雪");
    results[1] = (fcntl(3, F_GETFD) & FD_CLOEXEC) == 0;
    sigset_t mask;
    sigprocmask(SIG_SETMASK, nullptr, &mask);
    results[2] = !sigismember(&mask, SIGTERM);
    char packet[16];
    bool boundaries = Readable(3) && recv(3, packet, sizeof(packet), 0) == 3 && !std::memcmp(packet, "one", 3);
    boundaries &= Readable(3) && recv(3, packet, sizeof(packet), 0) == 3 && !std::memcmp(packet, "two", 3);
    results[3] = boundaries;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
    iovec vector { packet, sizeof(packet) };
    msghdr message { };
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (Readable(3) && recvmsg(3, &message, MSG_CMSG_CLOEXEC) == 4 && !(message.msg_flags & MSG_CTRUNC)) {
        auto* header = CMSG_FIRSTHDR(&message);
        if (header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS && header->cmsg_len == CMSG_LEN(sizeof(int))) {
            int descriptor;
            std::memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
            results[4] = (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0;
            void* memory = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
            if (memory != MAP_FAILED) {
                results[5] = !std::memcmp(memory, "from parent", 12);
                std::memcpy(memory, "from child", 11);
                munmap(memory, 4096);
            }
            close(descriptor);
        }
    }
    return send(3, results.data(), sizeof(results), 0) == sizeof(results) ? 0 : 1;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "4277001")) return Child(argc, argv);
    char* executable = realpath(argv[0], nullptr);
    if (!executable) return 1;
    int sockets[2];
    Check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) == 0, "native sequenced-packet socket pair");
    if (failures) { free(executable); return 1; }
    fcntl(sockets[0], F_SETFD, FD_CLOEXEC);
    fcntl(sockets[1], F_SETFD, FD_CLOEXEC);
    sigset_t blocked, previous;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigprocmask(SIG_BLOCK, &blocked, &previous);
    auto child = WebKit::spawnHaikuProcess(executable, 4277001, sockets[0], { "argument with spaces and 雪" });
    sigprocmask(SIG_SETMASK, &previous, nullptr);
    Check(!child.error && child.process > 0, "spawn a native child with an inherited IPC endpoint");
    Check((fcntl(sockets[0], F_GETFD) & FD_CLOEXEC) != 0, "launch preserves the parent's descriptor flags");
    if (child.error) { free(executable); close(sockets[0]); close(sockets[1]); return 1; }
    close(sockets[0]);
    send(sockets[1], "one", 3, 0);
    send(sockets[1], "two", 3, 0);

    char filename[] = "/tmp/summit-ipc-XXXXXX";
    int file = mkstemp(filename);
    if (file >= 0) unlink(filename);
    bool sized = file >= 0 && ftruncate(file, 4096) == 0;
    void* shared = sized ? mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0) : MAP_FAILED;
    Check(shared != MAP_FAILED, "create an unlinked shared mapping for descriptor transfer");
    if (shared != MAP_FAILED) {
        std::memcpy(shared, "from parent", 12);
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] { };
        char payload[] = "file";
        iovec vector { payload, 4 };
        msghdr message { };
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &file, sizeof(file));
        Check(sendmsg(sockets[1], &message, 0) == 4, "send a real file descriptor over the process IPC channel");
        std::array<int, 6> results { };
        Check(Readable(sockets[1]) && recv(sockets[1], results.data(), sizeof(results), 0) == sizeof(results), "child returns a complete IPC response");
        const char* labels[] = { "child receives exact identifiers and Unicode arguments", "child IPC endpoint survives exec", "child does not inherit a blocked termination signal", "packet boundaries survive IPC", "received descriptor has close-on-exec protection", "child maps and reads the transferred file" };
        for (size_t i = 0; i < results.size(); ++i) Check(results[i], labels[i]);
        Check(!std::memcmp(shared, "from child", 11), "parent sees writes through the child's shared mapping");
        munmap(shared, 4096);
    }
    if (file >= 0) close(file);
    close(sockets[1]);
    int status = 0;
    while (waitpid(child.process, &status, 0) < 0 && errno == EINTR) { }
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exits and is reaped after IPC completes");
    Check(WebKit::spawnHaikuProcess("relative-path", 1, -1).error == EINVAL, "relative auxiliary executable path is rejected");
    Check(WebKit::spawnHaikuProcess(executable, 0, -1).error == EINVAL, "zero process identifier is rejected");
    Check(WebKit::spawnHaikuProcess(executable, 1, -1).error == EBADF, "invalid IPC descriptor is rejected");
    int descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
    auto missing = WebKit::spawnHaikuProcess("/boot/home/summit-no-such-auxiliary-process", 1, descriptor);
    Check(missing.error == ENOENT && !missing.process, "failed exec returns an error without a live child identifier");
    close(descriptor);
    free(executable);
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
