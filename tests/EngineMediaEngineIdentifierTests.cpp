#include "config.h"
#include "MediaPlayerEnums.h"
#include <wtf/EnumTraits.h>
#include "GeneratedMediaEngineValidator.h"
#include <algorithm>
#include <array>
#include <cstdio>

int main()
{
    using Engine = WebCore::MediaPlayerMediaEngineIdentifier;
    constexpr std::array engines {
        Engine::AVFoundation, Engine::AVFoundationMSE,
        Engine::AVFoundationMediaStream, Engine::AVFoundationCF,
        Engine::GStreamer, Engine::GStreamerMSE, Engine::Haiku,
        Engine::HolePunch, Engine::MediaFoundation, Engine::MockMSE,
        Engine::CocoaWebM, Engine::WirelessPlayback
    };
    unsigned failures = 0;
    for (unsigned value = 0; value < 256; ++value) {
        auto byte = static_cast<uint8_t>(value);
        bool expected = std::find(engines.begin(), engines.end(), static_cast<Engine>(byte)) != engines.end();
        bool actual = WTF::isValidEnum<Engine>(byte);
        if (expected != actual) {
            std::printf("FAIL: wire value %u: expected %d, received %d\n", value, expected, actual);
            ++failures;
        }
    }
    std::printf("256 native media engine wire-value checks, %u failures\n", failures);
    return failures ? 1 : 0;
}
