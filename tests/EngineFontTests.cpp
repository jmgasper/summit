#include "config.h"
#include "FontPlatformData.h"
#include "FontCustomPlatformData.h"
#include "FontDescription.h"
#include "SharedBuffer.h"
#include <Application.h>
#include <OS.h>
#include <wtf/MainThread.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace WebCore;
static int checks = 0, failures = 0;
static void Check(bool passed, const char* label)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

int main()
{
    BApplication app("application/x-vnd.Kunanyi-Summit-FontTests");
    WTF::initializeMainThread();
    FontDescription description;
    description.setUsedSize(23.5f);
    description.setOrientation(FontOrientation::Vertical);
    description.setWidthVariant(FontWidthVariant::HalfWidth);
    description.setTextRenderingMode(TextRenderingMode::GeometricPrecision);
    FontPlatformData installed(description, AtomString("Noto Sans"_s));
    Check(installed.size() == 23.5f && installed.font()->Size() == 23.5f,
        "installed font metadata agrees with native size");
    Check(installed.orientation() == FontOrientation::Vertical
        && installed.widthVariant() == FontWidthVariant::HalfWidth
        && installed.textRenderingMode() == TextRenderingMode::GeometricPrecision,
        "native font preserves layout and text rendering metadata");
    auto resized = FontPlatformData::cloneWithSize(installed, 41);
    Check(resized.size() == 41 && resized.font()->Size() == 41
        && installed.size() == 23.5f && installed.font()->Size() == 23.5f,
        "resizing a copied font updates metadata without mutating its source");

    auto attributes = installed.attributes();
    attributes.m_font.shear = 105;
    attributes.m_font.rotation = 12;
    attributes.m_font.falseBoldWidth = 0.75f;
    attributes.m_font.flags = B_FORCE_ANTIALIASING;
    attributes.m_font.spacing = B_FIXED_SPACING;
    auto decorated = FontPlatformData::create(attributes, nullptr);
    auto restored = FontPlatformData::fromIPCData(decorated.metadata(), decorated.toIPCData());
    Check(restored && *restored == decorated && restored->isFixedPitch(),
        "installed font IPC round trip preserves native style and layout metadata");
    Check(restored && restored->font()->Shear() == 105 && restored->font()->Rotation() == 12
        && restored->font()->FalseBoldWidth() == 0.75f && restored->font()->Flags() == B_FORCE_ANTIALIASING,
        "font IPC restores shear rotation synthetic stroke and antialiasing");
    Check(restored && std::abs(restored->font()->StringWidth("Summit native font")
        - decorated.font()->StringWidth("Summit native font")) < 0.01f,
        "restored installed font has identical native text metrics");

    auto reject = [&](FontMetadata metadata, FontPlatformSerializedData data, const char* label) {
        Check(!FontPlatformData::fromIPCData(metadata, FontPlatformData::IPCData(WTF::move(data))), label);
    };
    auto native = installed.attributes().m_font;
    auto metadata = installed.metadata();
    metadata.pointSize = std::numeric_limits<float>::quiet_NaN();
    reject(metadata, native, "reject non-finite point size from IPC");
    metadata.pointSize = -1;
    reject(metadata, native, "reject negative point size from IPC");
    metadata.pointSize = std::numeric_limits<float>::infinity();
    reject(metadata, native, "reject infinite point size from IPC");
    metadata = installed.metadata();
    auto invalid = native;
    invalid.family = "Summit nonexistent font family"_s;
    reject(metadata, invalid, "reject missing installed family instead of silently substituting");
    invalid = native;
    invalid.family = String::fromUTF8("Noto Sans\0hidden"_span);
    reject(metadata, invalid, "reject embedded NUL in native font identity");
    invalid = native;
    invalid.shear = std::numeric_limits<float>::quiet_NaN();
    reject(metadata, invalid, "reject non-finite shear from IPC");
    invalid = native;
    invalid.rotation = 361;
    reject(metadata, invalid, "reject rotation outside native bounds");
    invalid = native;
    invalid.flags = 0x80000000;
    reject(metadata, invalid, "reject unsupported native font flags");
    invalid = native;
    invalid.face = 0x8000;
    reject(metadata, invalid, "reject unsupported native face bits");
    invalid = native;
    invalid.spacing = 255;
    reject(metadata, invalid, "reject unknown native spacing mode");
    invalid = native;
    invalid.encoding = 255;
    reject(metadata, invalid, "reject non-UTF-8 native font encoding");

    auto empty = SharedBuffer::create();
    Check(!FontCustomPlatformData::create(empty, emptyString()), "reject empty downloaded font safely");
    const uint8_t garbage[] = { 0, 1, 2, 3, 4, 5, 6 };
    auto malformed = SharedBuffer::create(std::span<const uint8_t>(garbage));
    Check(!FontCustomPlatformData::create(malformed, emptyString()), "reject malformed downloaded font safely");
    auto bytes = SharedBuffer::createWithContentsOfFile("/boot/system/data/fonts/ttfonts/NotoSans-Regular.ttf"_s);
    Check(bytes && bytes->size(), "read native font fixture bytes");
    if (!bytes)
        return 1;
    auto custom = FontCustomPlatformData::create(*bytes, emptyString());
    Check(!!custom, "load private downloaded face through native app_server");
    if (!custom)
        return 1;
    FontMetricsOverrides overrides;
    overrides.ascentOverride.value = 0.8f;
    FontCreationContext context({ }, { }, { }, nullptr, 1, overrides);
    std::optional<FontPlatformData> webFont = custom->fontPlatformData(description, context);
    Check(webFont->customPlatformData() == custom.get() && webFont->creationData()
        && webFont->creationData()->fontFaceData->size() == bytes->size(),
        "native web font retains downloaded data for its complete lifetime");
    Check(webFont->metricsOverrides() == overrides && webFont->size() == description.usedSize(),
        "downloaded font preserves CSS metrics overrides and used size");
    area_id originalArea = custom->m_area;
    auto serialized = custom->serializedData();
    auto identifier = serialized.renderingResourceIdentifier;
    auto rebuiltCustom = FontCustomPlatformData::tryMakeFromSerializationData(WTF::move(serialized), false);
    Check(rebuiltCustom && (*rebuiltCustom)->m_renderingResourceIdentifier == identifier,
        "custom resource reconstruction preserves rendering identifier");
    Check(rebuiltCustom && (*rebuiltCustom)->m_font.FamilyAndStyle() != custom->m_font.FamilyAndStyle(),
        "repeated downloaded fonts receive independent private native identities");
    auto locked = custom->serializedData();
    Check(!FontCustomPlatformData::tryMakeFromSerializationData(WTF::move(locked), true),
        "lockdown parser request cannot fall through to unrestricted native font parser");
    auto transferred = FontPlatformData::fromIPCData(webFont->metadata(), webFont->toIPCData());
    Check(transferred && transferred->customPlatformData() && transferred->creationData()
        && transferred->creationData()->fontFaceData->size() == bytes->size(),
        "web font IPC reconstructs a private face with retained source bytes");
    Check(transferred && transferred->creationData()
        && transferred->creationData()->fontFaceData->size() == bytes->size()
        && !std::memcmp(transferred->creationData()->fontFaceData->span().data(), bytes->span().data(), bytes->size()),
        "native identity rewriting leaves original downloaded bytes unchanged through IPC");
    Check(transferred && transferred->metadata() == webFont->metadata()
        && std::abs(transferred->font()->StringWidth("Downloaded web font")
            - webFont->font()->StringWidth("Downloaded web font")) < 0.01f,
        "web font IPC preserves native text metrics and CSS metadata");
    custom = nullptr;
    area_info info;
    Check(get_area_info(originalArea, &info) == B_OK && webFont->font()->StringWidth("Still alive") > 0,
        "native face stays usable after creator releases its reference");
    webFont.reset();
    Check(get_area_info(originalArea, &info) != B_OK,
        "last native platform font release frees its downloaded font area");
    Check(transferred && transferred->font()->StringWidth("Independent copy") > 0,
        "IPC reconstruction stays usable after original downloaded face is destroyed");

    auto cffBytes = SharedBuffer::createWithContentsOfFile("/boot/home/summit-webkit/Tools/DumpRenderTree/fonts/FontWithFeatures.otf"_s);
    Check(cffBytes && cffBytes->size(), "read upstream OpenType CFF fixture");
    if (!cffBytes)
        return 1;
    auto cff = FontCustomPlatformData::create(*cffBytes, emptyString());
    auto cffDuplicate = FontCustomPlatformData::create(*cffBytes, emptyString());
    Check(cff && cffDuplicate && cff->m_font.FamilyAndStyle() != cffDuplicate->m_font.FamilyAndStyle(),
        "OpenType CFF downloads support independent native private identities");
    Check(cff && cffDuplicate && cff->m_font.StringWidth("abc") > 0
        && cff->m_font.StringWidth("abc") == cffDuplicate->m_font.StringWidth("abc"),
        "renaming OpenType CFF preserves usable and identical glyph metrics");
    Vector<uint8_t> unalignedBytes { bytes->span() };
    while (unalignedBytes.size() % 4)
        unalignedBytes.append(0);
    unalignedBytes.append(0);
    auto unalignedBuffer = SharedBuffer::create(WTF::move(unalignedBytes));
    auto unalignedFont = FontCustomPlatformData::create(unalignedBuffer, emptyString());
    Check(unalignedFont && unalignedFont->m_font.StringWidth("abc") > 0,
        "native name rewriting supports valid sfnt with unaligned trailing bytes");
    constexpr auto collectionPath = "/boot/home/summit/build-font-tests/collection.ttc";
    BFont originalCollection;
    bool collectionSupported = originalCollection.LoadFont(collectionPath) == B_OK;
    Check(collectionSupported && originalCollection.StringWidth("abc") > 0,
        "native loader accepts generated two-face collection fixture");
    auto collectionBytes = SharedBuffer::createWithContentsOfFile(String::fromUTF8(collectionPath));
    auto collectionFont = collectionBytes ? FontCustomPlatformData::create(*collectionBytes, emptyString()) : nullptr;
    auto collectionDuplicate = collectionBytes ? FontCustomPlatformData::create(*collectionBytes, emptyString()) : nullptr;
    Check(collectionFont && collectionDuplicate
        && collectionFont->m_font.FamilyAndStyle() != collectionDuplicate->m_font.FamilyAndStyle(),
        "collection first faces support independent private native identities");
    Check(collectionSupported && collectionFont && originalCollection.StringWidth("abc")
        == collectionFont->m_font.StringWidth("abc"),
        "collection rewriting preserves native first-face glyph metrics");
    if (collectionSupported)
        originalCollection.UnloadFont();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
