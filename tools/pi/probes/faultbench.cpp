// faultbench FILE : per-page fault cost for anonymous zero-fill, file read
// (page cache) and file copy-on-write mappings.
#include <OS.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
static uint64 faults() { system_info s; get_system_info(&s); return s.page_faults; }
static void report(const char* what, bigtime_t t, uint64 f, size_t pages)
{
    std::printf("%-22s %6zu pages %8.1f ms  %6.1f us/page  faults %llu\n", what, pages, t / 1e3,
        (double)t / pages, (unsigned long long)f);
}
int main(int argc, char** argv)
{
    const size_t size = 64 << 20; const size_t pages = size / B_PAGE_SIZE;
    {
        void* a = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64 f0 = faults(); bigtime_t t0 = system_time();
        for (size_t i = 0; i < pages; i++) ((volatile char*)a)[i * B_PAGE_SIZE] = 1;
        report("anon write", system_time() - t0, faults() - f0, pages);
        munmap(a, size);
    }
    if (argc < 2) return 0;
    int fd = open(argv[1], O_RDONLY);
    off_t len = lseek(fd, 0, SEEK_END); size_t fp = len / B_PAGE_SIZE;
    if (fp > 8192) fp = 8192;
    {
        void* m = mmap(nullptr, fp * B_PAGE_SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
        uint64 f0 = faults(); bigtime_t t0 = system_time(); unsigned sum = 0;
        for (size_t i = 0; i < fp; i++) sum += ((volatile unsigned char*)m)[i * B_PAGE_SIZE];
        report("file read", system_time() - t0, faults() - f0, fp);
        munmap(m, fp * B_PAGE_SIZE);
    }
    {
        void* m = mmap(nullptr, fp * B_PAGE_SIZE, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
        uint64 f0 = faults(); bigtime_t t0 = system_time(); unsigned sum = 0;
        for (size_t i = 0; i < fp; i++) sum += ((volatile unsigned char*)m)[i * B_PAGE_SIZE];
        report("file read (exec map)", system_time() - t0, faults() - f0, fp);
        munmap(m, fp * B_PAGE_SIZE);
    }
    {
        void* m = mmap(nullptr, fp * B_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        uint64 f0 = faults(); bigtime_t t0 = system_time();
        for (size_t i = 0; i < fp; i++) ((volatile unsigned char*)m)[i * B_PAGE_SIZE] += 1;
        report("file copy-on-write", system_time() - t0, faults() - f0, fp);
        munmap(m, fp * B_PAGE_SIZE);
    }
    return 0;
}
