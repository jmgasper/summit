// Check Media Kit's decoder selection with the same format request Summit uses.
// Run on Haiku: c++ -std=c++17 check-codec.cpp -lbe -lmedia -o check-codec
#include <File.h>
#include <MediaFile.h>
#include <MediaFormats.h>
#include <MediaTrack.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && std::strcmp(argv[2], "--select-only"))) {
        std::fprintf(stderr, "usage: %s FILE [--select-only]\n", argv[0]);
        return 2;
    }
    BFile source(argv[1], B_READ_ONLY);
    if (source.InitCheck() != B_OK) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    // HTTP playback in Summit downloads to a BFile and gives that stream to
    // BMediaFile; match that path rather than BMediaFile's entry_ref overload.
    BMediaFile file(&source);
    if (file.InitCheck() != B_OK) {
        std::fprintf(stderr, "Media Kit cannot open %s\n", argv[1]);
        return 1;
    }
    for (int index = file.CountTracks() - 1; index >= 0; --index) {
        BMediaTrack* track = file.TrackAt(index);
        media_format encoded = { };
        if (track->EncodedFormat(&encoded) != B_OK || encoded.type != B_MEDIA_ENCODED_VIDEO) {
            file.ReleaseTrack(track);
            continue;
        }
        media_format decoded = { };
        decoded.type = B_MEDIA_RAW_VIDEO;
        decoded.u.raw_video.display.format = B_RGB32;
        if (track->DecodedFormat(&decoded) != B_OK) {
            std::fprintf(stderr, "video track %d has no RGB32 decoder\n", index);
            file.ReleaseTrack(track);
            return 1;
        }
        media_codec_info codec = { };
        if (track->GetCodecInfo(&codec) != B_OK) {
            std::fprintf(stderr, "video track %d has no codec identity\n", index);
            file.ReleaseTrack(track);
            return 1;
        }
        std::printf("track=%d codec=%s short=%s size=%ldx%ld RGB32\n",
            index, codec.pretty_name, codec.short_name, long(decoded.Width()), long(decoded.Height()));
        std::fflush(stdout);
        if (argc == 3) {
            file.ReleaseTrack(track);
            return 0;
        }
        // A command-line probe has no BApplication, so use the negotiated
        // frame stride instead of constructing a BBitmap through app_server.
        std::vector<unsigned char> frame(decoded.u.raw_video.display.bytes_per_row * decoded.Height());
        int64 frames = 1;
        status_t read = frame.empty() ? B_ERROR : track->ReadFrames(frame.data(), &frames);
        std::printf("read=%ld frames=%lld\n", long(read), static_cast<long long>(frames));
        file.ReleaseTrack(track);
        return read == B_OK && frames == 1 ? 0 : 1;
    }
    std::fprintf(stderr, "no encoded video track\n");
    return 1;
}
