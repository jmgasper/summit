# KunanyiOS / Haiku platform issues found while porting Summit

These are operating-system behaviours that Summit currently works around in its
WebKit port. Each one has a proper fix in the OS; the workarounds can be removed
once KunanyiOS ships those fixes. Found on Haiku R1/beta6 hrev59866+79 x86_64.

## 1. app_server drops text and bitmaps inside `BView::BeginLayer()` layers

**Symptom.** Any page content inside an element with `opacity < 1` lost its text
and images; only lines and rectangles were painted. `https://example.com/`
(whose body `div` has `opacity: 0.8`) rendered as an empty page with a lone
link underline.

**Cause.** `Layer::RenderToBitmap()` sizes the layer bitmap from the bounding
box of the recorded picture (`PictureBoundingBoxPlayer`). In
`src/servers/app/PictureBoundingBoxPlayer.cpp` two callbacks are still stubs:

```cpp
void BoundingBoxCallbacks::DrawPixels(...)           { BRect dest = _dest; /* TODO */ }
void BoundingBoxCallbacks::DrawStringLocations(...)  { /* TODO */ }
```

WebKit draws every glyph run with `BView::DrawString(string, locations, count)`
and every image with `DrawBitmap`, so neither contributes to the bounding box
and both are clipped away when the layer is composited.

**Workaround in Summit.** `GraphicsContextHaiku::beginTransparencyLayer()`
records a fully transparent `FillRect(clipBounds())` as the first operation of
each layer, so the layer always covers the clipped area
(`Source/WebCore/platform/graphics/haiku/GraphicsContextHaiku.cpp`). This costs a
larger layer bitmap than strictly necessary.

**Proper OS fix (suggested, untested).** Implement both callbacks:

```cpp
void
BoundingBoxCallbacks::DrawPixels(const BRect&, const BRect& _dest, uint32, uint32,
	size_t, color_space, uint32, const void*, size_t)
{
	BRect dest = _dest;
	fState->PenToLocalTransform().Apply(&dest);
	fState->IncludeRect(dest);
}

void
BoundingBoxCallbacks::DrawStringLocations(const char* string, size_t length,
	const BPoint locations[], size_t locationCount)
{
	ServerFont font = fState->GetDrawState()->Font();
	font_height height;
	font.GetHeight(height);
	// Conservative per-glyph box: one em wide around every glyph origin.
	const float size = font.Size();
	for (size_t i = 0; i < locationCount; i++) {
		BPoint origin = locations[i];
		fState->PenToLocalTransform().Apply(&origin);
		BRect rect(origin.x - size, origin.y - ceilf(height.ascent) - 1,
			origin.x + 2 * size, origin.y + ceilf(height.descent) + 1);
		fState->IncludeRect(rect);
	}
}
```

## 2. System `malloc` is slow under multi-threaded load

mimalloc's own `test-stress 8 20 25` workload takes **6.1 s** with the system
allocator (5.9 s of that in the kernel) against **0.79 s** with mimalloc built
from WebKit's bundled copy — about 7.8x. Summit's engine is switching from
`USE_SYSTEM_MALLOC` to WebKit's bundled mimalloc; other allocation-heavy
KunanyiOS software would benefit from the same, or from a faster libroot
allocator.

## 3. BFS has no hard links

WebKit's network disk cache stores large response bodies once and hard-links
them from each record. Every such store failed on BFS ("Failed to create hard
link…"), so large resources were never served from the disk cache. Summit now
stores the body with its record on Haiku (`NetworkCacheBlobStorage.cpp`), giving
up de-duplication of identical bodies.

## 4. Native notifications cannot call back into the sender

`BNotification` can launch an application or open a file on click, but offers no
activation callback. Extension `notifications.onClicked`/`onButtonClicked`
therefore never fire; `create/update/clear/getAll` work.

## 5. Stock Mesa 22.0.5 EGL does not initialize

`eglInitialize()` fails with `EGL_NOT_INITIALIZED` on the stock x86_64 packages,
so GL compositing cannot be exercised there. KunanyiOS' own Mesa 25.3 port has a
working Haiku EGL platform (pbuffers and windows); see `docs/vm-egl-stack.md`
once the private VM build is recorded.

## 6. The audio mixer crashes when a sound player connects and no audio output exists

**Symptom.** Opening https://www.reddit.com/ (which autoplays video) crashed
`media_addon_server` on a machine without sound hardware (the QEMU VM):
`Segment violation` in `AudioMixer::Connected` (`mixer.media_addon` + 0x126).
Every later sound playback in the session failed until the server was restarted.

**Cause.** In `src/add-ons/media/media-add-ons/mixer/AudioMixer.cpp`,
`AudioMixer::Connected()` auto-starts the mixer when its first input connects:

```cpp
if (fAutoStop && fCore->CountInputs() == 1) {
	...
	roster->NodeIDFor(fCore->Output()->MediaOutput().source.port)   // Output() is NULL
```

`fCore->Output()` is null when no output (sound card) is connected, so any
application that creates a `BSoundPlayer` crashes the add-on server.

**Workaround in Summit.** The media player creates a sound player only when
`BMediaRoster::GetAudioOutput()` succeeds and the decoded audio format has no
wildcards; otherwise the media plays without sound (`MediaPlayerPrivateHaiku.cpp`).

**Proper OS fix (suggested).** Skip the auto-start while there is no output:

```cpp
if (fAutoStop && fCore->CountInputs() == 1 && fCore->Output() != NULL) {
```
