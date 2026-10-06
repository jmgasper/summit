# Web platform issues, October 2026

Work for GitHub issues #25–30. This is an implementation and verification record;
entries without runtime results are still open.

- **#30, buttons:** native HVIF toolbar resources, rendered at the display scale,
  with BControl's pressed and disabled variants. Back, Forward, Stop and Home
  come from Haiku's WebPositive; the other icons are Summit vectors. Native
  browser compilation and `rc` resource compilation pass on the X399, and a
  contact sheet rendered by BIconUtils verifies all normal and disabled icons.
  Still to verify the complete bundled browser on screen.
- **#25, fullscreen:** implementation in progress. The modern engine's
  WebFullScreenManagerProxy client requests a transition on the browser window's
  looper and waits for acknowledgement before completing the DOM request.
  Requests time out after five seconds. The browser saves its frame, look,
  flags and visible controls; Escape, switching tabs, closing a tab and a
  crashed renderer restore the window. A native notice identifies the URL and
  Escape key for three seconds. Session persistence keeps the normal frame.
  Native browser compilation passes; engine build and runtime checks pending.
- **#27, OffscreenCanvas:** the source already defaulted both feature switches
  on, but the X399's SkiaCGMiPGO CMake cache still set both OFF. The modern build
  wrapper now explicitly enables them (and fullscreen) unless the caller
  supplies an override. This migrates retained build directories too. Engine
  compilation and runtime pixel tests are pending.
- **#26, image formats:** private, checksum-pinned libjxl 0.12.0, libheif
  1.23.6/libde265 1.1.3 and Debian-patched jxrlib 1.2 now build on the X399.
  `tools/prepare-image-codecs.py` installs them under
  `/boot/home/summit-deps/image-codecs-20261006` with source/file manifests and
  license notices. The OS packages are untouched. A new HEIC still-image
  decoder handles dimensions, crops/rotation, alpha and ICC conversion, with
  bounded allocations and decoded-plane checks. It passes native compilation.
  HEIC brand detection precedes the existing broad AVIF container check.
  JPEG XR now has bounded in-memory encode/decode helpers, a ScalableImageDecoder
  adapter and a Skia canvas export path (including GPU readback). The native
  standalone suite passes 256 checks covering exact pixels, transparency,
  dimensions, quality, truncation, directory limits and malformed data. A
  separately encoded 24-bit RGB fixture also matches every expected pixel.
  Native engine-object compilation passes for the codec, decoder adapter and
  Skia export path. Feature enablement/bundling and browser pixel tests are
  still pending. JPEG XL and
  HEIC in html5test.co are decoding probes;
  **JPEG XR is a canvas encoding probe** (`toDataURL('image/vnd.ms-photo')`),
  so decoding alone cannot satisfy it.
- **#28, custom schemes:** registration/unregistration now have secure-context
  bindings, scheme and same-origin validation, and a frame-origin-checked IPC
  bridge. A native asynchronous prompt approves website handlers. User-activated
  external links use a remembered app or website, or offer Haiku's MIME-preferred
  app and an application chooser. Arguments go directly to BRoster, without a
  shell. Saved choices and registration denials can be cleared in Preferences
  › Link Handlers; private choices stay in memory. Stale or background-tab
  prompt replies do nothing. Portable validation/encoding/profile tests pass;
  native browser compilation passes. Engine compilation and runtime checks
  are pending. These changes are newer
  than the first engine build and require a subsequent incremental build.
- **#29, WebUSB:** still to implement native device access and the browser API,
  with device selection and permission enforcement, then verify a real transfer.

The four portable CTest suites pass. Their profile regression had retained an
old expectation that camera permissions were unsupported; it now checks
camera grants, microphone denials and rejection of an unknown permission.

## Build and verification

The engine build started on October 6 uses the workstation's existing
`SkiaCGMiPGO` directory, preserving its trained profile and private Mesa. Use
these settings for subsequent iterations (do not start another build while
its native ninja is live):

```sh
export SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
export SUMMIT_ENGINE_BUILD_NAME=SkiaCGMiPGO SUMMIT_WEBKIT_JOBS=24
export SUMMIT_ENGINE_CXX_FLAGS='-ftrack-macro-expansion=0 -fprofile-use -fprofile-correction -fprofile-partial-training -Wno-error=coverage-mismatch -Wno-coverage-mismatch'
export SUMMIT_ENGINE_CMAKE_EXTRA='-DUSE_SKIA=ON -DUSE_HAIKU_GL_COMPOSITING=ON -DENABLE_WEBGL=ON -DENABLE_FULLSCREEN_API=ON -DENABLE_OFFSCREEN_CANVAS=ON -DENABLE_OFFSCREEN_CANVAS_IN_WORKERS=ON'
bash tools/build-webkit-in-vm.sh --modern-extensions all
bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

The first build's host log is `.vm/issues-engine-build.log`; its wrapper is
`.vm/issues-build.sh`. The installed launcher still points to `bundle-acipjxcr`
with Mesa `prefix-20261002`; do not replace it until the new bundle is verified.
The owner's running Summit is team 18408 at this point; run tests from a
separate bundle executable/profile and target only its returned team ID.

Serve `tools/bench/pages` for the runtime probes:

- `offscreen-canvas.html`: 2D pixels, PNG round trip, ImageBitmap and
  bitmaprenderer, clear/resize, main-thread and worker WebGL/WebGL2 GPU
  readback, transferred HTML canvas presentation and returned ImageBitmap.
  Completion sets `window.testDone`, with detailed `window.testResults` and
  a pass count in the title.
- `protocol-handlers.html`: API presence, reserved/invalid schemes, malformed
  templates and cross-origin rejection. Manual registration, round-trip link,
  unregistration and native app links exercise the chooser. Verify remembering,
  cancellation, removal in Preferences, private isolation and stale replies.
- `fullscreen.html`: user activation, promise resolution, fullscreenchange,
  element/viewport dimensions, exitFullscreen, Escape and iframe permission.
  `enterFullscreen()` / `exitFullscreen()` can be evaluated through the native
  Developer Tools (entry evaluations count as user gestures). Also exercise
  switching tabs, navigating away, closing the tab, and renderer termination;
  compare native window frame and controls with their pre-entry state.
  `summitctl state` includes `fullscreen` for native state verification.

The html5test.co OffscreenCanvas 2D detection uses
`context instanceof CanvasRenderingContext2D`. The standard's separate
`OffscreenCanvasRenderingContext2D` interface is the correct context type;
use the drawing and readback checks above as evidence instead of changing the
interface hierarchy solely to influence that old detector.
