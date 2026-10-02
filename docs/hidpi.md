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

## Frames straight into the screen (issue #16, 2 October 2026)

Even with every copy above trimmed, a frame at 200% still went from the GPU
to the screen the long way: the web process waited for the GPU to finish
compositing and read the changed rectangles back over the bus into shared
memory, then app_server copied them into its own copy of the screen and from
there into the frame buffer in video memory. Now the GPU copies the
composited frame into the screen itself. WebGL Aquarium at 200% (6.9 Mpx a
frame, 60 frames/s either way), CPU time in 10 s, two runs each:

| | presented directly | read back (`SUMMIT_DIRECT_PRESENT=0`) |
| --- | --- | --- |
| web process | 3.0-3.2 s | 6.5-6.7 s |
| app_server | 0.37 s | 4.9-6.0 s |
| Summit (UI) | 0.1 s | 0.2 s |
| total | 0.36 cores | 1.2 cores |

Wheel scrolling of a Wikipedia article runs at 55 frames/s either way (110
frames in a 2 s burst, no gap over 50 ms): scrolling is no longer limited by
the copies.

Three pieces, one in each layer:

- **app_server** (Haiku fork, `x399-workstation` 7a2a4f225f). A
  `BDirectWindow` is told where it is on the screen and which parts of it
  are visible, and may draw into the frame buffer itself; at 200% such
  windows used to be disconnected, because the frame buffer is not in the
  coordinates they draw in. With the new window flag
  `B_DIRECT_DEVICE_PIXELS` a window stays connected and gets its bounds and
  clipping in frame buffer pixels, the density (`device_scale`, in percent)
  and app_server's own copy of the screen (`drawing_bits_area`).
  `docs/x399-workstation/tests/directscale.cpp` in the fork shows the use;
  airTime draws films through it too.
- **Mesa** (Summit's private build, patch 06,
  `tools/mesa-vm/mesa-25.3.6-summit-06-present-into-screen.patch`,
  `/boot/home/summit-mesa/prefix-20261002` on the X399).
  `summit_haiku_present_framebuffer()`, exported from `libEGL_mesa` and
  found with `get_image_symbol()` (EGL's dispatch never sees it), has Zink
  copy the bound framebuffer object into the visible rectangles of the frame
  buffer through NVK's `VkImportScanoutMemoryHAIKU`, video memory to video
  memory, at most two presents in flight. A helper thread reports each one
  finished by waiting on the screen's timeline semaphore; it must never wait
  through Zink's fences, which belong to the context's thread (a first
  version did, and marked recycled batches idle).
  `tools/mesa-vm/scanout-present-probe.cpp` presents 3800 frames/s of
  800x500, 480/s of 3824x2096, and random small damage rectangles.
- **The engine and Summit.** `BrowserWindow` is a `BDirectWindow` with the
  flag and hands each `DirectConnected()` to
  `BWebKitView::WindowDirectConnected()`. Each web view works out where its
  page is in frame buffer pixels and which parts of it show, and puts that in
  a page of memory it shares with its web process (`DirectPresentHaiku.h`).
  The web process presents each frame through Mesa instead of waiting for
  the GPU and reading it back, sends `DirectFrame` instead of `Frame`, and
  the view draws nothing. A change of place or clipping is written under a
  lock the web process presents under; the UI process waits there until the
  GPU has finished the copies made for the old place, and the page is then
  presented again in full (`RedisplayDirect`). When the window is not
  connected (the screen blanker blanks, a workspace is switched) or Mesa
  cannot present, frames are read back as before, the first one in full.
  `SUMMIT_DIRECT_PRESENT=0` turns it all off; `SUMMIT_DIRECT_PRESENT_TRACE=1`
  logs each place worked out and each target a web process gets.

**app_server's copy of the screen.** It is what screenshots and the VNC
server read and what app_server copies back when a window moves away, so it
must hold the page too. Mesa can copy into it as well (imported host
memory), but that is off unless `SUMMIT_DIRECT_PRESENT_BACK=1`: a web
process that ends while the driver holds those pages for the GPU hangs the
X399 (Haiku deletes the address space before it closes the driver), and a
crash ends a process that way. Instead a page that changes goes through
app_server, read back as before, four times a second; the UI holds that
frame's `FrameDone` until the view has drawn it (100 ms at most), so the
older pixels never cover a newer direct frame.

**Two ways to hang the X399 found on the way** (both kernel faults, reported
to the OS work; no ping on either interface, nothing in the syslog):

- *fork() with the frame buffer mapped.* A connected direct window maps the
  frame buffer (video memory) into the application. WebKit's launcher used
  `fork()` and `exec`, and the fork copies that mapping copy-on-write: the
  first web process launched after the window was connected hung the
  machine, every time. Web and network processes now start with
  `load_image()` (`HaikuProcess.h`), a new team with an address space of
  its own, which also no longer copies the browser's whole address space
  for each launch.
- *A process ending with GPU-imported host memory*, above.
