// dltime LIB... : dlopen()s each library in order (RTLD_NOW|RTLD_GLOBAL) and
// prints wall time, this team's user/kernel time and system page faults per load.
#include <OS.h>
#include <dlfcn.h>
#include <cstdio>
static void usage(bigtime_t& u, bigtime_t& k)
{
    team_usage_info info;
    get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &info);
    u = info.user_time; k = info.kernel_time;
}
static uint64 faults()
{
    system_info s; get_system_info(&s); return s.page_faults;
}
int main(int argc, char** argv)
{
    bigtime_t start = system_time();
    for (int i = 1; i < argc; i++) {
        bigtime_t u0, k0, u1, k1;
        usage(u0, k0);
        uint64 f0 = faults();
        bigtime_t t0 = system_time();
        void* h = dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL);
        bigtime_t t1 = system_time();
        uint64 f1 = faults();
        usage(u1, k1);
        std::printf("%-28s wall %7.1f ms  user %6.1f  kernel %7.1f  faults %6llu %s\n", argv[i],
            (t1 - t0) / 1e3, (u1 - u0) / 1e3, (k1 - k0) / 1e3, (unsigned long long)(f1 - f0), h ? "" : dlerror());
    }
    std::printf("total %.1f ms\n", (system_time() - start) / 1e3);
    return 0;
}
