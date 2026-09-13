#include "config.h"
#include "SocketMonitorHaiku.h"
#include <wtf/MainThread.h>
#include <wtf/WorkQueue.h>
#include <wtf/threads/BinarySemaphore.h>
#include <wtf/unix/UnixFileDescriptor.h>
#include <OS.h>
#include <atomic>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

static int checks = 0, failures = 0;
static void Check(bool result, const char* label)
{
    ++checks;
    failures += !result;
    std::printf("%s %s\n", result ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

struct Pair {
    Pair()
    {
        int descriptors[2];
        RELEASE_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, descriptors) == 0);
        reader = UnixFileDescriptor { descriptors[0], UnixFileDescriptor::Adopt };
        writer = UnixFileDescriptor { descriptors[1], UnixFileDescriptor::Adopt };
        RELEASE_ASSERT(setNonBlock(reader.value()));
    }
    UnixFileDescriptor reader, writer;
};

static int OpenDescriptors()
{
    int count = 0;
    for (int fd = 0; fd < 2048; ++fd)
        count += fcntl(fd, F_GETFD) >= 0;
    return count;
}

int main()
{
    WTF::initializeMainThread();
    auto queue = WorkQueue::create("Summit IPC monitor test"_s);
    std::unique_ptr<IPC::SocketMonitorHaiku> monitor;
    Pair pair;
    BinarySemaphore received;
    int sequence = 0;
    bool ordered = true, onQueue = true;
    queue->dispatchSync([&] {
        std::array<bool, 2048> previouslyOpen;
        for (size_t fd = 0; fd < previouslyOpen.size(); ++fd)
            previouslyOpen[fd] = fcntl(fd, F_GETFD) >= 0;
        monitor = IPC::SocketMonitorHaiku::create(pair.reader.value(), queue, [&] {
            onQueue &= queue->isCurrent();
            int value = -1;
            auto bytes = recv(pair.reader.value(), &value, sizeof(value), 0);
            ordered &= bytes == sizeof(value) && value == sequence;
            if (++sequence == 32)
                received.signal();
        });
        bool closeOnExec = true;
        for (size_t fd = 0; fd < previouslyOpen.size(); ++fd) {
            int flags = fcntl(fd, F_GETFD);
            if (!previouslyOpen[fd] && flags >= 0)
                closeOnExec &= flags & FD_CLOEXEC;
        }
        Check(closeOnExec, "monitor descriptors cannot leak into an executed child");
    });
    Check(!!monitor, "create a native socket monitor");
    snooze(30000);
    Check(sequence == 0, "idle socket does not schedule callbacks");
    for (int i = 0; i < 32; ++i)
        RELEASE_ASSERT(send(pair.writer.value(), &i, sizeof(i), 0) == sizeof(i));
    Check(received.waitFor(3_s), "socket arrivals wake the queue after its initial dispatch");
    queue->dispatchSync([&] {
        Check(sequence == 32 && ordered, "sequenced packets arrive exactly once and in order");
        Check(onQueue, "all callbacks execute on the serial connection queue");
        auto start = system_time();
        monitor->stop();
        monitor->stop();
        Check(system_time() - start < 1000000, "idle stop wakes and joins the polling thread promptly");
        monitor.reset();
    });
    int value = 99, actual = 0;
    Check(send(pair.writer.value(), &value, sizeof(value), 0) == sizeof(value)
        && recv(pair.reader.value(), &actual, sizeof(actual), 0) == sizeof(actual) && actual == value,
        "monitor destruction preserves the caller's socket");

    int cancelledCalls = 0;
    std::atomic<int> capturedDestroyed { 0 };
    auto captured = std::shared_ptr<int>(new int(1), [&](int* p) { delete p; ++capturedDestroyed; });
    queue->dispatchSync([&] {
        monitor = IPC::SocketMonitorHaiku::create(pair.reader.value(), queue,
            [&, captured] { ++cancelledCalls; });
    });
    captured.reset();
    BinarySemaphore blocked, unblock;
    queue->dispatch([&] {
        blocked.signal();
        unblock.wait();
        monitor.reset();
    });
    RELEASE_ASSERT(blocked.waitFor(2_s));
    RELEASE_ASSERT(send(pair.writer.value(), &value, sizeof(value), 0) == sizeof(value));
    snooze(100000); // Permit readiness to queue while its dispatcher is occupied.
    unblock.signal();
    queue->dispatchSync([] { });
    Check(cancelledCalls == 0, "invalidation suppresses an already queued callback");
    Check(capturedDestroyed == 1, "cancelled callback releases its captured owner");
    Check(recv(pair.reader.value(), &actual, sizeof(actual), 0) == sizeof(actual),
        "the polling thread never consumes protocol bytes");

    BinarySemaphore closed;
    bool sawEOF = false;
    queue->dispatchSync([&] {
        monitor = IPC::SocketMonitorHaiku::create(pair.reader.value(), queue, [&] {
            char byte;
            sawEOF = recv(pair.reader.value(), &byte, 1, 0) == 0;
            monitor.reset();
            closed.signal();
        });
    });
    pair.writer = { };
    Check(closed.waitFor(2_s), "peer closure wakes the connection queue");
    queue->dispatchSync([&] {
        Check(sawEOF && !monitor, "a callback can destroy its own monitor without deadlocking");
        Check(!IPC::SocketMonitorHaiku::create(-1, queue, [] { }), "invalid descriptors report creation failure");
    });

    Pair oldPair, replacement;
    auto oldReader = oldPair.reader.duplicate();
    BinarySemaphore originalArrived;
    std::atomic<int> reuseCalls { 0 };
    queue->dispatchSync([&] {
        monitor = IPC::SocketMonitorHaiku::create(oldPair.reader.value(), queue, [&] {
            int item;
            if (recv(oldReader.value(), &item, sizeof(item), 0) == sizeof(item)) {
                ++reuseCalls;
                originalArrived.signal();
            }
        });
    });
    const int reused = oldPair.reader.release();
    close(reused);
    RELEASE_ASSERT(dup2(replacement.reader.value(), reused) == reused);
    UnixFileDescriptor replacementAlias { reused, UnixFileDescriptor::Adopt };
    RELEASE_ASSERT(send(replacement.writer.value(), &value, sizeof(value), 0) == sizeof(value));
    snooze(50000);
    Check(reuseCalls == 0, "descriptor reuse cannot redirect the monitor to an unrelated socket");
    RELEASE_ASSERT(send(oldPair.writer.value(), &value, sizeof(value), 0) == sizeof(value));
    Check(originalArrived.waitFor(2_s), "the monitor retains the original socket after descriptor reuse");
    queue->dispatchSync([&] { monitor.reset(); });
    Check(recv(replacementAlias.value(), &actual, sizeof(actual), 0) == sizeof(actual),
        "cancellation does not close or drain a reused descriptor");

    int before = OpenDescriptors();
    for (int i = 0; i < 40; ++i) {
        queue->dispatchSync([&] {
            auto transient = IPC::SocketMonitorHaiku::create(oldReader.value(), queue, [] { });
            RELEASE_ASSERT(transient);
        });
    }
    queue->dispatchSync([] { });
    Check(OpenDescriptors() == before, "repeated creation and cancellation do not leak descriptors");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
