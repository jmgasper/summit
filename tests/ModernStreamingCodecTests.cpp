// Decode the streaming parsers' packets through native Media Kit, independently
// of HTML's playback clock. Used alongside the in-browser MSE tests.
#include "WebMParserHaiku.h"
#include "MPEGAudioParserHaiku.h"
#include "VideoPresentationTimelineHaiku.h"
#if SUMMIT_TEST_MPEGTS
#include "MPEGTSParserHaiku.h"
#endif
#include "nlohmann/json.hpp"
#include <Application.h>
#include <MediaDecoder.h>
#include <MediaFormats.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>

using nlohmann::json;

class Decoder : public BMediaDecoder {
public:
    Decoder(const SummitMP4::TrackInfo& info, const std::vector<SummitMP4::Sample>& samples)
        : info(info), samples(samples) { }
    const SummitMP4::TrackInfo& info;
    const std::vector<SummitMP4::Sample>& samples;
    size_t position { 0 };
    WebCore::VideoPresentationTimelineHaiku timeline;
    bool mapsTimestamps() const { return info.codec == "av01" || info.codec == "hvc1" || info.codec == "hev1"; }
    status_t GetNextChunk(const void** data, size_t* size, media_header* header) override
    {
        if (position == samples.size())
            return B_LAST_BUFFER_ERROR;
        const auto& sample = samples[position++];
        *header = { };
        header->type = info.kind == SummitMP4::TrackInfo::Kind::Video ? B_MEDIA_ENCODED_VIDEO : B_MEDIA_ENCODED_AUDIO;
        header->start_time = sample.presentationTime * 1000000 / info.timescale;
        if (mapsTimestamps() && !timeline.append(header->start_time))
            return B_MEDIA_BAD_FORMAT;
        header->size_used = sample.data.size();
        if (sample.isSync && info.kind == SummitMP4::TrackInfo::Kind::Video)
            header->u.encoded_video.field_flags = B_MEDIA_KEY_FRAME;
        *data = sample.data.data();
        *size = sample.data.size();
        return B_OK;
    }
};

