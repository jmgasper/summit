/* LD_PRELOAD crash reporter for arm64 Haiku: prints PC/LR/SP and a backtrace. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <OS.h>

static void where(const char* label, void* address)
{
	Dl_info info;
	if (dladdr(address, &info) && info.dli_fname)
		fprintf(stderr, "  %s %p %s+0x%lx (%s+0x%lx)\n", label, address, info.dli_fname,
			(unsigned long)((char*)address - (char*)info.dli_fbase),
			info.dli_sname ? info.dli_sname : "?",
			info.dli_saddr ? (unsigned long)((char*)address - (char*)info.dli_saddr) : 0UL);
	else
		fprintf(stderr, "  %s %p\n", label, address);
}

static void handler(int signal, siginfo_t* info, void* context)
{
	ucontext_t* uc = context;
	struct vregs* r = &uc->uc_mcontext;
	fprintf(stderr, "\n*** summit-crash: signal %d (%s) addr %p team %d thread %d (%s)\n", signal,
		strsignal(signal), info->si_addr, (int)getpid(), (int)find_thread(NULL), "");
	thread_info ti;
	if (get_thread_info(find_thread(NULL), &ti) == B_OK)
		fprintf(stderr, "  thread name: %s\n", ti.name);
	where("pc", (void*)r->elr);
	where("lr", (void*)r->lr);
	fprintf(stderr, "  sp %p fp %p x0 %lx x1 %lx x2 %lx\n", (void*)r->sp, (void*)r->x[29], r->x[0], r->x[1], r->x[2]);
	/* Walk frame pointers from the faulting frame. */
	unsigned long* fp = (unsigned long*)r->x[29];
	for (int i = 0; i < 40 && fp && ((unsigned long)fp & 7) == 0; i++) {
		void* ret = (void*)fp[1];
		if (!ret) break;
		char label[16]; snprintf(label, sizeof label, "#%d", i);
		where(label, ret);
		unsigned long* next = (unsigned long*)fp[0];
		if (next <= fp) break;
		fp = next;
	}
	void* frames[64];
	int n = backtrace(frames, 64);
	fprintf(stderr, "  unwinder backtrace (%d):\n", n);
	for (int i = 0; i < n; i++) where("  u", frames[i]);
	fflush(stderr);
	if (signal == SIGUSR1)
		return;
	_exit(128 + signal);
}

__attribute__((constructor)) static void install(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof action);
	action.sa_sigaction = handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGFPE, &action, NULL);
	sigaction(SIGABRT, &action, NULL);
	sigaction(SIGTRAP, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
}
