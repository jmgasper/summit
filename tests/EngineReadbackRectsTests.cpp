// Copyright (C) 2026 Summit contributors. SPDX-License-Identifier: BSD-2-Clause
#include "config.h"
#include <WebCore/IntRect.h>
#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <tuple>
#include <vector>
#include <wtf/Vector.h>

using namespace WebCore;
// Generated verbatim from AcceleratedSurface.cpp by the test runner.
#include "ReadbackRectsUnderTest.h"

static unsigned checks;
static unsigned failures;

static void check(bool condition, const char* label)
{
    ++checks;
    if (!condition) {
        if (failures < 12)
            std::printf("FAIL %s\n", label);
        ++failures;
    }
}

// Split an input rectangle at every output edge. Coverage is constant within
// each resulting cell, so checking its upper-left pixel checks the whole cell.
// This tests the union, without requiring a particular merging heuristic.
static bool covered(const IntRect& input, const Vector<IntRect, 1>& output)
{
    std::vector<int> xs { input.x(), input.maxX() };
    std::vector<int> ys { input.y(), input.maxY() };
    for (const auto& rect : output) {
        for (int x : { rect.x(), rect.maxX() })
            if (x > input.x() && x < input.maxX())
                xs.push_back(x);
        for (int y : { rect.y(), rect.maxY() })
            if (y > input.y() && y < input.maxY())
                ys.push_back(y);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    for (int x : xs) {
        if (x == input.maxX())
            continue;
        for (int y : ys) {
            if (y == input.maxY())
                continue;
            if (!std::any_of(output.begin(), output.end(), [&](const auto& r) {
                    return x >= r.x() && x < r.maxX() && y >= r.y() && y < r.maxY();
                }))
                return false;
        }
    }
    return true;
}

static void verify(const Vector<IntRect, 1>& input, const Vector<IntRect, 1>& output,
    const IntSize& size, const char* label)
{
    IntRect frame { { }, size };
    for (const auto& rect : input)
        check(covered(rect, output), label);
    for (const auto& rect : output)
        check(!rect.isEmpty() && frame.contains(rect), "read stays within the framebuffer");
    check(output.size() <= input.size(), "coalescing never increases the read count");
}

int main()
{
    // A wide update at the top and two tall, narrow updates at opposite edges.
    // Narrow updates must not cover each other merely because their Y spans match.
    IntSize size { 3840, 2160 };
    Vector<IntRect, 1> separated { { 0, 0, 1600, 100 },
        { 0, 1400, 100, 600 }, { 3700, 1400, 100, 600 } };
    verify(separated, widenedReadbackRectsHaiku(Vector<IntRect, 1>(separated), size), size,
        "separated narrow damage survives widening");
    verify(separated, widenedReadbackRectsHaiku(mergedReadbackRectsHaiku(Vector<IntRect, 1>(separated)), size), size,
        "separated narrow damage survives the complete readback pipeline");

    // Exercise the union invariant over varied framebuffer sizes, overlaps,
    // equal Y spans, full-width bands, fragmented damage and input order.
    std::mt19937 random(0x5eed1234);
    for (unsigned iteration = 0; iteration < 12000; ++iteration) {
        IntSize frame { 320 + int(random() % 7361), 240 + int(random() % 4081) };
        Vector<IntRect, 1> damage;
        unsigned count = random() % 49;
        for (unsigned i = 0; i < count; ++i) {
            int x = random() % frame.width();
            int y = random() % frame.height();
            int width = 1 + random() % (frame.width() - x);
            int height = 1 + random() % (frame.height() - y);
            if (iteration % 3 == 0 && i % 2 == 0) {
                y = frame.height() / 2;
                height = frame.height() / 3;
            }
            if (iteration % 5 == 0 && i == 0) {
                x = 0;
                width = frame.width();
            }
            damage.append({ x, y, width, height });
        }
        auto merged = mergedReadbackRectsHaiku(Vector<IntRect, 1>(damage));
        verify(damage, merged, frame, "merged damage covers every changed pixel");
        check(merged.size() <= 16, "read count remains bounded");
        verify(damage, widenedReadbackRectsHaiku(Vector<IntRect, 1>(damage), frame), frame,
            "widened damage covers every changed pixel");
        verify(damage, widenedReadbackRectsHaiku(WTF::move(merged), frame), frame,
            "complete readback pipeline covers every changed pixel");
    }
    std::printf("READBACK_RECTS_RESULT %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