static json decode(const SummitMP4::TrackInfo& info, const std::vector<SummitMP4::Sample>& samples, bool clearPreSkip)
{
    const std::map<std::string, uint32> codecs { { "avc1", 27 }, { "hvc1", 173 }, { "hev1", 173 }, { "ac-3", 86019 }, { "ec-3", 86056 }, { "vp08", 139 }, { "vp09", 167 }, { "av01", 226 }, { "vorb", 86021 }, { "Opus", 86076 }, { "mp4a", 86018 }, { "mp1", 86058 }, { "mp2", 86016 }, { "mp3", 86017 } };
    auto found = codecs.find(info.codec);
    if (found == codecs.end())
        return { { "error", "unsupported codec" } };
    bool video = info.kind == SummitMP4::TrackInfo::Kind::Video;
    media_format_description description { };
    description.family = B_MISC_FORMAT_FAMILY;
    description.u.misc.file_format = 'ffmp';
    description.u.misc.codec = found->second;
    media_format input { };
    input.type = video ? B_MEDIA_ENCODED_VIDEO : B_MEDIA_ENCODED_AUDIO;
    BMediaFormats formats;
    auto status = formats.GetFormatFor(description, &input);
    if (status != B_OK)
        status = formats.MakeFormatFor(&description, 1, &input);
    if (status != B_OK)
        return { { "formatStatus", status } };
    if (video) {
        input.u.encoded_video.output.display.line_width = info.width;
        input.u.encoded_video.output.display.line_count = info.height;
        input.u.encoded_video.output.field_rate = 20;
        input.u.encoded_video.output.pixel_width_aspect = 1;
        input.u.encoded_video.output.pixel_height_aspect = 1;
    } else {
        input.u.encoded_audio.output.frame_rate = info.sampleRate;
        input.u.encoded_audio.output.channel_count = info.channels;
        input.u.encoded_audio.output.byte_order = B_MEDIA_HOST_ENDIAN;
        input.u.encoded_audio.frame_size = 0;
    }
    auto config = info.codecConfig;
    if (clearPreSkip && info.codec == "Opus" && config.size() >= 19)
        config[10] = config[11] = 0;
    Decoder decoder(info, samples);
    status = decoder.SetTo(&input, config.data(), config.size());
    json result { { "codec", info.codec }, { "inputPackets", samples.size() }, { "setupStatus", status } };
    if (status != B_OK)
        return result;
    media_codec_info chosen { };
    decoder.GetDecoderInfo(&chosen);
    result["decoder"] = chosen.pretty_name;
    media_format output { };
    size_t bytesPerFrame;
    size_t capacity;
    if (video) {
        output.type = B_MEDIA_RAW_VIDEO;
        output.u.raw_video = media_raw_video_format::wildcard;
        output.u.raw_video.display.format = B_RGB32;
        output.u.raw_video.display.line_width = info.width;
        output.u.raw_video.display.line_count = info.height;
    } else {
        output.type = B_MEDIA_RAW_AUDIO;
        output.u.raw_audio = media_raw_audio_format::wildcard;
        output.u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
    }
    status = decoder.SetOutputFormat(&output);
    result["outputStatus"] = status;
    if (status != B_OK)
        return result;
    if (video) {
        bytesPerFrame = (output.u.raw_video.display.bytes_per_row ? output.u.raw_video.display.bytes_per_row : info.width * 4) * info.height;
        capacity = bytesPerFrame;
    } else {
        bytesPerFrame = (output.u.raw_audio.format & media_raw_audio_format::B_AUDIO_SIZE_MASK) * output.u.raw_audio.channel_count;
        capacity = output.u.raw_audio.buffer_size;
        result["audioFormat"] = output.u.raw_audio.format;
        result["rate"] = output.u.raw_audio.frame_rate;
        result["channels"] = output.u.raw_audio.channel_count;
    }
    if (!capacity || !bytesPerFrame || capacity > 256 * 1024 * 1024)
        return { { "error", "invalid output format" } };
    std::vector<uint8_t> buffer(capacity);
    uint64_t frames = 0;
    uint64_t changed = 0;
    uint64_t previousHash = 0;
    double peak = 0;
    json timestamps = json::array();
    std::vector<int64_t> presentationTimes;
    std::vector<int64_t> expectedPresentationTimes;
    if (video) {
        for (auto& sample : samples)
            expectedPresentationTimes.push_back(sample.presentationTime * 1000000 / info.timescale);
        std::sort(expectedPresentationTimes.begin(), expectedPresentationTimes.end());
    }
    for (unsigned count = 0; count < 10000; ++count) {
        media_header header { };
        int64 amount = 0;
        status = decoder.Decode(buffer.data(), &amount, &header, nullptr);
        if (status != B_OK || amount <= 0)
            break;
        if (uint64_t(amount) * bytesPerFrame > capacity)
            return { { "error", "output overflow" } };
        frames += amount;
        timestamps.push_back({ header.start_time, amount });
        if (video) {
            auto pts = decoder.mapsTimestamps() ? decoder.timeline.take() : std::optional<int64_t>(header.start_time);
            if (!pts || amount != 1)
                return { { "error", "missing presentation timestamp" } };
            presentationTimes.push_back(*pts);
            uint64_t hash = 14695981039346656037ULL;
            for (auto byte : buffer)
                hash = (hash ^ byte) * 1099511628211ULL;
            if (hash != previousHash)
                ++changed;
            previousHash = hash;
        } else if (output.u.raw_audio.format == media_raw_audio_format::B_AUDIO_FLOAT) {
            auto* pcm = reinterpret_cast<const float*>(buffer.data());
            for (size_t i = 0; i < size_t(amount) * output.u.raw_audio.channel_count; ++i) {
                if (!std::isfinite(pcm[i]))
                    return { { "error", "nonfinite PCM" } };
                peak = std::max(peak, std::abs(double(pcm[i])));
            }
        } else if (output.u.raw_audio.format == media_raw_audio_format::B_AUDIO_SHORT) {
            auto* pcm = reinterpret_cast<const int16_t*>(buffer.data());
            for (size_t i = 0; i < size_t(amount) * output.u.raw_audio.channel_count; ++i)
                peak = std::max(peak, std::abs(double(pcm[i])) / 32768.0);
        } else
            return { { "error", "unmeasured PCM format" }, { "format", output.u.raw_audio.format } };
    }
    result["endStatus"] = status;
    result["frames"] = frames;
    result["changedFrames"] = changed;
    result["peak"] = peak;
    result["timestamps"] = timestamps;
    if (video) {
        result["presentationTimes"] = presentationTimes;
        result["presentationOrderCorrect"] = presentationTimes == expectedPresentationTimes;
    }
    uint64_t expectedFrames = 0;
    for (auto& sample : samples)
        expectedFrames += sample.duration;
    if (info.codec == "vorb" && !samples.empty())
        expectedFrames -= samples.front().duration;
    if (info.codec == "Opus" && !clearPreSkip)
        expectedFrames -= uint32_t(info.codecConfig[10]) | uint32_t(info.codecConfig[11]) << 8;
    result["expectedFrames"] = expectedFrames;
    result["ok"] = video ? frames == samples.size() && changed > 1 && presentationTimes == expectedPresentationTimes
        : frames == expectedFrames && (peak > 0.01 || info.codec == "mp1");
    return result;
}

