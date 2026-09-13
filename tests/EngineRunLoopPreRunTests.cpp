#include "config.h"
#include <wtf/MainThread.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#include <Application.h>
#include <MessageRunner.h>
#include <OS.h>
#include <cstdio>
#include <memory>

using namespace WTF;

static unsigned checks;
static unsigned failures;
static constexpr uint32 watchdogMessage = 'rlwd';
static constexpr uint32 workerDoneMessage = 'rlwk';

static void check(bool condition, const char* description)
{
    ++checks;
    failures += !condition;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", description);
    std::fflush(stdout);
}

static bool exerciseWorkerLoop()
{
    auto thread = find_thread(nullptr);
    auto& loop = RunLoop::currentSingleton();
    bool dispatchedOnWorker = false;
    bool timerOnWorker = false;
    loop.dispatch([&] { dispatchedOnWorker = find_thread(nullptr) == thread; });
    RunLoop::Timer timer(loop, "pre-Run worker regression"_s, [&] {
        timerOnWorker = find_thread(nullptr) == thread;
        loop.stop();
    });
    timer.startOneShot(1_ms);
    RunLoop::run();
    return dispatchedOnWorker && timerOnWorker;
}

class PreRunTestApplication final : public BApplication {
public:
    PreRunTestApplication()
        : BApplication("application/x-vnd.Summit-RunLoopPreRunTests")
    {
    }

    void ReadyToRun() override
    {
        readyToRun = true;
        // Owning the lock of an already-running application must not make
        // that application the worker thread's WTF event loop.
        worker = WTF::Thread::create("RunLoop locked application worker"_s, [&] {
            if (Lock()) {
                runningApplicationWorkerPassed = exerciseWorkerLoop();
                Unlock();
            }
            PostMessage(workerDoneMessage);
        });
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == workerDoneMessage) {
            workerJoined = worker && !worker->waitForCompletion();
            maybeQuit();
            return;
        }
        if (message->what == watchdogMessage) {
            watchdogExpired = true;
            Quit();
            return;
        }
        BApplication::MessageReceived(message);
    }

    void maybeQuit()
    {
        if (dispatchCount && timerCount && workerJoined)
            Quit();
    }

    RefPtr<WTF::Thread> worker;
    unsigned dispatchCount { 0 };
    unsigned timerCount { 0 };
    bool dispatchOnMain { false };
    bool timerOnMain { false };
    bool readyToRun { false };
    bool workerJoined { false };
    bool runningApplicationWorkerPassed { false };
    bool watchdogExpired { false };
};

int main()
{
    PreRunTestApplication application;
    auto thread = find_thread(nullptr);
    check(application.InitCheck() == B_OK, "native application initializes");
    check(application.Thread() < B_OK, "application has no registered thread before Run");
    check(application.LockingThread() == thread, "creating thread owns application's initial lock");
    check(!BLooper::LooperForThread(thread), "LooperForThread cannot discover the pre-Run application");
    auto originalHandlerCount = application.CountHandlers();

    // This is the actual WTF initializer called by InitializeWebKit2 before
    // AuxiliaryProcessMainBase enters WebProcessMainHaiku's BApplication::Run.
    WTF::initializeMainThread();
    auto& loop = RunLoop::mainSingleton();
    check(application.CountHandlers() == originalHandlerCount + 1, "WTF handler attaches to the pre-Run application");
    loop.dispatch([&] {
        ++application.dispatchCount;
        application.dispatchOnMain = find_thread(nullptr) == thread;
        application.maybeQuit();
    });
    auto timer = std::make_unique<RunLoop::Timer>(loop, "pre-Run application timer"_s, [&] {
        ++application.timerCount;
        application.timerOnMain = find_thread(nullptr) == thread;
        application.maybeQuit();
    });
    timer->startOneShot(1_ms);
    check(timer->isActive(), "application timer starts before BApplication::Run");

    bool preRunWorkerPassed = false;
    auto worker = WTF::Thread::create("RunLoop pre-Run worker"_s, [&] {
        preRunWorkerPassed = exerciseWorkerLoop();
    });
    check(!worker->waitForCompletion() && preRunWorkerPassed, "worker stays independent while main owns the pre-Run application");

    // A native watchdog still runs in the baseline, even when WTF accidentally
    // created an undriven standalone looper. It turns that regression into
    // explicit failed assertions rather than a hung test process.
    BMessage watchdog(watchdogMessage);
    auto watchdogRunner = std::make_unique<BMessageRunner>(BMessenger(&application), &watchdog, 3000000, 1);
    check(watchdogRunner->InitCheck() == B_OK, "native watchdog is available independently of WTF");
    application.Run();
    watchdogRunner.reset();
    if (application.worker && !application.workerJoined)
        application.workerJoined = !application.worker->waitForCompletion();

    check(application.readyToRun, "BApplication::Run processes its native startup event");
    check(application.dispatchCount == 1 && application.dispatchOnMain, "pre-Run dispatch executes once on the application thread");
    check(application.timerCount == 1 && application.timerOnMain, "pre-Run timer fires once through BApplication::Run");
    check(application.workerJoined && application.runningApplicationWorkerPassed, "worker holding a running application's lock keeps its own loop");
    check(!application.watchdogExpired, "WTF work completes before the native watchdog");

    timer.reset();
    loop.stop();
    check(application.CountHandlers() == originalHandlerCount, "WTF teardown removes its handler");
    check(BMessenger(&application).IsValid(), "WTF teardown preserves the native application looper");
    std::printf("PRERUN_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
