# The Haiku workstation: building, the GPU stack and benchmarking

Summit's second Haiku machine is a bare-metal workstation, and it is the only
one with a GPU. Everything in this document was set up and verified on
September 21, 2026; the QEMU VM ([VM.md](VM.md)) is unchanged and remains the
default target of every tool.

| | |
| --- | --- |
| Reached with | `bash tools/ws.sh '<command>'` (key in `.vm/ws_id_ed25519`) |
| CPU | AMD Ryzen Threadripper 1950X, 16 cores / 32 threads, 3.4 GHz |
| Memory | 64 GiB |
| GPU | NVIDIA GeForce GTX 1070 (GP104), 1920x1080 at 60 Hz |
| System | Haiku R1/beta6 hrev60097+49 x86_64 (the owner's own build) |
| Disk | 238 GiB NVMe, about 170 GiB free |
| Screen | VNC on port 5900 (`vncserver` starts at login), `screenshot -s -f png` over SSH |

## Building Summit natively

The workstation builds the engine about fifteen times faster than the VM (a
complete `ModernGL` configuration in roughly an hour and a half at `-j24`
instead of about ten hours), and it is where GPU compositing can actually be
tested. The build scripts take the machine as an environment variable:

```sh
export SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_REMOTE_TAG=ws
SUMMIT_ICU_JOBS=24 bash tools/build-icu-in-vm.sh            # once
SUMMIT_ENGINE_BUILD_NAME=ModernGL SUMMIT_WEBKIT_JOBS=24 \
  SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_HAIKU_GL_COMPOSITING=ON -DUSE_SYSTEM_MALLOC=OFF -DUSE_MIMALLOC=ON" \
  bash tools/build-webkit-in-vm.sh --modern-extensions all
bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

`SUMMIT_REMOTE_TAG` only names the host-side lock and result files, so a VM
build and a workstation build can run at the same time.

One flag is worth changing here. The engine is compiled with
`--param ggc-min-expand=10`, which holds GCC's garbage collector to a tenth of
its usual growth so that six parallel jobs fit in the VM's 20 GiB. With 64 GiB
it only costs time: one WebCore unified source took **97 s with the parameter
and 57 s without**, and the build peaked at 31 GiB of 64 GiB. Set

```sh
SUMMIT_ENGINE_CXX_FLAGS=-ftrack-macro-expansion=0
```

for a full build here. It changes `CMAKE_CXX_FLAGS`, so it reconfigures CMake
and rebuilds everything: choose it when starting a build, not in the middle. Guest paths are the
ordinary ones (`/boot/home/summit-webkit-extensions`, `/boot/home/summit`);
the boot volume has room, so nothing needs a second volume as in the VM.

### Installing packages

**`pkgman` cannot download on this machine**: every repository access fails with
`Operation not supported`, although `curl` reaches the same URLs. Packages are
therefore fetched by hand and installed from the file:

```sh
curl -O https://haikuports-repository.cdn.haiku-os.org/master/x86_64/current/packages/<name>-<version>-x86_64.hpkg
pkgman install -y ./<name>-<version>-x86_64.hpkg
```

Two things to know. Dependencies still have to be fetched the same way, one
round at a time (`pkgman` prints them as "from repository"). And copying an
`.hpkg` straight into `/boot/system/packages` does *not* activate it; it wedges
`package_daemon` into "Another package operation is already in progress", which
`kill <package_daemon team>` clears (`launch_daemon` restarts it).

Added for the engine build: `gperf`, `ruby`, `nasm`, `patch`, `diffutils` and
the `_devel` packages for icu74, libxml2, libxslt, libjpeg_turbo, libpng16,
libwebp, lcms, libavif, woff2, libexecinfo, libiconv, libpsl, dav1d and rav1e.

## OpenGL on the GeForce: zink over NVK

Summit's compositor needs **EGL with OpenGL ES**
([gpu-compositing.md](gpu-compositing.md)). Haiku's own Mesa cannot provide it,
and neither can the Zink add-on that is already installed here:

- `mesa` 22.0.5 from HaikuPorts has EGL disabled on purpose ("Does not work"),
  so `eglInitialize` returns `EGL_NOT_INITIALIZED` — the same wall as in the VM
  ([vm-egl-stack.md](vm-egl-stack.md)).
- `~/config/non-packaged/add-ons/opengl/Zink` is a **`BGLView`** renderer. It
  serves desktop OpenGL through `libGL`, which no part of WebKit calls.

What does work is the private Mesa 25.3.6 from `tools/mesa-vm/`, built with the
`zink` Gallium driver instead of the software rasterizers, so GLES becomes
Vulkan and Vulkan is NVK:

```sh
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_MESA_ROOT=/boot/home/summit-mesa \
  SUMMIT_MESA_JOBS=10 SUMMIT_MESA_DRIVERS=zink bash tools/mesa-vm/build.sh upload
SUMMIT_REMOTE_SHELL=tools/ws.sh SUMMIT_MESA_ROOT=/boot/home/summit-mesa \
  SUMMIT_MESA_JOBS=10 SUMMIT_MESA_DRIVERS=zink bash tools/mesa-vm/build.sh run \
  check clean tools glvnd mesa-prepare mesa-configure mesa-build mesa-install probe
```

Two upstream gaps had to be closed
(`tools/mesa-vm/mesa-25.3.6-summit-02-haiku-egl-zink.patch`):

- `src/egl/meson.build` gives the Haiku platform `driver_swrast` and
  `driver_panfrost` but not `driver_zink`, so `GALLIUM_ZINK` was never defined
  and every zink branch in the target helpers compiled out.
- `inline_sw_helper.h` calls `zink_create_screen()` without including
  `zink/zink_public.h` (`sw_helper.h`, its sibling, does include it).

`egl_haiku.cpp` then prefers a zink screen over the software one, unless the
display asks for software or `LIBGL_ALWAYS_SOFTWARE=1` is set.

Using it needs one variable, because Haiku's `runtime_loader` takes
`LIBRARY_PATH` as the complete search path:

```sh
LIBRARY_PATH=/boot/home/summit-mesa/prefix/lib:/boot/system/lib <program>
```

The Vulkan loader finds NVK by itself here; no `VK_ICD_FILENAMES` is needed.
`tools/bench/*` pass this through `SUMMIT_LIBRARY_PATH_PREFIX`.

Verified by `tools/mesa-vm/`'s probes:

```
GL_RENDERER=zink Vulkan 1.3(NVIDIA GeForce GTX 1070 (NVK GP104-A) (MESA_NVK))
GL_VERSION=OpenGL ES 3.1 Mesa 25.3.6
clear+readback 1024x768: 1.45 ms/frame
surfaceless / pbuffer / second thread: 64x64 scene exact, 0 bad pixels
1024x768 blended textured quad: 0.55 ms/frame draw+finish, 1.83 ms draw+readback
```

Known limitation: the probe's last check, a display from
`eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)`, **hangs** with zink. No
Summit code path uses that platform (`LayerTreeHostHaiku` uses
`eglGetDisplay(EGL_DEFAULT_DISPLAY)`), and every check before it passes.

## Hardware video decoding (NVDEC)

The owner's NVDEC decoder is installed as an ordinary Media Kit plugin in
`~/config/non-packaged/add-ons/media/plugins/nvdec`, so anything that decodes
through `BMediaFile`/`BMediaTrack` gets it without knowing. Summit's
`MediaPlayerPrivateHaiku` does exactly that. Confirmed with the owner's own
tool:

```
$ /boot/home/build/nvdec/mediadecode /boot/home/bbb12s.mp4 60
decoder: H.264 on the graphics card (NVDEC)
1920x1080, 7680 bytes a line, 30.00 pictures a second, 366 in all
60 pictures in 927.6 ms, 15.46 ms each (64.7 a second)
```

The plugin decodes **H.264 only** and hands back `B_RGB32`.

### Hardware video decoding in Summit

Summit reaches the plugin: a WebProcess playing a 1080p H.264 file has its
`video decoder` thread inside `nvdecH264Decode()` and uses 0.11–0.25 cores.
It then **crashes there**, and the crash is in the plugin, not in Summit:

```
thread 49419: video decoder   state: Exception (Segment violation)
  strlen + 0x32
  vfprintf / vsnprintf / __snprintf
  nvdecH264Decode(NvdecH264*, const uint8_t*, size_t, int64_t) + 0x6fb
      (/boot/home/build/nvdec/nvdec_h264.c:237)
  decoder: { reason: Bad address ... }
```

`NVDecPlugin.cpp` declares `char reason[256]` as a local of the setup function
and hands it to `nvdecH264Create(fEngine, reason, sizeof(reason))`, which keeps
the pointer in `decoder->reason`. Once setup returns, that stack is reused, and
the first `snprintf(why, sizeof(why), "%s", decoder->reason ...)` during
decoding reads freed stack. Giving the buffer the decoder's lifetime (a member
array) should be all it needs. Reports are kept in
`/boot/home/summit/bench/reports/`.

With the plugin moved aside the ffmpeg decoder renders the same file, so the
rest of Summit's media path works; playback then stalls with
`partial file` / `Invalid NAL unit size`, which is `BMediaFile` reading the URL
itself rather than anything in the page. `ENABLE_MEDIA_SOURCE` is off, so
YouTube, which needs Media Source Extensions, cannot use any of this yet.

## Firefox, for comparison

Firefox 155.0 is installed at `/boot/system/apps/Firefox/Firefox`.
`tools/bench/run-firefox-speedometer.py` runs the same local Speedometer 3.1
checkout in it; the page posts its result back to this host, so the score comes
from JSON rather than from a screenshot.

## Things that skew a measurement here

- **The screen blanker stops rendering.** After a few idle hours app_server
  sends no Draw messages, so the UI process never acknowledges a frame, the web
  process waits forever and the browser looks frozen (one run reported
  `present=602561 ms`). `guest.wake_display()` stops `screen_blanker` before
  and during a run.
- `vncserver` runs from the owner's boot launch folder and polls the frame
  buffer continuously, using about 3% of one core. Stop it for a quiet run.
- A parallel engine build saturates all 32 threads; `run-speedometer.py
  --wait-quiet` refuses to start until the machine is idle.
- `screenshot -s -f png <file>` exits non-zero even when it wrote the file.
- `~/config/settings/system/debug_server/settings` now asks for a report
  instead of a dialog when `WebProcess` or `NetworkProcess` crashes, so an
  unattended run is not stopped by an alert waiting to be answered.

## fontconfig has no conf.d

Haiku's `fontconfig` package installs `/boot/system/data/fontconfig/conf.avail`
but no `conf.d`, and `/boot/system/settings/fonts/fonts.conf` includes `conf.d`
with `ignore_missing="yes"`. None of the standard configuration is therefore
active -- including `45-generic.conf` and `60-generic.conf`, which define the
`serif`, `sans-serif` and `monospace` aliases.

`fc-match` conceals it, because it answers every query with a best effort, but a
program asking fontconfig to match a family gets nothing back. In Summit's Skia
build that crashed the web process on every Speedometer run: the last-resort
fallback fell through to `SkTypeface::MakeEmpty()`, and the empty font tripped a
hash table assertion (see [performance](performance.md)).

The fix is the ordinary fontconfig layout:

```
D=/boot/system/settings/fonts/conf.d
A=/boot/system/data/fontconfig/conf.avail
mkdir -p "$D"
for c in 10-hinting-slight 10-scale-bitmap-fonts 10-yes-antialias \
         11-lcdfilter-default 20-unhint-small-vera 25-unhint-nonlatin \
         30-metric-aliases 35-lang-normalize 40-nonlatin 45-generic 45-latin \
         48-guessfamily 48-spacing 49-sansserif 50-user 51-local 60-generic \
         60-latin 65-fonts-persian 65-khmer 65-nonlatin 69-unifont \
         80-delicious 90-synthetic; do
    ln -sf "$A/$c.conf" "$D/$c.conf"
done
```

Afterwards `fc-match serif`, `sans-serif` and `monospace` answer Noto Serif,
Noto Sans and Noto Sans Mono; before, all three answered Noto Sans.
