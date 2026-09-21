// Measures how quickly a run loop can arm, fire and cancel timers. Linked twice by
// tools/bench-engine-runloop-timers.py: against the library's run loop and against
// the working-tree RunLoopHaiku.cpp.
#include "config.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <Application.h>
#include <cstdio>
#include <memory>

using namespace WTF;

static double chain(RunLoop& loop, int count, Seconds delay)
{
    int fired = 0;
    std::unique_ptr<RunLoop::Timer> timer;
    timer = std::make_unique<RunLoop::Timer>(loop, "bench"_s, [&] {
        if (++fired == count) {
            loop.stop();
            return;
        }
        timer->startOneShot(delay);
    });
    auto start = MonotonicTime::now();
    timer->startOneShot(delay);
    RunLoop::run();
    return (MonotonicTime::now() - start).milliseconds();
}

static double armAndCancel(RunLoop& loop, int count)
{
    RunLoop::Timer timer(loop, "bench cancel"_s, [] { });
    auto start = MonotonicTime::now();
    for (int i = 0; i < count; ++i) {
        timer.startOneShot(10_s);
        timer.stop();
    }
    return (MonotonicTime::now() - start).milliseconds();
}

int main()
{
    // No BApplication: both variants then drive a standalone run loop on this thread.
    WTF::initializeMainThread();
    auto& loop = RunLoop::currentSingleton();
    std::printf("zero-delay chain x2000: %.1f ms\n", chain(loop, 2000, 0_s));
    std::fflush(stdout);
    std::printf("1 ms chain x500: %.1f ms (ideal 500)\n", chain(loop, 500, 1_ms));
    std::printf("arm+cancel x2000: %.1f ms\n", armAndCancel(loop, 2000));
    return 0;
}
