// Real libroot locks and libbe loopers under process-wide child-exit signals.
#include <Application.h>
#include <Handler.h>
#include <Looper.h>
#include <OS.h>
#include <locks.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

static unsigned checks;
static void Check(bool value, const char* label)
{
    std::printf("%s %s\n", value ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    ++checks;
    if (!value)
        _Exit(1);
}

static void FinishWithSignals(std::vector<std::thread>& threads, std::atomic<unsigned>& finished)
{
    const auto deadline = system_time() + 10000000;
    unsigned signals = 0;
    while (finished.load() != threads.size() && system_time() < deadline) {
        if (kill(getpid(), SIGCHLD) != 0)
            Check(false, "deliver process-wide child-exit signal");
        ++signals;
        snooze(100);
    }
    Check(signals > 0, "concurrent operations overlap signal delivery");
    Check(finished.load() == threads.size(), "all interrupted lock users finish before the deadline");
    for (auto& thread : threads)
        thread.join();
}

int main(int argc, char** argv)
{
    Check(argc == 2, "expected WebKit provider path is supplied");
    for (const char* symbol : { "__rw_lock_read_lock", "__rw_lock_write_lock" }) {
        Dl_info provider {};
        auto* address = dlsym(RTLD_DEFAULT, symbol);
        char* path = address && dladdr(address, &provider) && provider.dli_fname
            ? realpath(provider.dli_fname, nullptr) : nullptr;
        std::printf("PROVIDER %s %s\n", symbol, path ? path : "unresolved");
        bool matches = path && !std::strcmp(path, argv[1]);
        std::free(path);
        Check(matches, "native lock acquisition resolves to the tested WebKit library");
    }
    status_t status;
    BApplication application("application/x-vnd.Kunanyi-Summit-native-lock-tests", &status);
    Check(status == B_OK, "initialize native lock integration test");
    rw_lock lock;
    rw_lock_init(&lock, "Summit native rw_lock regression");
    Check(rw_lock_write_lock(&lock) == B_OK, "acquire native writer");
    Check(rw_lock_write_lock(&lock) == B_OK, "native writer remains recursive");
    Check(rw_lock_read_lock(&lock) == B_OK, "writer can enter a nested native read lock");
    Check(rw_lock_read_unlock(&lock) == B_OK && rw_lock_write_unlock(&lock) == B_OK
        && rw_lock_write_unlock(&lock) == B_OK, "nested native acquisitions release in order");

    uint64 value = 0, mirror = 0;
    std::atomic<bool> start { false }, valid { true };
    std::atomic<unsigned> finished { 0 };
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 8; ++index) {
        workers.emplace_back([&, index] {
            while (!start.load()) snooze(1000);
            for (unsigned count = 0; count < 2000; ++count) {
                if (index < 4) {
                    if (rw_lock_write_lock(&lock) != B_OK) { valid = false; break; }
                    ++value;
                    mirror = value * 17;
                    if (rw_lock_write_unlock(&lock) != B_OK) { valid = false; break; }
                } else {
                    if (rw_lock_read_lock(&lock) != B_OK) { valid = false; break; }
                    if (mirror != value * 17) valid = false;
                    if (rw_lock_read_unlock(&lock) != B_OK) { valid = false; break; }
                }
            }
            ++finished;
        });
    }
    start = true;
    FinishWithSignals(workers, finished);
    Check(valid.load() && value == 8000 && mirror == value * 17,
        "mixed readers and writers preserve every protected update under signals");
    rw_lock_destroy(&lock);

    workers.clear();
    start = false;
    finished = 0;
    valid = true;
    for (unsigned index = 0; index < 8; ++index) {
        workers.emplace_back([&] {
            while (!start.load()) snooze(1000);
            for (unsigned count = 0; count < 100; ++count) {
                auto* looper = new BLooper("Summit interrupted native looper");
                BHandler handler("native lock handler");
                looper->AddHandler(&handler);
                if (handler.Looper() != looper || !looper->RemoveHandler(&handler)) valid = false;
                looper->Unlock();
                if (!looper->Lock()) { valid = false; break; }
                looper->Quit();
            }
            ++finished;
        });
    }
    start = true;
    FinishWithSignals(workers, finished);
    Check(valid.load(), "800 real looper and handler lifecycles survive signal interruptions");
    std::printf("NATIVE_LOCK_RESULT PASS checks=%u\n", checks);
    return 0;
}
