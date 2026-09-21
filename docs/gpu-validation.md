# Validating GL compositing

GL compositing is built only in the `ModernGL` engine configuration (see
[gpu-compositing.md](gpu-compositing.md)). This runbook applies to both the QEMU VM
(software GL, forced) and a KunanyiOS machine with a supported GPU.

## Build

```sh
SUMMIT_ENGINE_BUILD_NAME=ModernGL \
SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_HAIKU_GL_COMPOSITING=ON -DUSE_SYSTEM_MALLOC=OFF -DUSE_MIMALLOC=ON" \
  bash tools/build-webkit-in-vm.sh --modern-extensions all
SUMMIT_ENGINE_BUILD_NAME=ModernGL SUMMIT_NATIVE_BUILD_ROOT=/SummitExtensions/summit \
  bash tools/build-modern-browser-in-vm.sh --browser --bundle --modern-extensions
```

The first command is a full rebuild (about 10 hours in the VM); later changes are
incremental. `Modern` (software only, system malloc) and `ModernGL` build
directories are independent and may build at the same time.

## Run

The bundle does not ship EGL; it uses whichever `libEGL.so.1`/`libGLESv2.so.2`
the loader finds first.

| Environment | Command |
| --- | --- |
| VM, software GL | `. /SummitExtensions/mesa/summit-mesa-env.sh; SUMMIT_FORCE_GL_COMPOSITING=1 ./run-browser.sh` |
| Real GPU (KunanyiOS Mesa) | `HAIKU_CSF_DEVICE=… ./run-browser.sh` (the variable must reach the WebProcess) |
| Force software | `SUMMIT_DISABLE_GL_COMPOSITING=1 ./run-browser.sh` |

The stock Haiku Mesa 22.0.5 cannot initialize EGL, so without the private
Mesa the browser silently uses software rendering. Every WebProcess prints one
line starting `Summit GL compositing:` with the decision and the GL renderer.
On llvmpipe/softpipe the path is refused unless forced, because software GL is
slower than the software painter.

## Checks

Open `tests/fixtures/gpu/compositing.html` (hover a cell for its expected
appearance) in both modes and compare screenshots:

1. Red top-left / blue bottom-right cell: catches vertical flips and red/blue swaps.
2. 50% green over white must read (127, 208, 127) ±2 — premultiplied alpha.
3. Soft shadow, rotated purple cell and composited text must match software.
4. The orange square rotates smoothly; the fixed box stays put while scrolling.
5. Resize the window, open two tabs, and use Speedometer 3.1 (`tools/bench/`)
   with `SUMMIT_DISABLE_GL_COMPOSITING=1` versus without, to compare.
6. Force a failure (e.g. a window larger than the GPU's maximum texture size):
   rendering must continue in software after one dropped frame.

On real hardware also watch WebProcess memory over a long browsing session;
Mesa's Panfrost port has not been qualified for sustained load or GPU reset.
