// Exercise the production launcher while libroot's heap is active on other
// threads. A raw fork can otherwise leave execve waiting on an inherited lock.
#define main EngineProcessMain
#include "EngineProcessTests.cpp"
#undef main

#include <OS.h>
#include <atomic>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    if (argc > 1)
        return EngineProcessMain(argc, argv);
    // A failed launch must fail the test instead of waiting for a crash dialog.
    disable_debugger(1);
    std::atomic<bool> running { true };
    std::vector<std::thread> allocators;
    for (int i = 0; i < 12; ++i) {
        allocators.emplace_back([&, i] {
            std::vector<void*> allocations;
            while (running.load()) {
                for (int j = 0; j < 100; ++j) {
                    void* memory = std::malloc(31 + ((j + 1) * (i + 1) * 173) % 40000);
                    if (memory) {
                        static_cast<volatile char*>(memory)[0] = j;
                        allocations.push_back(memory);
                    }
                }
                for (void* memory : allocations)
                    std::free(memory);
                allocations.clear();
            }
        });
    }
    int result = 0;
    for (int round = 0; round < 50; ++round) {
        std::printf("BEGIN_LAUNCH_ROUND %d\n", round + 1);
        std::fflush(stdout);
        if (EngineProcessMain(argc, argv)) {
            result = 1;
            break;
        }
        std::printf("END_LAUNCH_ROUND %d\n", round + 1);
        std::fflush(stdout);
    }
    running = false;
    for (auto& thread : allocators)
        thread.join();
    return result;
}
