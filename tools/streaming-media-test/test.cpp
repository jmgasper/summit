#include "StreamingMediaIOHaiku.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>
using namespace WebCore;
static uint8_t expected(off_t i) { return uint8_t((i * 2654435761u) >> 13); }
static bool check(StreamingMediaIO& io, off_t at, size_t length)
{
    std::vector<uint8_t> buffer(length);
    auto start = std::chrono::steady_clock::now();
    ssize_t got = io.ReadAt(at, buffer.data(), length);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    off_t size; io.GetSize(&size);
    size_t want = std::min<off_t>(length, std::max<off_t>(0, size - at));
    if (got != ssize_t(want)) { printf("FAIL read at %lld: got %zd want %zu\n", (long long)at, got, want); return false; }
    for (size_t i = 0; i < want; ++i) if (buffer[i] != expected(at + i)) { printf("FAIL byte %lld\n", (long long)(at + i)); return false; }
    printf("ok read %zu at %lld in %.1f ms\n", want, (long long)at, ms);
    return true;
}
int main(int argc, char** argv)
{
    std::atomic<bool> abort { false };
    auto t0 = std::chrono::steady_clock::now();
    auto io = StreamingMediaIO::open(argv[1], abort);
    if (!io) { printf("open failed\n"); return argc > 2 ? 0 : 1; }
    off_t size; io->GetSize(&size);
    printf("open in %.1f ms, size %lld\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), (long long)size);
    bool ok = check(*io, 0, 4096) && check(*io, size - 5000, 8192) && check(*io, size / 2, 300000)
        && check(*io, 1000, 700000) && check(*io, size - 1, 1) && check(*io, size, 10);
    // everything, in 1 MB reads
    for (off_t at = 0; ok && at < size; at += 1 << 20) {
        std::vector<uint8_t> b(1 << 20); ssize_t got = io->ReadAt(at, b.data(), b.size());
        size_t want = std::min<off_t>(b.size(), size - at);
        if (got != ssize_t(want)) { printf("FAIL sweep %lld\n", (long long)at); ok = false; }
        for (size_t i = 0; ok && i < want; ++i) if (b[i] != expected(at + i)) { printf("FAIL sweep byte\n"); ok = false; }
    }
    printf(ok ? "PASS\n" : "FAILED\n");
    // abort wakes a blocked reader
    return ok ? 0 : 1;
}
