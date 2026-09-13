#include "config.h"
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#include <wtf/WTFProcess.h>
#include <wtf/threads/BinarySemaphore.h>
#include <Application.h>
#include <OS.h>
#include <cstdio>
#include <cstring>
#include <memory>

static int checks = 0, failures = 0;
static void Check(bool passed, const char* label)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static int ConsoleChecks()
{
    using namespace WTF;
    auto& loop = RunLoop::currentSingleton();
    const auto thread = find_thread(nullptr);
    int fired = 0;
    RunLoop::Timer immediate(loop, "immediate test"_s, [&] {
        ++fired;
        Check(find_thread(nullptr) == thread, "timer runs on the thread that owns its run loop");
        loop.stop();
        loop.stop();
    });
    immediate.startOneShot(0_s);
    Check(immediate.isActive(), "zero-delay timer is active before entering the run loop");
    RunLoop::run();
    Check(fired == 1 && !immediate.isActive(), "stopping a standalone loop returns without terminating its thread");

    int cancelled = 0;
    auto obsolete = std::make_unique<RunLoop::Timer>(loop, "cancelled test"_s, [&] { ++cancelled; });
    obsolete->startOneShot(0_s);
    snooze(20000); // Leave a native timer notification queued before deletion.
    obsolete.reset();
    RunLoop::Timer repeating(loop, "repeating test"_s, [&] {
        if (++fired == 4)
            loop.stop();
    });
    repeating.startRepeating(10_ms);
    Check(repeating.secondsUntilFire() > 0_s && repeating.secondsUntilFire() <= 10_ms, "timer reports its remaining interval");
    RunLoop::run();
    repeating.stop();
    Check(fired == 4, "a stopped run loop can run again and deliver repeating timers");
    Check(cancelled == 0, "queued notifications cannot invoke a destroyed timer");

    int replaced = 0;
    RunLoop::Timer rearmed(loop, "rearmed test"_s, [&] { ++replaced; loop.stop(); });
    rearmed.startOneShot(0_s);
    snooze(20000);
    rearmed.startOneShot(30_ms);
    const auto started = system_time();
    RunLoop::run();
    Check(replaced == 1 && system_time() - started >= 20000, "rearming a timer invalidates its earlier queued notification");

    RefPtr<RunLoop> worker;
    BinarySemaphore ready;
    auto workerThread = Thread::create("Summit run-loop test"_s, [&] {
        worker = &RunLoop::currentSingleton();
        ready.signal();
        RunLoop::run();
    });
    ready.wait();
    bool workerRan = false;
    worker->dispatch([&] {
        workerRan = find_thread(nullptr) != thread;
        loop.dispatch([&] { loop.stop(); });
    });
    RunLoop::run();
    Check(workerRan, "cross-thread dispatch wakes an independent worker run loop");
    worker->dispatch([worker] { worker->stop(); });
    Check(workerThread->waitForCompletion() == 0, "worker thread exits cleanly after its run loop stops");
    return failures ? 1 : 0;
}

class NativeTestApp final : public BApplication {
public:
    NativeTestApp() : BApplication("application/x-vnd.Kunanyi-Summit-run-loop-tests") { }
    void ReadyToRun() override
    {
        WTF::initializeMainThread();
        WTF::RunLoop::run();
        timer = std::make_unique<WTF::RunLoop::Timer>(WTF::RunLoop::mainSingleton(), "native timer test"_s, [&] {
            Check(find_thread(nullptr) == Thread(), "native timer runs on the application thread");
            Check(!timer->isActive(), "native one-shot timer is inactive before its callback");
            PostMessage(B_QUIT_REQUESTED);
        });
        timer->startOneShot(0_s);
        Check(timer->isActive(), "native application accepts a zero-delay timer");
    }
    std::unique_ptr<WTF::RunLoop::Timer> timer;
};

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "--exit-with-worker")) {
        WTF::initializeMainThread();
        BinarySemaphore ready;
        WTF::Thread::create("Summit exit test"_s, [&] {
            auto& loop = WTF::RunLoop::currentSingleton();
            loop.dispatch([&] { ready.signal(); loop.stop(); });
            WTF::RunLoop::run();
        })->detach();
        ready.wait();
        std::printf("Explicit exit while worker thread finishes\n");
        WTF::exitProcess(0);
    }
    if (argc > 1 && !std::strcmp(argv[1], "--native")) {
        NativeTestApp app;
        app.Run();
    } else {
        WTF::initializeMainThread();
        ConsoleChecks();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
