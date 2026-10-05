/* Minimal backtrace() family for arm64 Haiku, built on libgcc's unwinder. */
#define _GNU_SOURCE
#include "execinfo.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

struct state { void** buffer; int size; int count; };

static _Unwind_Reason_Code collect(struct _Unwind_Context* context, void* argument)
{
	struct state* s = argument;
	if (s->count >= s->size)
		return _URC_END_OF_STACK;
	void* pc = (void*)_Unwind_GetIP(context);
	if (!pc)
		return _URC_END_OF_STACK;
	s->buffer[s->count++] = pc;
	return _URC_NO_REASON;
}

int backtrace(void** buffer, int size)
{
	struct state s = { buffer, size, 0 };
	if (size <= 0)
		return 0;
	_Unwind_Backtrace(collect, &s);
	return s.count;
}

static int format(void* address, char* out, size_t length)
{
	Dl_info info;
	if (dladdr(address, &info) && info.dli_sname)
		return snprintf(out, length, "%s(%s+0x%lx) [%p]", info.dli_fname ? info.dli_fname : "?",
			info.dli_sname, (unsigned long)((char*)address - (char*)info.dli_saddr), address);
	if (dladdr(address, &info) && info.dli_fname)
		return snprintf(out, length, "%s(+0x%lx) [%p]", info.dli_fname,
			(unsigned long)((char*)address - (char*)info.dli_fbase), address);
	return snprintf(out, length, "[%p]", address);
}

char** backtrace_symbols(void* const* buffer, int size)
{
	char line[1024];
	size_t total = size * sizeof(char*);
	for (int i = 0; i < size; i++)
		total += format(buffer[i], line, sizeof(line)) + 1;
	char** result = malloc(total);
	if (!result)
		return NULL;
	char* text = (char*)(result + size);
	for (int i = 0; i < size; i++) {
		int n = format(buffer[i], line, sizeof(line));
		memcpy(text, line, n + 1);
		result[i] = text;
		text += n + 1;
	}
	return result;
}

void backtrace_symbols_fd(void* const* buffer, int size, int fd)
{
	char line[1024];
	for (int i = 0; i < size; i++) {
		int n = format(buffer[i], line, sizeof(line) - 1);
		line[n] = '\n';
		write(fd, line, n + 1);
	}
}
