// Portable packet-level probe for the Haiku MSE WebM parser.
#include "WebMParserHaiku.h"
#include "nlohmann/json.hpp"
#include <openssl/sha.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>

using nlohmann::json;

static std::string digest(const std::vector<uint8_t>& bytes)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(bytes.data(), bytes.size(), hash);
    std::ostringstream out;
    for (auto byte : hash)
        out << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return out.str();
}

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 4)
        return 2;
    size_t chunk = std::stoull(argv[2]);
    if (!chunk)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input)
        return 2;
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), { });
    json result { { "init", json::array() }, { "samples", json::array() } };
    SummitWebM::Parser parser;
    parser.onInitSegment = [&](SummitMP4::InitSegment&& init) {
        json tracks = json::array();
        for (auto& track : init.tracks)
            tracks.push_back({ { "id", track.id }, { "codec", track.codec }, { "scale", track.timescale },
                { "width", track.width }, { "height", track.height }, { "rate", track.sampleRate },
                { "channels", track.channels }, { "config", digest(track.codecConfig) } });
        result["init"].push_back({ { "scale", init.movieTimescale }, { "duration", init.duration }, { "tracks", tracks } });
    };
    parser.onSample = [&](SummitMP4::Sample&& sample) {
        result["samples"].push_back({ { "track", sample.trackID }, { "pts", sample.presentationTime },
            { "dts", sample.decodeTime }, { "duration", sample.duration }, { "key", sample.isSync },
            { "size", sample.data.size() }, { "sha256", digest(sample.data) },
            { "trimStart", sample.trimStart }, { "trimEnd", sample.trimEnd } });
    };
    // Optional reset byte offset: abort a partially appended element, then
    // resume at a fresh Cluster, retaining the prior initialization segment.
    size_t resetOffset = argc == 4 ? std::stoull(argv[3]) : SIZE_MAX;
    bool passed = true;
    for (size_t offset = 0; offset < bytes.size();) {
        size_t amount = std::min(chunk, bytes.size() - offset);
        if (offset < resetOffset)
            amount = std::min(amount, resetOffset - offset);
        if (!parser.append(bytes.data() + offset, amount)) {
            passed = false;
            result["failedAt"] = offset;
            break;
        }
        offset += amount;
        if (offset == resetOffset) {
            parser.reset();
            resetOffset = SIZE_MAX;
        }
    }
    parser.flushPendingSamples();
    result["ok"] = passed;
    std::cout << result.dump() << '\n';
    return passed ? 0 : 1;
}
