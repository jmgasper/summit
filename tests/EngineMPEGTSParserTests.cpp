// Exercise the public TS parser without WebKit or Haiku. The Python runner
// compares packets with an independent ffprobe reference and varies appends.
#include "MPEGTSParserHaiku.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iterator>
#include <iostream>

using nlohmann::json;

int main(int argc, char** argv)
{
    if (argc < 3 || argc > 5)
        return 2;
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), { });
    size_t chunk = std::stoul(argv[2]);
    if (!chunk)
        return 2;
    SummitMPEGTS::Parser parser;
    json tracks = json::array(), samples = json::array();
    parser.onInitSegment = [&](SummitMP4::InitSegment&& init) {
        for (auto& track : init.tracks) {
            tracks.push_back({ { "id", track.id }, { "codec", track.codec }, { "scale", track.timescale },
                { "extra", track.codecConfig }, { "width", track.width }, { "height", track.height },
                { "rate", track.sampleRate }, { "channels", track.channels } });
        }
    };
    parser.onSample = [&](SummitMP4::Sample&& sample) {
        uint64_t hash = 14695981039346656037ULL;
        for (auto byte : sample.data)
            hash = (hash ^ byte) * 1099511628211ULL;
        samples.push_back({ { "id", sample.trackID }, { "pts", sample.presentationTime },
            { "dts", sample.decodeTime }, { "duration", sample.duration }, { "sync", sample.isSync },
            { "size", sample.data.size() }, { "hash", hash } });
    };
    auto append = [&](const std::vector<uint8_t>& inputBytes) {
        for (size_t position = 0; position < inputBytes.size(); position += chunk) {
            if (!parser.append(inputBytes.data() + position, std::min(chunk, inputBytes.size() - position)))
                return false;
        }
        return true;
    };
    bool ok = append(bytes);
    if (ok && argc >= 4) {
        // Abort with a partial transport packet and a video access unit still
        // inside FFmpeg. The following media contains no initialization PSI.
        bool offsetOnly = argc == 5 && std::string(argv[4]) == "--timestamp-offset";
        if (offsetOnly)
            ok = parser.finish() && parser.resetTimestampOffset();
        else if (argc == 5) {
            std::ifstream malformed(argv[4], std::ios::binary);
            std::vector<uint8_t> rejected(std::istreambuf_iterator<char>(malformed), { });
            ok = !rejected.empty() && !append(rejected);
        } else
            ok = parser.append(bytes.data(), std::min<size_t>(37, bytes.size()));
        if (!offsetOnly)
            parser.reset();
        if (!offsetOnly)
            tracks.clear();
        samples.clear();
        std::ifstream replacement(argv[3], std::ios::binary);
        bytes = std::vector<uint8_t>(std::istreambuf_iterator<char>(replacement), { });
        ok = ok && !bytes.empty() && append(bytes);
    }
    if (ok)
        ok = parser.finish();
    std::cout << json({ { "available", parser.available() }, { "ok", ok }, { "tracks", tracks }, { "samples", samples } }) << '\n';
    return ok ? 0 : 1;
}
