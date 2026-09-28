// Milestone lines for extension start-up and popup latency, printed when
// SUMMIT_EXTENSION_TIMING=1. They share system_time() with the engine's
// "Summit extension timing" lines, so one log shows both sides in order.
#pragma once
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace summit {

inline bool ExtensionTimingEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("SUMMIT_EXTENSION_TIMING");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

}

#define SUMMIT_EXTENSION_TIMING(format, ...) do { \
    if (summit::ExtensionTimingEnabled()) \
        std::fprintf(stderr, "Summit browser timing %.3f " format "\n", system_time() / 1e6, ##__VA_ARGS__); \
} while (0)
