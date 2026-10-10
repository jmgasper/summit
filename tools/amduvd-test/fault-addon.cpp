// Copyright 2026 Summit contributors. Distributed under the MIT license.
// Test-only proxy: delegates to a private copy of the real amduvd add-on.
// It must never be shipped or left in a Media Kit add-on directory.
#include <private/media/DecoderPlugin.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>

using namespace BPrivate::media;

class FaultDecoder final : public Decoder {
public:
    explicit FaultDecoder(Decoder* decoder) : fDecoder(decoder) {
        class Forwarder final : public ChunkProvider {
        public:
            explicit Forwarder(FaultDecoder& owner) : fOwner(owner) { }
            status_t GetNextChunk(const void** data, size_t* size, media_header* header) override {
                return fOwner.GetNextChunk(data, size, header);
            }
        private:
            FaultDecoder& fOwner;
        };
        fDecoder->SetChunkProvider(new Forwarder(*this));
        if (const char* value = getenv("SUMMIT_TEST_AMDUVD_FAIL_AFTER")) {
            char* end = nullptr;
            long number = strtol(value, &end, 10);
            if (end != value && !*end && number >= 0 && number <= 100000)
                fFailAfter = number;
        }
    }
    void GetCodecInfo(media_codec_info* info) override { fDecoder->GetCodecInfo(info); }
    status_t Setup(media_format* input, const void* data, size_t size) override {
        fFrames = 0;
        return fDecoder->Setup(input, data, size);
    }
    status_t NegotiateOutputFormat(media_format* format) override {
        status_t status = fDecoder->NegotiateOutputFormat(format);
        // Only used with FAIL_AFTER=0: the real decoder must never write
        // into this deliberately undersized output configuration.
        if (status == B_OK && getenv("SUMMIT_TEST_AMDUVD_BAD_WIDTH") && fFailAfter == 0) {
            format->u.raw_video.display.line_width -= 16;
            format->u.raw_video.display.bytes_per_row -= 64;
            fprintf(stderr, "Summit TEST ONLY: injected undersized output geometry\n");
        }
        return status;
    }
    status_t SeekedTo(int64 frame, bigtime_t time) override {
        fFrames = 0;
        if (getenv("SUMMIT_TEST_AMDUVD_FAIL_SEEK")) {
            fprintf(stderr, "Summit TEST ONLY: injected hardware seek reset failure\n");
            return B_ERROR;
        }
        return fDecoder->SeekedTo(frame, time);
    }
    status_t Decode(void* buffer, int64* count, media_header* header, media_decode_info* info) override {
        if (fFrames == fFailAfter) {
            fprintf(stderr, "Summit TEST ONLY: injected amduvd failure after %ld output frames\n", fFrames);
            *count = 0;
            return B_ERROR;
        }
        status_t status = fDecoder->Decode(buffer, count, header, info);
        if (status == B_OK)
            fFrames += *count;
        return status;
    }
private:
    std::unique_ptr<Decoder> fDecoder;
    long fFrames { 0 };
    long fFailAfter { -1 };
};

class FaultPlugin final : public DecoderPlugin {
public:
    FaultPlugin() {
        const char* path = getenv("SUMMIT_TEST_AMDUVD_REAL");
        if (!path || !*path)
            return;
        fImage = load_add_on(path);
        if (fImage < 0)
            return;
        MediaPlugin* (*instantiate)() = nullptr;
        if (get_image_symbol(fImage, "instantiate_plugin", B_SYMBOL_TYPE_TEXT,
                reinterpret_cast<void**>(&instantiate)) != B_OK || !instantiate)
            return;
        fBase.reset(instantiate());
        fPlugin = dynamic_cast<DecoderPlugin*>(fBase.get());
    }
    ~FaultPlugin() {
        fBase.reset();
        if (fImage >= 0)
            unload_add_on(fImage);
    }
    Decoder* NewDecoder(uint index) override {
        if (!fPlugin)
            return nullptr;
        Decoder* decoder = fPlugin->NewDecoder(index);
        return decoder ? new FaultDecoder(decoder) : nullptr;
    }
    status_t GetSupportedFormats(media_format** formats, size_t* count) override {
        *formats = nullptr;
        *count = 0;
        return B_NOT_SUPPORTED;
    }
private:
    image_id fImage { -1 };
    std::unique_ptr<MediaPlugin> fBase;
    DecoderPlugin* fPlugin { nullptr };
};

extern "C" _EXPORT MediaPlugin* instantiate_plugin() { return new FaultPlugin; }
