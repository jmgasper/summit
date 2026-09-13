#include "config.h"
#include "ShareableBitmap.h"
#include "Color.h"
#include "GraphicsContext.h"
#include "GraphicsContextHaiku.h"
#include "BitmapFrameHaiku.h"
#include "BitmapPresenterHaiku.h"
#include <Application.h>
#include <View.h>
#include <wtf/MainThread.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <atomic>
#include <thread>

using namespace WebCore;
static int checks = 0, failures = 0;
static void Check(bool passed, const char* label)
{
    ++checks;
    failures += !passed;
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}

static bool Pixel(std::span<const uint8_t> bytes, size_t stride, int x, int y,
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
{
    size_t offset = y * stride + x * 4;
    return offset + 4 <= bytes.size() && bytes[offset] == blue && bytes[offset + 1] == green
        && bytes[offset + 2] == red && bytes[offset + 3] == alpha;
}

static bool Pixel(const ShareableBitmap& bitmap, int x, int y,
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
{
    return Pixel(bitmap.span(), bitmap.bytesPerRow(), x, y, red, green, blue, alpha);
}

static bool Pixel(const BBitmap& image, int x, int y,
    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
{
    return Pixel({ static_cast<const uint8_t*>(image.Bits()), static_cast<size_t>(image.BitsLength()) },
        image.BytesPerRow(), x, y, red, green, blue, alpha);
}

static void SetPixel(ShareableBitmap& bitmap, int x, int y, uint8_t red, uint8_t green, uint8_t blue)
{
    size_t offset = y * bitmap.bytesPerRow() + x * 4;
    auto bytes = bitmap.mutableSpan();
    bytes[offset] = blue;
    bytes[offset + 1] = green;
    bytes[offset + 2] = red;
    bytes[offset + 3] = 255;
}

int main()
{
    BApplication app("application/x-vnd.Kunanyi-Summit-BitmapTests");
    WTF::initializeMainThread();

    ShareableBitmapConfiguration configuration({ 6, 5 });
    auto bitmap = ShareableBitmap::create(configuration);
    Check(bitmap && bitmap->sizeInBytes() == 120 && bitmap->bytesPerRow() == 24,
        "allocate exact native bitmap dimensions and stride");
    if (!bitmap) return 1;
    auto context = bitmap->createGraphicsContext();
    Check(context && context->hasPlatformContext() && context->platformContext()->Looper(),
        "shared drawing context owns an attached native view");
    if (!context) return 1;
    context->fillRect(FloatRect(1, 1, 2, 2), Color::red);
    context = nullptr;
    Check(Pixel(*bitmap, 1, 1, 255, 0, 0) && Pixel(*bitmap, 2, 2, 255, 0, 0),
        "context destruction flushes native drawing into shared pixels");
    Check(Pixel(*bitmap, 0, 0, 0, 0, 0, 0) && Pixel(*bitmap, 3, 3, 0, 0, 0, 0),
        "native fill respects its requested bounds");

    auto snapshot = bitmap->createPlatformImage(CopyBackingStore);
    auto reference = bitmap->createPlatformImage(DontCopyBackingStore);
    Check(snapshot && reference && snapshot->IsValid() && reference->IsValid(),
        "create copied and shared native images");
    if (!snapshot || !reference) return 1;
    Check(snapshot->Bits() != bitmap->span().data() && reference->Bits() == bitmap->span().data(),
        "copy and reference modes use independent and shared storage respectively");
    SetPixel(*bitmap, 1, 1, 0, 255, 0);
    Check(Pixel(*snapshot, 1, 1, 255, 0, 0) && Pixel(*reference, 1, 1, 0, 255, 0),
        "snapshot remains stable while reference observes later writes");
    auto handle = bitmap->createReadOnlyHandle();
    Check(handle.has_value(), "export pixels through a read-only capability");
    if (!handle) return 1;
    auto readOnly = ShareableBitmap::create(WTF::move(*handle), SharedMemory::Protection::ReadOnly);
    Check(readOnly && !readOnly->createGraphicsContext(),
        "read-only shared pixels cannot create a writable drawing context");
    if (!readOnly) return 1;
    auto readOnlyImage = readOnly->createPlatformImage(DontCopyBackingStore);
    Check(readOnlyImage && readOnlyImage->IsValid() && Pixel(*readOnlyImage, 1, 1, 0, 255, 0),
        "read-only shared pixels can be displayed as a native image");
    area_info readOnlyInfo;
    Check(readOnlyImage && readOnlyImage->Bits() != readOnly->span().data()
        && get_area_info(area_for(readOnly->mutableSpan().data()), &readOnlyInfo) == B_OK
        && !(readOnlyInfo.protection & (B_WRITE_AREA | B_CLONEABLE_AREA)),
        "displaying a read-only capability preserves its native access restrictions");

    auto source = ShareableBitmap::create({ { 8, 8 } });
    auto target = ShareableBitmap::create({ { 8, 8 } });
    if (!source || !target) return 1;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            SetPixel(*source, x, y, x < 4 ? 255 : 0, y < 4 ? 0 : 255, x >= 4 && y < 4 ? 255 : 0);
    auto painter = target->createGraphicsContext();
    if (!painter) return 1;
    painter->setImageInterpolationQuality(InterpolationQuality::Low);
    source->paint(*painter, { 1, 2 }, { 4, 0, 2, 2 });
    Check(Pixel(*target, 1, 2, 0, 0, 255) && Pixel(*target, 2, 3, 0, 0, 255),
        "paint selects the source rectangle and destination position");
    Check(Pixel(*target, 0, 0, 0, 0, 0, 0) && Pixel(*target, 3, 4, 0, 0, 0, 0),
        "paint leaves pixels outside the destination unchanged");
    source->paint(*painter, 2, { 4, 4 }, { 2, 2, 2, 2 });
    Check(Pixel(*target, 4, 4, 0, 255, 0) && Pixel(*target, 5, 5, 0, 255, 0),
        "scaled paint samples device pixels into the logical destination");
    source->paint(*painter, 0, { 0, 0 }, { 0, 0, 8, 8 });
    source->paint(*painter, -1, { 0, 0 }, { 0, 0, 8, 8 });
    source->paint(*painter, std::numeric_limits<float>::quiet_NaN(), { 0, 0 }, { 0, 0, 8, 8 });
    source->paint(*painter, std::numeric_limits<float>::infinity(), { 0, 0 }, { 0, 0, 8, 8 });
    Check(Pixel(*target, 0, 0, 0, 0, 0, 0), "invalid scale factors do not draw");
    auto transparent = ShareableBitmap::create({ { 2, 2 } });
    if (!transparent) return 1;
    painter->setCompositeOperation(CompositeOperator::Copy);
    transparent->paint(*painter, { 1, 2 }, { 0, 0, 2, 2 });
    Check(Pixel(*target, 1, 2, 0, 0, 0, 0) && Pixel(*target, 2, 3, 0, 0, 0, 0),
        "copy compositing replaces existing pixels with transparent source pixels");
    painter->setCompositeOperation(CompositeOperator::SourceOver);
    auto transparentImage = transparent->createPlatformImage();
    if (!transparentImage) return 1;
    static_cast<GraphicsContextHaiku&>(*painter).drawBitmap(transparentImage.get(), { 4, 4, 2, 2 }, { 0, 0, 2, 2 }, { CompositeOperator::Copy });
    painter->platformContext()->Sync();
    Check(Pixel(*target, 4, 4, 0, 0, 0, 0) && painter->compositeOperation() == CompositeOperator::SourceOver,
        "per-image copy options replace pixels without changing the surrounding compositing state");
    painter = nullptr;

    auto backingAddress = reference->Bits();
    auto backingArea = area_for(backingAddress);
    bitmap = nullptr;
    readOnly = nullptr;
    readOnlyImage = nullptr;
    Check(area_for(backingAddress) == backingArea && Pixel(*reference, 1, 1, 0, 255, 0),
        "native image retains its mapping after the shareable bitmap is destroyed");
    reference = nullptr;
    area_info info;
    Check(get_area_info(backingArea, &info) != B_OK,
        "last shared image reference releases the original mapping");
    Check(Pixel(*snapshot, 1, 1, 255, 0, 0), "copied image survives all original backing references");

    auto retained = ShareableBitmap::create({ { 4, 4 } });
    if (!retained) return 1;
    auto retainedHandle = retained->createReadOnlyHandle();
    auto retainedContext = retained->createGraphicsContext();
    if (!retainedHandle || !retainedContext) return 1;
    auto observer = ShareableBitmap::create(WTF::move(*retainedHandle), SharedMemory::Protection::ReadOnly);
    if (!observer) return 1;
    retained = nullptr;
    retainedContext->fillRect(FloatRect(0, 0, 4, 4), Color::blue);
    retainedContext = nullptr;
    Check(Pixel(*observer, 0, 0, 0, 0, 255) && Pixel(*observer, 3, 3, 0, 0, 255),
        "drawing context remains usable after its shareable bitmap is destroyed");

    Check(ShareableBitmapConfiguration::calculateBytesPerRow({ 0, 4 }, PixelFormat::RGBA8, ColorSpace::SRGB()).hasOverflowed(),
        "reject empty native bitmap rows");
    Check(ShareableBitmapConfiguration::calculateBytesPerRow({ std::numeric_limits<int>::max(), 1 }, PixelFormat::RGBA8, ColorSpace::SRGB()).hasOverflowed(),
        "checked stride rejects dimensions beyond native signed limits");
    Check(ShareableBitmapConfiguration::calculateBytesPerRow({ 4, 4 }, static_cast<PixelFormat>(255), ColorSpace::SRGB()).hasOverflowed(),
        "reject unsupported pixel formats");
    ShareableBitmapConfiguration colorConfiguration({ 1, 1 }, ColorSpace::LinearSRGB());
    Check(colorConfiguration.colorSpace() == ColorSpace::SRGB(), "native bitmap configuration reports its supported sRGB output");
    Check(WebKit::bitmapFrameSizeHaiku({ 801, 601 }, 1.5f) == IntSize(1202, 902),
        "frame geometry rounds fractional device pixels upward");
    Check(!WebKit::bitmapFrameSizeHaiku({ 1, 1 }, 0)
        && !WebKit::bitmapFrameSizeHaiku({ 1, 1 }, -1)
        && !WebKit::bitmapFrameSizeHaiku({ 1, 1 }, std::numeric_limits<float>::quiet_NaN())
        && !WebKit::bitmapFrameSizeHaiku({ 1, 1 }, std::numeric_limits<float>::infinity()),
        "frame geometry rejects invalid device scales");
    Check(!WebKit::bitmapFrameSizeHaiku({ 0, 1 }, 1)
        && !WebKit::bitmapFrameSizeHaiku({ 1, -1 }, 1)
        && !WebKit::bitmapFrameSizeHaiku({ std::numeric_limits<int>::max(), 1 }, 8),
        "frame geometry rejects empty and oversized native dimensions");
    Check(WebKit::bitmapFrameSizeHaiku({ 8192, 8192 }, 1) == IntSize(8192, 8192)
        && !WebKit::bitmapFrameSizeHaiku({ 8193, 8192 }, 1),
        "frame geometry bounds memory before accepting a shared frame");
    auto validFrame = source->createReadOnlyHandle();
    if (!validFrame) return 1;
    Check(WebKit::bitmapFrameHandleIsValidHaiku({ 8, 8 }, 1, *validFrame),
        "accept a frame capability matching the advertised view geometry");
    Check(!WebKit::bitmapFrameHandleIsValidHaiku({ 4, 4 }, 1, *validFrame)
        && !WebKit::bitmapFrameHandleIsValidHaiku({ 8, 8 }, 2, *validFrame),
        "reject frame geometry and scale mismatches before mapping pixels");
    auto oversizedMemory = SharedMemory::allocate(4096);
    if (!oversizedMemory) return 1;
    auto oversizedHandle = oversizedMemory->createHandle(SharedMemory::Protection::ReadOnly);
    if (!oversizedHandle) return 1;
    ShareableBitmapHandle oversizedFrame(WTF::move(*oversizedHandle), ShareableBitmapConfiguration({ 1, 1 }));
    Check(!WebKit::bitmapFrameHandleIsValidHaiku({ 1, 1 }, 1, oversizedFrame),
        "reject oversized backing objects hidden behind a small frame configuration");

    WebKit::BitmapPresenterHaiku presenter;
    SetPixel(*source, 1, 1, 1, 0, 0);
    Check(presenter.publish(1, *source, { 8, 8 }, 1), "publish a native window frame from shared pixels");
    auto firstFrame = presenter.snapshot();
    Check(firstFrame && firstFrame->identifier == 1 && firstFrame->viewSize == IntSize(8, 8)
        && firstFrame->image->Bits() != source->span().data() && Pixel(*firstFrame->image, 1, 1, 1, 0, 0),
        "native window snapshot owns immutable pixels independent of WebContent");
    SetPixel(*source, 1, 1, 2, 0, 0);
    Check(firstFrame && Pixel(*firstFrame->image, 1, 1, 1, 0, 0),
        "later shared-memory writes cannot alter an outstanding native window frame");
    Check(!presenter.publish(1, *source, { 8, 8 }, 1) && !presenter.publish(0, *source, { 8, 8 }, 1)
        && !presenter.publish(2, *source, { 7, 8 }, 1),
        "native presentation rejects stale frames and inconsistent view geometry");
    Check(presenter.publish(2, *source, { 4, 4 }, 2) && presenter.snapshot()->scale == 2,
        "native window frame retains logical size and device scale");
    std::atomic<bool> reading { true }, validSnapshots { true };
    std::atomic<unsigned> observations { 0 };
    std::thread windowReader([&] {
        while (reading.load()) {
            auto frame = presenter.snapshot();
            if (!frame || !Pixel(*frame->image, 1, 1, static_cast<uint8_t>(frame->identifier), 0, 0))
                validSnapshots = false;
            ++observations;
            std::this_thread::yield();
        }
    });
    while (!observations.load()) std::this_thread::yield();
    bool published = true;
    for (uint64_t identifier = 3; identifier <= 32; ++identifier) {
        SetPixel(*source, 1, 1, identifier, 0, 0);
        published &= presenter.publish(identifier, *source, { 8, 8 }, 1);
    }
    reading = false;
    windowReader.join();
    Check(published && validSnapshots && observations,
        "window and application threads exchange complete stable frames during replacement");
    auto finalFrame = presenter.snapshot();
    presenter.close();
    Check(!presenter.snapshot() && finalFrame && Pixel(*finalFrame->image, 1, 1, 32, 0, 0)
        && firstFrame && Pixel(*firstFrame->image, 1, 1, 1, 0, 0),
        "closing presentation preserves snapshots already retained by a drawing window");
    Check(!presenter.publish(33, *source, { 8, 8 }, 1), "closed native views reject subsequent frames");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
