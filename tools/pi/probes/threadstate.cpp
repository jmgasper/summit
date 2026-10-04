// threadstate INTERVAL_MS COUNT TEAM... : every INTERVAL_MS, one line per
// thread of each team that used CPU (the first time: every thread): system_time() in seconds,
// team, thread name, state, the semaphore it waits on and the CPU time it
// used since the last sample. Shows what a stalled main thread is doing
// (busy, or blocked on which lock) where `profile` is not available.
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <map>

static const char* stateName(thread_state state)
{
	switch (state) {
	case B_THREAD_RUNNING: return "run";
	case B_THREAD_READY: return "rdy";
	case B_THREAD_RECEIVING: return "recv";
	case B_THREAD_ASLEEP: return "zzz";
	case B_THREAD_SUSPENDED: return "susp";
	case B_THREAD_WAITING: return "wait";
	}
	return "?";
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "usage: threadstate INTERVAL_MS COUNT TEAM...\n");
		return 1;
	}
	bigtime_t interval = std::atoi(argv[1]) * 1000LL;
	int count = std::atoi(argv[2]);
	std::map<thread_id, bigtime_t> lastTime;
	for (int sample = 0; sample < count; sample++) {
		double now = system_time() / 1e6;
		for (int t = 3; t < argc; t++) {
			team_id team = std::atoi(argv[t]);
			int32 cookie = 0;
			thread_info info;
			while (get_next_thread_info(team, &cookie, &info) == B_OK) {
				bigtime_t used = info.user_time + info.kernel_time;
				bigtime_t delta = lastTime.count(info.thread) ? used - lastTime[info.thread] : 0;
				lastTime[info.thread] = used;
				char semName[B_OS_NAME_LENGTH] = "";
				if (info.sem >= 0) {
					sem_info sem;
					if (get_sem_info(info.sem, &sem) == B_OK)
						std::snprintf(semName, sizeof(semName), "%s", sem.name);
				}
				// Idle waits are the rule: after the first sample, print a
				// thread only when it used a millisecond or more, or runs.
				bool busy = delta >= 1000 || info.state == B_THREAD_RUNNING || info.state == B_THREAD_READY;
				if (!busy && sample > 0 && info.thread != team)
					continue;
				std::printf("%.3f %d %-28.28s %-4s %-28.28s +%.0f ms\n", now, (int)team, info.name,
					stateName(info.state), semName, delta / 1000.0);
			}
		}
		std::fflush(stdout);
		snooze(interval);
	}
	return 0;
}
