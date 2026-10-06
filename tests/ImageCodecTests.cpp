// Standalone native codec regression. Built against the locked private codecs
// and WebCore's JPEGXRCodec.cpp by tools/test-image-codecs.sh.
#include "JPEGXRCodec.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
int main(int argc, char** argv)
{
    using namespace WebCore;
    std::vector<uint8_t> sample;
    for (const auto [width, height] : {std::pair{1u, 1u}, {17u, 19u}, {64u, 33u}}) {
        for (bool transparency : {false, true}) {
            std::vector<uint8_t> pixels(width * height * 4);
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i] = i / 4 * 7; pixels[i + 1] = i / 4 * 11; pixels[i + 2] = i / 4 * 13;
                pixels[i + 3] = transparency ? uint8_t(i / 4 * 17) : 255;
            }
            auto encoded = encodeJPEGXR(pixels, width, height, width * 4, 1.0);
            CHECK(encoded.size() > 32 && encoded[0] == 'I' && encoded[1] == 'I' && encoded[2] == 0xbc);
            auto info = decodeJPEGXR(encoded, true);
            CHECK(info && info->width == width && info->height == height && info->pixels.empty());
            auto decoded = decodeJPEGXR(encoded);
            if (!decoded) std::cerr << "Decode case " << width << 'x' << height << " transparency=" << transparency << " bytes=" << encoded.size() << '\n';
            CHECK(decoded && decoded->width == width && decoded->height == height);
            CHECK(decoded && decoded->pixels == pixels);
            if (!transparency && width == 17) sample = encoded;
            auto lossy = decodeJPEGXR(encodeJPEGXR(pixels, width, height, width * 4, 0.6));
            CHECK(lossy && lossy->width == width && lossy->height == height);
            if (lossy) {
                bool alphaMatches = true;
                for (size_t i = 3; i < pixels.size(); i += 4) alphaMatches &= lossy->pixels[i] == pixels[i];
                CHECK(alphaMatches);
            }
            CHECK(!decodeJPEGXR(std::span(encoded).first(encoded.size() / 2)));
            CHECK(!decodeJPEGXR(std::span(encoded).first(encoded.size() - 1)));
        }
    }
    CHECK(encodeJPEGXR({}, 1, 1, 4, {}).empty());
    CHECK(encodeJPEGXR({}, 0, 1, 4, {}).empty());
    CHECK(encodeJPEGXR({}, 0xffffffff, 0xffffffff, 0xffffffff, {}).empty());
    CHECK(!decodeJPEGXR({}));
    // Declared metadata sizes and offsets must be rejected before allocation.
    auto damaged = sample;
    const size_t directory = damaged[4] | (damaged[5] << 8) | (damaged[6] << 16) | (damaged[7] << 24);
    for (size_t i = directory + 6; i < directory + 10; ++i) damaged[i] = 0xff;
    CHECK(!decodeJPEGXR(damaged));
    std::mt19937 random(1234);
    for (unsigned trial = 0; trial < 200; ++trial) {
        std::vector<uint8_t> invalid(64 + random() % 256);
        std::generate(invalid.begin(), invalid.end(), [&] { return random(); });
        invalid[0] = invalid[1] = 'I'; invalid[2] = 0xbc; invalid[3] = 1;
        CHECK(!decodeJPEGXR(invalid));
    }
    if (argc >= 3) {
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<uint8_t> reference((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto decoded = decodeJPEGXR(reference);
        CHECK(decoded && decoded->width == 19 && decoded->height == 7 && !decoded->hasAlpha);
        if (decoded) {
            bool matches = true;
            for (unsigned y = 0; y < 7; ++y) for (unsigned x = 0; x < 19; ++x) {
                const auto* pixel = decoded->pixels.data() + (y * 19 + x) * 4;
                matches &= pixel[0] == x * 11 % 256 && pixel[1] == y * 27 % 256
                    && pixel[2] == x * y * 7 % 256 && pixel[3] == 255;
            }
            CHECK(matches);
        }
    }
    if (argc >= 2) {
        std::ofstream file(argv[1], std::ios::binary);
        file.write(reinterpret_cast<const char*>(sample.data()), sample.size());
        CHECK(file.good());
    }
    std::cout << checks << " codec checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
