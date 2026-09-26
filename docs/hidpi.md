# High density screens

Since September 26, 2026 app_server can draw the desktop at the density of its
monitors (Haiku commit 908c1fb3fb, `app_server: draw the screen at the density
of its monitors`). On the workstation's two 24-inch 4K monitors at 200% the
desktop is 3840x1080 logical pixels and the frame buffer 7680x2160. Programs
keep drawing in logical coordinates; app_server scales every coordinate on its
way into the buffer, renders glyphs at the larger size and enlarges bitmaps.

Text a program draws with `DrawString` is therefore sharp without any change,
but a bitmap supplied at the logical size is enlarged pixel for pixel. Summit
rendered its pages into logical-size bitmaps, so web text was soft.

## What Summit does

- **The engine asks for the density.** `displayScaleFactorHaiku()`
  (`UIProcess/API/haiku/WebKitView.cpp`) reads the private
  `BPrivate::get_display_layout()` and takes the `render scale` of the enabled
  monitors: every monitor reports the frame buffer's single density (monitors
  set to a larger scale are enlarged by the display engine, which cannot
  shrink). It is looked up with `dlsym`, so the engine still loads on a Haiku
  without the call (scale 1), and cached for a second because every
  `BWebKitView::Draw` refreshes the view geometry.
  `SUMMIT_DEVICE_SCALE_FACTOR=1` (or 1.5, 2...) overrides it.
- **Pages render at that device scale.** The geometry the view sends to the
  engine carries the scale, and `WebView::updateGeometry()` sets the page's
  intrinsic device scale factor, so `devicePixelRatio` is 2, `srcset` picks
  the 2x images, the compositor's surface has device pixels and fonts are
  rasterised at twice the size. Video frames and canvases are composited at
  the same density.
- **Frames reach app_server one to one.** `BWebKitView::Draw` already drew the
  frame with `DrawBitmap(bitmap, sourceInDevicePixels, destinationInLogical)`;
  app_server scales the destination by the same factor, so the bitmap is
  copied unscaled. `AcceleratedBackingStore` now reports the frame's logical
  size (device size / scale), and the presenter accepts a device size one
  pixel off the expected one, since the compositor truncates where the view
  rounds up at fractional scales.
- **Browser chrome.** Toolbar, tabs and menus are drawn with vector
  primitives and `DrawString`, so they are sharp already. Favicons are now
  stored at 32 pixels and extension toolbar icons come from the engine in a
  32 pixel version (`icon_bgra_32`); both are drawn into the same 16 point
  squares (filtered down on a 100% screen). `BWebKitDisplayScale()` is public
  for such cases.

## What 200% costs, and what was done about it

Each frame travels from the GPU to the screen through full-frame copies, and
at 200% a 1920x913 logical view is 7 Mpx (28 MB) per frame.
`SUMMIT_PRESENT_STATS=1` prints the time of each stage every two seconds.
Scrolling Wikipedia (`tools/bench/run-scroll.py`) on the GTX 1070:

| Stage | 1x (1.75 Mpx) | 2x (7 Mpx), before | 2x, after |
| --- | --- | --- | --- |
| Web process: GPU composite + readback | 5.3 ms | 11.5 ms | 11.5 ms (mostly GPU compositing, see below) |
| UI process: copy into a BBitmap | 1.1 ms | 4.3 ms | none |
| app_server: DrawBitmap + Sync | 0.04 ms | 0.03 ms | 0.05 ms |
| Scrolling, frames/s per second of burst | 59-60 | 41-52 | 53-59 |

- **Zero-copy presentation.** The web process now shares each frame buffer
  writable (app_server can only draw an area it may clone for writing), and
  the UI process wraps it in an area-backed `BBitmap` that app_server draws in
  place. The view holds the frame it shows; its buffer returns to the web
  process only when a newer frame replaces it (the swap chain has up to four).
  `SUMMIT_ZERO_COPY_PRESENT=0` restores the copy.
- **Pixel buffer readback.** `tools/mesa-vm/readback-probe.cpp` measured
  reading a 4K frame on zink/NVK: `glReadPixels` into client memory 8.9 ms;
  into a pixel buffer object, then map and copy, 0.03 + 0.9 + 4.4 ms (the copy
  runs at memcpy speed, 4.4 ms for 28 MB). The web process now reads through a
  PBO (`SUMMIT_PBO_READBACK=0` reverts). In the browser the stage still takes
  about 11.5 ms because mapping waits for the GPU to finish compositing the
  frame.

Screenshots and the VNC server read the screen through `BScreen`, which
averages a scaled desktop down to its logical size, so neither shows whether
text is sharp. `tools/bench/fbgrab.py` dumps the real frame buffer with
`nvscanout --dump` and crops it.
