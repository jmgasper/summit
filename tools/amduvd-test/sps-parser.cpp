// Copyright 2026 Summit contributors. Distributed under the MIT license.
// Includes the actual parser extracted by sps-parser-test.py, not a copy.
#include "parser-under-test.h"
#include <cassert>
#include <random>

class Bits {
public:
    void bit(unsigned value) {
        if (!(count % 8)) bytes.push_back(0);
        bytes.back() |= (value & 1) << (7 - count++ % 8);
    }
    void bits(unsigned value, unsigned count) {
        for (unsigned i = count; i; --i) bit(value >> (i - 1));
    }
    void ue(uint32_t value) {
        uint64_t coded = uint64_t(value) + 1;
        unsigned width = 0;
        for (uint64_t n = coded; n; n >>= 1) ++width;
        for (unsigned i = 1; i < width; ++i) bit(0);
        for (unsigned i = width; i; --i) bit(coded >> (i - 1));
    }
    std::vector<uint8_t> bytes;
    unsigned count { 0 };
};

static std::vector<uint8_t> configuration(uint32_t firstScalingDelta)
{
    Bits b;
    b.bits(100, 8); b.bits(0, 8); b.bits(31, 8); b.ue(0);
    b.ue(1); b.ue(0); b.ue(0); b.bit(0); b.bit(1); b.bit(1);
    b.ue(firstScalingDelta);
    // Remaining first scaling-list entries; all other lists absent.
    for (unsigned i = 1; i < 16; ++i) b.ue(0);
    for (unsigned i = 1; i < 8; ++i) b.bit(0);
    b.ue(0); b.ue(0); b.ue(0); b.ue(4); b.bit(0);
    b.ue(19); b.ue(14); b.bit(1); b.bit(1); b.bit(0); b.bit(0); b.bit(1);
    std::vector<uint8_t> nal { 0x67 };
    unsigned zeros = 0;
    for (uint8_t byte : b.bytes) {
        if (zeros >= 2 && byte <= 3) { nal.push_back(3); zeros = 0; }
        nal.push_back(byte);
        zeros = byte ? 0 : zeros + 1;
    }
    std::vector<uint8_t> record { 1, 100, 0, 31, 0xff, 0xe1, uint8_t(nal.size() >> 8), uint8_t(nal.size()) };
    record.insert(record.end(), nal.begin(), nal.end());
    return record;
}

int main()
{
    using WebCore::h264SequenceInfo;
    auto valid = configuration(0);
    auto info = h264SequenceInfo(valid);
    assert(info && info->profile == 100 && info->bitDepthLuma == 8 && info->maxReferenceFrames == 4 && info->frameMbsOnly);
    // se(v): 4294967293 maps to INT_MAX. Its addition used to overflow.
    for (uint32_t value : { 4294967293u, 4294967294u, 255u, 258u })
        assert(!h264SequenceInfo(configuration(value)));
    assert(h264SequenceInfo(configuration(253))); // legal +127
    assert(h264SequenceInfo(configuration(256))); // legal -128
    std::mt19937 random(0x5100);
    for (unsigned iteration = 0; iteration < 25000; ++iteration) {
        std::vector<uint8_t> data = valid;
        unsigned edits = 1 + random() % 8;
        while (edits--) data[random() % data.size()] ^= 1u << (random() % 8);
        if (!(iteration % 3)) data.resize(random() % data.size());
        h264SequenceInfo(data);
    }
    puts("PASS: real SPS parser, legal scaling limits, overflow regressions and 25000 malformed configurations");
}
