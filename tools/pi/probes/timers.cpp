// timers: how late 16 ms waits wake up: snooze(), acquire_sem_etc() with a
// relative timeout, and pthread_cond_clockwait() on CLOCK_MONOTONIC.
#include <OS.h>
#include <pthread.h>
#include <time.h>
#include <cstdio>
static void report(const char* what, bigtime_t* lates, int n)
{
    bigtime_t worst = 0, sum = 0;
    for (int i = 0; i < n; i++) { sum += lates[i]; if (lates[i] > worst) worst = lates[i]; }
    std::printf("%-28s mean late %7.2f ms  worst %7.2f ms\n", what, sum / 1e3 / n, worst / 1e3);
}
int main()
{
    const int n = 20; bigtime_t lates[n];
    for (int i = 0; i < n; i++) { bigtime_t t = system_time(); snooze(16000); lates[i] = system_time() - t - 16000; }
    report("snooze 16 ms", lates, n);
    sem_id sem = create_sem(0, "t");
    for (int i = 0; i < n; i++) { bigtime_t t = system_time(); acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, 16000); lates[i] = system_time() - t - 16000; }
    report("acquire_sem_etc 16 ms", lates, n);
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER; pthread_cond_t c; pthread_condattr_t a;
    pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&c, &a);
    pthread_mutex_lock(&m);
    for (int i = 0; i < n; i++) {
        timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); ts.tv_nsec += 16000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        bigtime_t t = system_time(); pthread_cond_clockwait(&c, &m, CLOCK_MONOTONIC, &ts); lates[i] = system_time() - t - 16000;
    }
    report("pthread_cond_clockwait 16 ms", lates, n);
    for (int i = 0; i < n; i++) {
        timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_nsec += 16000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        bigtime_t t = system_time(); pthread_cond_timedwait(&c, &m, &ts); lates[i] = system_time() - t - 16000;
    }
    report("pthread_cond_timedwait 16 ms", lates, n);
    return 0;
}
