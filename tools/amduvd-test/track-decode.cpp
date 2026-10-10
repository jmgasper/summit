// Copyright 2026 Summit contributors. Distributed under the MIT license.
// Exercise Summit's real BMediaTrack substitution, including mid-stream
// hardware -> software recovery when run with fault-addon.cpp.
#include "config.h"
#include "MediaDecoderSelectionHaiku.h"
#include <Application.h>
#include <Entry.h>
#include <MediaFile.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <wtf/MainThread.h>

static void require(bool success, const char* message)
{
    if (!success) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv)
{
    require(argc == 5 || argc == 6, "usage: track-decode input.mp4 frame-count final-short-name output.bgra [seek-us]");
    BApplication app("application/x-vnd.Summit-amduvd-test");
    WTF::initializeMainThread();
    char* end = nullptr;
    unsigned long expected = std::strtoul(argv[2], &end, 10);
    require(end != argv[2] && !*end && expected > 0 && expected <= 100000, "valid frame count");
    entry_ref ref;
    require(get_ref_for_path(argv[1], &ref) == B_OK, "input file");
    BMediaFile file(&ref);
    require(file.InitCheck() == B_OK, "media file initialized");
    BMediaTrack* track = nullptr;
    media_format encoded { };
    for (int32 i = 0; i < file.CountTracks(); ++i) {
        auto* candidate = file.TrackAt(i);
        if (candidate && candidate->EncodedFormat(&encoded) == B_OK && encoded.type == B_MEDIA_ENCODED_VIDEO) {
            track = candidate;
            break;
        }
        if (candidate)
            file.ReleaseTrack(candidate);
    }
    require(track, "video track exists");
    media_format output { };
    output.type = B_MEDIA_RAW_VIDEO;
    output.u.raw_video = media_raw_video_format::wildcard;
    output.u.raw_video.display.format = B_RGB32;
    require(track->DecodedFormat(&output) == B_OK, "native software track format");
    auto decoder = WebCore::SubstituteVideoDecoderHaiku::createIfNeeded(*track, output);
    if (!decoder)
        decoder = WebCore::SubstituteVideoDecoderHaiku::createForTrackWithoutDecoder(*track, output);
    require(!!decoder, "explicit hardware or software substitution selected");
    if (argc == 6) {
        char* seekEnd = nullptr;
        bigtime_t seekTime = std::strtoll(argv[5], &seekEnd, 10);
        require(seekEnd != argv[5] && !*seekEnd && seekTime >= 0, "valid seek time");
        require(decoder->seekToTime(&seekTime) == B_OK, "seek to preceding keyframe");
        std::printf("seek requested=%s keyframe=%lld\n", argv[5], static_cast<long long>(seekTime));
    }
    output = decoder->decodedFormat();
    auto display = output.u.raw_video.display;
    require(display.format == B_RGB32 && display.line_width && display.line_count
        && display.bytes_per_row >= display.line_width * 4, "RGB output geometry");
    size_t bytes = size_t(display.bytes_per_row) * display.line_count;
    const bool expectFormatFailure = !std::strcmp(argv[3], "format-error");
    // Keep a generous canary beyond the advertised extent, so the historical
    // write-after-format-rejection regression cannot corrupt the test's heap.
    std::vector<unsigned char> guarded(bytes + 65536 + 64, 0xa5);
    if (expectFormatFailure) {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            int64 count = 123;
            media_header header { };
            status_t status = decoder->readFrame(guarded.data() + 64, &count, &header);
            require(std::all_of(guarded.begin(), guarded.end(), [](auto c) { return c == 0xa5; }), "format rejection never writes output");
            require(status == B_MEDIA_BAD_FORMAT && !count, "format error remains latched on repeated reads");
        }
        decoder = nullptr;
        file.ReleaseTrack(track);
        snooze(250000);
        std::puts("PASS: incompatible replacement format is sticky and never writes the caller's buffer");
        return 0;
    }
    FILE* capture = std::fopen(argv[4], "wb");
    require(capture, "open RGB capture");
    media_codec_info codec { };
    decoder->codecInfo(&codec);
    std::printf("initial=%s %ux%u pitch=%u\n", codec.short_name, display.line_width, display.line_count, display.bytes_per_row);
    unsigned long frames = 0;
    bigtime_t previousTime = -1;
    for (;;) {
        int64 count = 0;
        media_header header { };
        status_t status = decoder->readFrame(guarded.data() + 64, &count, &header);
        require(std::all_of(guarded.begin(), guarded.begin() + 64, [](auto c) { return c == 0xa5; })
            && std::all_of(guarded.begin() + 64 + bytes, guarded.end(), [](auto c) { return c == 0xa5; }), "output bounds");
        if (status == B_LAST_BUFFER_ERROR)
            break;
        if (status != B_OK)
            std::fprintf(stderr, "decode failed at %lu: %ld (%s)\n", frames, long(status), std::strerror(status));
        require(status == B_OK && count == 1, "one decoded picture");
        bigtime_t time = decoder->currentTime();
        require(time > previousTime, "monotonic track time through fallback");
        previousTime = time;
        for (uint32 y = 0; y < display.line_count; ++y)
            require(std::fwrite(guarded.data() + 64 + size_t(y) * display.bytes_per_row, display.line_width * 4, 1, capture) == 1, "write capture");
        require(++frames <= expected, "no duplicate pictures");
    }
    decoder->codecInfo(&codec);
    std::printf("final=%s frames=%lu time=%lld\n", codec.short_name, frames, static_cast<long long>(previousTime));
    require(frames == expected, "every picture returned");
    require(std::strcmp(codec.short_name, argv[3]) == 0, "expected final decoder");
    require(std::fclose(capture) == 0, "close capture");
    decoder = nullptr;
    file.ReleaseTrack(track);
    // Failed hardware decoders close on a worker thread; keep the process
    // alive long enough to observe their cleanup before the next test.
    snooze(250000);
    std::puts("PASS: track selection, complete ordered decode, bounds, EOF and decoder identity");
}
