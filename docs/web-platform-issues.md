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
- **#26, image formats:** still to implement and verify. The pinned engine has
  an optional libjxl decoder; the workstation only has libjxl 0.6.1, below the
  port's required 0.7.0, and no HEIF or JPEG XR library. JPEG XL and HEIC in
  html5test.co are decoding probes; **JPEG XR is a canvas encoding probe**
  (`toDataURL('image/vnd.ms-photo')`), so decoding alone cannot satisfy it.
- **#28, custom schemes:** still to implement registration plus native app
  selection, remembered choices and a system handler recommendation. The
  pinned engine has no modern `navigator.registerProtocolHandler` implementation.
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
