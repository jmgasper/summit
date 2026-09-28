// SUMMIT_UI_STALL_TRACE=1 prints "Summit stall:" lines for message dispatches
// (application and window threads) and profile saves that take longer than
// 50 ms, with the message code, so a scrolling stall can be traced to the
// thread that stopped feeding events.
#pragma once
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {

inline bool StallTraceEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_UI_STALL_TRACE");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

class StallScope {
public:
    StallScope(const char* where, uint32 what) : fWhere(where), fWhat(what), fStart(StallTraceEnabled() ? system_time() : 0) {}
    ~StallScope()
    {
        if (!fStart) return;
        const bigtime_t took = system_time() - fStart;
        if (took < 50000) return;
        char code[5] = { char(fWhat >> 24), char(fWhat >> 16), char(fWhat >> 8), char(fWhat), 0 };
        for (char& c : code) if (c && (c < 32 || c > 126)) c = '?';
        std::fprintf(stderr, "Summit stall: %s what=%s (0x%08x) %.0f ms at %.3f\n", fWhere, code, (unsigned)fWhat, took / 1000.0, system_time() / 1e6);
    }
private:
    const char* fWhere;
    uint32 fWhat;
    bigtime_t fStart;
};

}