int main(int argc, char** argv)
{
    BApplication application("application/x-vnd.Summit-streaming-codec-tests");
    bool passed = true;
    for (int index = 1; index < argc; ++index) {
        std::ifstream input(argv[index], std::ios::binary);
        std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input), { });
        std::vector<SummitMP4::TrackInfo> tracks;
        std::map<uint32_t, std::vector<SummitMP4::Sample>> packets;
        auto parse = [&](auto& parser) {
            parser.onInitSegment = [&](SummitMP4::InitSegment&& init) { tracks = std::move(init.tracks); };
            parser.onSample = [&](SummitMP4::Sample&& sample) { packets[sample.trackID].push_back(std::move(sample)); };
            return !bytes.empty() && parser.append(bytes.data(), bytes.size());
        };
        bool parsed;
#if SUMMIT_TEST_MPEGTS
        if (std::string(argv[index]).ends_with(".ts")) {
            SummitMPEGTS::Parser parser;
            parsed = parse(parser) && parser.finish();
        } else
#endif
        if (std::string(argv[index]).ends_with(".webm")) {
            SummitWebM::Parser parser;
            parsed = parse(parser);
            parser.flushPendingSamples();
        } else if (std::string(argv[index]).ends_with(".mp4")) {
            SummitMP4::Parser parser;
            parsed = parse(parser);
        } else {
            SummitMPEGAudio::Parser parser(std::string(argv[index]).ends_with(".aac") ? SummitMPEGAudio::Parser::Format::ADTS : SummitMPEGAudio::Parser::Format::MPEG);
            parsed = parse(parser);
        }
        if (!parsed) {
            std::cout << json({ { "file", argv[index] }, { "parseError", true } }) << '\n';
            passed = false;
            continue;
        }
        for (auto& track : tracks) {
            auto result = decode(track, packets[track.id], false);
            result["file"] = argv[index];
            passed &= result.value("ok", false);
            std::cout << result << '\n' << std::flush;
            if (track.codec == "Opus") {
                result = decode(track, packets[track.id], true);
                result["file"] = argv[index];
                result["clearedPreSkip"] = true;
                passed &= result.value("ok", false);
                std::cout << result << '\n' << std::flush;
            }
        }
    }
    std::cout << "StreamingCodec_RESULT " << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
