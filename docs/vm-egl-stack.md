# Private EGL + OpenGL ES stack in Summit's Haiku VM

Summit's x86_64 test VM (see [VM.md](VM.md)) now has a working offscreen
EGL + OpenGL ES 2/3 stack on software GL: Mesa 25.3.6 with llvmpipe (LLVM 21.1.8
JIT) and softpipe, behind libglvnd 1.7.0. It is installed privately under
`/SummitExtensions/mesa/prefix`. The system `mesa`, `mesa_devel` and
`mesa_swpipe` 22.0.5 packages are untouched, nothing was installed with
`pkgman`, and nothing is written to `/boot`.

Validated on 2026-09-19 (guest clock 2026-09-18/19 UTC): `eglInitialize`
succeeds, GLES2 contexts become current on a pbuffer, surfaceless, on a second
thread and on an `EGL_PLATFORM_SURFACELESS_MESA` display; FBOs are complete;
shaders, shared-context textures, blending and scissor produce exact pixels;
`glReadPixels` returns the cleared colour `64,128,191,255`.

This qualifies offscreen rendering for compositor bring-up. It is not GLES
conformance, and no WebKit code has used it yet.

## Why the system EGL cannot be used

`/system/lib/libEGL.so.1` is Mesa 22.0.5 from HaikuPorts revision 3. The probe
fails with `eglInitialize failed 0x3001` (`EGL_NOT_INITIALIZED`) and
`EGL_LOG_LEVEL=debug` prints only `Native platform type: haiku`. No environment
setting can change this:

- HaikuPorts' `mesa-22.0.5.patchset` contains the commit "Disable EGL on
  Haiku." (Augustin Cavalier, 2024-09-09). It makes `init_haiku()` in
  `src/egl/drivers/haiku/egl_haiku.cpp` return `EGL_FALSE` unconditionally,
  with the comment "Does not work."
- The code it disables is a `BGLView` stub in any case:
  `haiku_create_pbuffer_surface()` returns `NULL`, the single config advertises
  `EGL_WINDOW_BIT` and `EGL_OPENGL_BIT` only (no pbuffer, no ES2), and
  `haiku_create_context()` creates no GL context; it relies on
  `BGLView::LockGL()`.

`--app` (a `BApplication`), `LIBGL_*`, `MESA_*`, `EGL_PLATFORM` and add-on
paths are therefore irrelevant. The `Software Pipe` add-on in
`/system/add-ons/opengl` serves `BGLView` desktop GL only.

## What is installed

| Item | Value |
| --- | --- |
| Root | `/SummitExtensions/mesa` (BFS build volume; about 610 MiB used) |
| Prefix | `/SummitExtensions/mesa/prefix` (26 MiB) |
| Environment file | `/SummitExtensions/mesa/summit-mesa-env.sh` |
| Probes | `/SummitExtensions/mesa/probe/eglprobe`, `.../gles2-draw-probe` |
| Logs | `/SummitExtensions/mesa/logs/` (`20260918T235811-all.log` is the from-scratch run) |
| Mesa | 25.3.6, `mesa-25.3.6.tar.xz` SHA-256 `59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206` |
| libglvnd | 1.7.0, `libglvnd-v1.7.0.tar.gz` SHA-256 `2b6e15b06aafb4c0b6e2348124808cbd9b291c647299eaaba2e3202f51ff2f3d` |
| Toolchain | GCC 13.3.0_2023_08_10-6, LLVM 21.1.8-2 (`llvm21`, `llvm21_libs`), Ninja 1.13.2, Python 3.10.21 |
| Guest | Haiku R1/beta6 hrev59866+79 x86_64 |

Libraries in `prefix/lib`:

| Library | Source | SHA-256 of this build |
| --- | --- | --- |
| `libEGL.so.1` (`.1.1.0`) | libglvnd dispatcher | `507fc522d9853cae48ab263543d5b5e995126d5ae41c5d19d7fd9ab3e4366daa` |
| `libGLESv2.so.2` (`.2.1.0`) | libglvnd | `fe9aef0c5bc1505a2a03701fb9555f54dbfcf7343be7854bf585689b00b023d6` |
| `libGLdispatch.so.0` | libglvnd | `68f1b748ba6aac2a31b7294c4abcf6454ffecc3709ad28cfc067e7834ebdfd9a` |
| `libOpenGL.so.0`, `libGL.so.1` | libglvnd (`libGL` is its Haiku `BGLView`-over-EGL layer) | not recorded |
| `libEGL_mesa.so.0` (`.0.0.0`, 20 MiB) | Mesa vendor library: EGL, state tracker, llvmpipe and softpipe linked in statically | `3058af728d9612c02f014cb4d2be81021ff99b3aabaefe70cd5cab229c5f2f28` |

`libEGL_mesa.so.0` needs `libbe.so`, `libroot.so`, `libLLVM.so.21.1`,
`libz.so.1`, `libstdc++.so.6` and `libgcc_s.so.1` from the system. The
`llvm21_libs` package must stay installed. There is no renderer add-on and no
`ADDON_PATH` or `LIBGL_DRIVERS_PATH` lookup: the rasterizers are inside the
vendor library. Hashes are not reproducible across rebuilds (build paths).

Headers are in `prefix/include` (`EGL`, `GLES2`, `GLES3`, `KHR`, `GL`,
`glvnd`). Meson's Haiku defaults put pkg-config files in
`prefix/develop/lib/pkgconfig` and use `data` rather than `share`, so the
vendor file is `prefix/data/glvnd/egl_vendor.d/50_mesa.json`. It names
`/SummitExtensions/mesa/prefix/lib/libEGL_mesa.so.0` by absolute path.

## Using it from an application

Runtime, inside the guest:

```sh
. /SummitExtensions/mesa/summit-mesa-env.sh
```

which sets:

```sh
LIBRARY_PATH=/SummitExtensions/mesa/prefix/lib:%A/lib:/boot/home/config/non-packaged/lib:/boot/home/config/lib:/boot/system/non-packaged/lib:/boot/system/lib
__EGL_VENDOR_LIBRARY_FILENAMES=/SummitExtensions/mesa/prefix/data/glvnd/egl_vendor.d/50_mesa.json
```

- `LIBRARY_PATH` is the only required variable. On Haiku it is the complete
  runtime search path: once set, `runtime_loader` no longer consults its
  built-in defaults, so they must follow the private directory as above. An
  existing `LIBRARY_PATH` is kept and only prefixed.
- `__EGL_VENDOR_LIBRARY_FILENAMES` is redundant for this prefix. The private
  `libEGL.so.1` is patched to search `<prefix>/data/glvnd/egl_vendor.d` first;
  a run with only `LIBRARY_PATH` selected llvmpipe correctly. The variable is
  exported so the choice is explicit.
- The sonames match the system ones (`libEGL.so.1`, `libGLESv2.so.2`). The
  original probe binary, linked against the system libraries, fails without
  the environment and passes with it, unmodified. An existing executable needs
  no relink. Child processes must inherit the environment.
- Optional: `GALLIUM_DRIVER=softpipe` selects softpipe at run time (checked);
  `LP_NUM_THREADS=N` limits llvmpipe's rasterizer threads;
  `EGL_LOG_LEVEL=debug` enables Mesa EGL diagnostics.

Compile and link:

```sh
g++ ... -I/SummitExtensions/mesa/prefix/include -L/SummitExtensions/mesa/prefix/lib -lEGL -lGLESv2
# or
PKG_CONFIG_PATH=/SummitExtensions/mesa/prefix/develop/lib/pkgconfig pkg-config --cflags --libs egl glesv2
```

The private `-I` must precede the system OpenGL kit headers from `mesa_devel`
(`/boot/system/develop/headers/os/opengl`).

## Build recipe

Everything is driven from the host. Scripts and patches are in
`tools/mesa-vm/`; downloaded inputs are cached in the ignored
`.vm/mesa-inputs/` and verified by SHA-256 on the host and again in the guest.

```sh
bash tools/mesa-vm/build.sh upload      # fetch + verify inputs, copy to /SummitExtensions/mesa/inputs
bash tools/mesa-vm/build.sh start all   # detached guest run: clean, tools, glvnd, mesa, install, probe
bash tools/mesa-vm/build.sh wait        # poll; prints RUNNING/FINISHED and the log tail
bash tools/mesa-vm/build.sh status 40
```

`all` deletes and rebuilds `prefix`, `work`, `build`, `pytools` and `probe`;
inputs and logs are kept. The recorded from-scratch run took 3 min 10 s with
four jobs. Individual stages: `check clean tools glvnd mesa-prepare
mesa-configure mesa-build mesa-install probe`; `build.sh run STAGE...` runs
them in the foreground. `SUMMIT_MESA_JOBS` defaults to 4 (see VM.md for why not
more). `SUMMIT_MESA_DRIVERS=softpipe` builds the LLVM-free flavour in its own
build directory; the default is `softpipe,llvmpipe`. The `check`, `glvnd` and
`mesa-*` stages stop when `/boot` has less than 600 MiB free. `/boot` read
1175-1176 MiB free at every check. The build writes only under
`/SummitExtensions/mesa` (`TMPDIR` there, Python bytecode disabled) and no
Mesa or Meson cache directory appeared under `/boot/home`; the 1 MiB drift was
not traced and is assumed to be other activity in the VM.

### Build tools

Meson and Mako are not installed in the guest and the default `python3` (3.14)
has no `yaml`. Instead of installing packages on the nearly full boot volume,
the `tools` stage unpacks pinned pure-Python wheels into
`/SummitExtensions/mesa/pytools/site` and creates `pytools/bin/meson` plus a
`pytools/bin/python3` link to `python3.10`, which already has the packaged
`pyyaml_python310` 6.0.

| Input | SHA-256 |
| --- | --- |
| `meson-1.8.4-py3-none-any.whl` | `2d71499af23ea06abe1bb7ea16843973f53b47b1ce89810c32ec154e6c1dc1db` |
| `mako-1.3.10-py3-none-any.whl` | `baef24a52fc4fc514a0887ac600f9f1cff3d82c61d4d700a1fa84d597b88db59` |
| `packaging-25.0-py3-none-any.whl` | `29572ef2b1f17581046b3a2227d5c611fb25ec70ca1ba8554b24b0e69331a484` |
| `MarkupSafe-2.1.5.tar.gz` (pure Python part only) | `d283d37a890ba4c1ae73ffadf8046435c76e7bc2247bbb63c00bd1a709c6544b` |

The guest build exports `TMPDIR=/SummitExtensions/mesa/tmp`,
`PYTHONDONTWRITEBYTECODE=1`, `PYTHONPATH=<pytools>/site` and
`PKG_CONFIG_PATH=<prefix>/develop/lib/pkgconfig:<prefix>/lib/pkgconfig`.

### libglvnd 1.7.0

Mesa 25.3 cannot build `libGLESv2` on Haiku without libglvnd
(`shared_glapi_lib` is only defined for the DRI, Xlib and WGL targets), and
both HaikuPorts' 25.3.6 recipe and the KunanyiOS port use libglvnd. Patches, in
order:

| Patch | SHA-256 | Origin |
| --- | --- | --- |
| `libglvnd-1.7.0.patchset` | `a2b593754f479d72395f148fa650c21d79ab75e0d7b62602d3f196f056708da5` | HaikuPorts: guard `fRenderer` in HGL |
| `libglvnd-haiku-mutex.patch` | `96f0fa776fa8b3c6425e26981e6e4cbca7fc33f31eb8f8e58bad730891de6035` | KunanyiOS: beta6 only has `PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP` |
| `libglvnd-haiku-bitmap.patch` | `e9b6c1d5301b952a536d66bdd2f2985b983d3adf45dbb6e2ef373b46ac1b81ff` | KunanyiOS: check the bitmap under the renderer lock |
| `libglvnd-haiku-private-prefix.patch` | `2b93a5ef8232d9202f6a9ee9e99fc4fbbf2564b515f4c23a93580d8a25c7f04c` | new: search `<prefix>/<datadir>/glvnd/egl_vendor.d` before Haiku's fixed system add-on directories |

KunanyiOS' `libglvnd-haiku-sysroot.patch` is for cross builds and is omitted.

```sh
meson setup build/glvnd work/libglvnd-v1.7.0 \
  --prefix=/SummitExtensions/mesa/prefix --libdir=lib --includedir=include \
  --buildtype=release --wrap-mode=nofallback \
  -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu \
  -Dx11=disabled -Dglx=disabled -Dhgl=true -Dgles1=false -Dgles2=true -Degl=true
```

Its configure log reports `Using dispatch stub type: pure_c` (no assembly
stubs) and `Using TLS variable for dispatch table: true`.

### Mesa 25.3.6

| Patch | SHA-256 | Content |
| --- | --- | --- |
| `mesa-25.3.6-haiku-x86_64.patch` | `d66a2b5f7d945baf54609a237b7d9e136b16d8883cf16d3ce29f92bc933516a8` | architecture-independent subset of the KunanyiOS port |
| `mesa-25.3.6-summit-01-quiet-egl-haiku.patch` | `f090490f7e4c151242102b5d1767154f081e93603e0075f617bbacb40c512235` | new: three unconditional `printf` calls in the surface constructors become the existing debug-only `TRACE` |

`tools/mesa-vm/make-patch.py` derives the first patch from
`/mnt/HaikuWork/src/haiku/tools/rock5-itx/mesa/mesa-haiku-native.patch`
(SHA-256 `19ca86d5aa36039da00ea8a2c0e590fe5d7b31a8f18f046f59d1d31b1ffc6187`)
by dropping the 42 file sections under `src/panfrost/`,
`src/gallium/drivers/panfrost/` and `src/gallium/winsys/panfrost/`. Kept:

- `src/egl/drivers/haiku/egl_haiku.cpp`: requested API/profile/version honoured,
  client APIs derived from the screen, real pbuffers without a window hook,
  pbuffer swaps preserve contents, display reference counting across
  terminate/reinitialize, unimplemented extensions no longer advertised. Its
  Mali path is inside `#ifdef GALLIUM_PANFROST` and compiles out.
- `src/egl/meson.build`: link `libglapi` into the vendor library (also
  HaikuPorts' change); `driver_panfrost` is an empty dependency here.
- `src/gallium/frontends/hgl/hgl.c`, `hgl_context.h`: pbuffer framebuffers,
  `hgl_create_context_attribs`, allocation failure handling, teardown order.
- `src/gallium/auxiliary/draw/draw_vs_exec.c`, `draw_decompose_tmp.h`: stale
  TGSI token cache after shader deletion; quad-strip edge flags.
- `src/mesa/state_tracker/st_cb_flush.c`: reset-status caching. Dead code
  here, since neither software driver implements `get_device_reset_status`.
- `include/renderdoc_app.h`, `include/drm-uapi/drm.h`: Haiku build fixes.

The second patch matters for a browser: upstream printed
`haiku_create_pbuffer_surface` to stdout for every surface.

```sh
meson setup build/mesa-llvmpipe work/mesa-25.3.6 \
  --prefix=/SummitExtensions/mesa/prefix --libdir=lib --includedir=include \
  --buildtype=release --wrap-mode=nofallback \
  -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu \
  -Dplatforms=haiku \
  -Degl=enabled -Dglvnd=enabled -Dglx=disabled -Dgbm=disabled \
  -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
  -Dgallium-drivers=softpipe,llvmpipe -Dvulkan-drivers= \
  -Dgallium-va=disabled -Dgallium-rusticl=false \
  -Dllvm=enabled \
  -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled \
  -Dzstd=disabled -Dzlib=enabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
  -Dspirv-tools=disabled -Dbuild-tests=false -Dtools=
ninja -C build/mesa-llvmpipe -j4          # 1032 steps
meson install -C build/mesa-llvmpipe --no-rebuild
```

The install stage then rewrites `50_mesa.json` with the absolute library path,
records the flavour in `prefix/summit-mesa-flavor`, and fails if any installed
library contains `TLSDESC`, `R_X86_64_TPOFF*` or `GOTTPOFF` relocations.

Two configuration choices are deliberate:

- **`-mtls-dialect=gnu` is required for a native build.** Haiku's
  `runtime_loader` implements only general-dynamic TLS relocations
  (`R_X86_64_DTPMOD64`, `DTPOFF32/64`). Mesa and libglvnd both probe
  `-mtls-dialect=gnu2` by running a small executable; the linker relaxes
  TLSDESC away in an executable, so the probe succeeds and the shared libraries
  would be built with relocations the loader cannot resolve. Supplying a
  `-mtls-dialect=` in `c_args` skips the probe; neither Meson log contains a
  `gnu2` test. The KunanyiOS cross build never met this because the probe is
  skipped when cross compiling. The hazard was identified by reading
  `src/system/runtime_loader/arch/x86_64/arch_relocate.cpp` in the KunanyiOS
  Haiku tree, not the exact beta6 source, and was avoided rather than
  reproduced: the failing configuration was never built.
- **`shader-cache=disabled`.** Mesa's disk cache defaults to `$HOME/.cache`,
  which is on the boot volume. The cost is that llvmpipe recompiles every
  shader in every process.

No Mesa or libglvnd compile error occurred in either flavour; no feature was
disabled to get the build through.

## Probe output

`bash tools/mesa-vm/build.sh run probe` rebuilds both probes against the prefix
and runs them with the environment file. Installed flavour, default driver:

```text
EGL 1.4 vendor=Mesa Project
EGL client apis=OpenGL OpenGL_ES
EGL extensions=EGL_EXT_create_context_robustness EGL_KHR_config_attribs EGL_KHR_create_context EGL_KHR_get_all_proc_addresses EGL_KHR_no_config_context EGL_KHR_surfaceless_context EGL_MESA_configless_context
EGL client extensions=EGL_EXT_device_base EGL_EXT_device_enumeration EGL_EXT_device_query EGL_EXT_platform_base EGL_KHR_client_get_all_proc_addresses EGL_EXT_client_extensions EGL_KHR_debug EGL_EXT_platform_device EGL_EXT_explicit_device EGL_MESA_platform_surfaceless
config surface type=0x5 (pbuffer=1 window=1)
pbuffer surface=0xe82425ac40 err=0x3000
GL_VENDOR=Mesa
GL_RENDERER=llvmpipe (LLVM 21.1.8, 256 bits)
GL_VERSION=OpenGL ES 3.1 Mesa 25.3.6
GLSL=OpenGL ES GLSL ES 3.10
fbo status=0x8cd5 (complete=0x8cd5)
pixel0=64,128,191,255 glError=0x0
clear+readback 1024x768: 3.90 ms/frame
```

`gles2-draw-probe` (exit status 0 only if every check passes):

```text
EGL 1.4 vendor=Mesa Project version=1.4
GL_RENDERER=llvmpipe (LLVM 21.1.8, 256 bits)
GL_VERSION=OpenGL ES 3.1 Mesa 25.3.6
surfaceless: scene 64x64 checked, 0 bad pixels, blend=255,128,128,255
surfaceless: 1024x768 blended textured quad: 2.97 ms/frame draw+finish, 2.86 ms/frame draw+readback
pbuffer: scene 64x64 checked, 0 bad pixels, blend=255,128,128,255
pbuffer: 1024x768 blended textured quad: 3.12 ms/frame draw+finish, 2.66 ms/frame draw+readback
pbuffer default framebuffer pixel=255,128,64,255
thread: scene 64x64 checked, 0 bad pixels, blend=255,128,128,255
thread: 1024x768 blended textured quad: 2.73 ms/frame draw+finish, 2.70 ms/frame draw+readback
platform-surfaceless: EGL 1.4 renderer=llvmpipe (LLVM 21.1.8, 256 bits) pixel=0,255,0,255
RESULT: PASS
```

Each scene uploads a texture in one context, draws it from a second sharing
context with a GLSL ES 1.00 program and a VBO into an FBO, adds a premultiplied
50% layer under scissor, and checks all 4,096 pixels plus a blend result. The
`thread` run creates its contexts on a `pthread` while the main thread keeps
its own context current.

Software-only flavour and `GALLIUM_DRIVER=softpipe` on the installed flavour:
`GL_RENDERER=softpipe`, same version strings, all checks pass, clear+readback
1.29-2.63 ms/frame, blended 1024x768 pass 37-41 ms/frame. With softpipe
`EGL_EXT_create_context_robustness` is absent and the GL extension list is
shorter. llvmpipe is about 13 times faster on the blended pass. Timings vary
with the other engineer's WebKit builds in the same VM: one later probe stage
took 73 s of wall time instead of 2 s (still passing; not broken down).

The full `GL_EXTENSIONS` string is in the guest logs. Relevant to
TextureMapper: `GL_OES_texture_npot`, `GL_EXT_texture_format_BGRA8888`,
`GL_EXT_read_format_bgra`, `GL_OES_rgb8_rgba8`, `GL_OES_packed_depth_stencil`,
`GL_OES_vertex_array_object`, `GL_OES_standard_derivatives`,
`GL_EXT_unpack_subimage`, `GL_OES_mapbuffer`, `GL_EXT_map_buffer_range`,
`GL_APPLE_sync`, `GL_KHR_debug`, `GL_OES_surfaceless_context`.

## Known limitations

- **EGL 1.4 with a short extension list.** There is no `EGL_KHR_image_base`,
  `EGL_KHR_gl_texture_2D_image`, `EGL_KHR_fence_sync`, `EGL_KHR_reusable_sync`,
  `EGL_KHR_gl_colorspace` or dma-buf import. GL advertises `GL_OES_EGL_image`,
  but EGL advertises no extension that creates an `EGLImage`; this was not
  probed further. Plain texture/FBO compositing is covered; EGLImage-based
  sharing (video, cross-process buffers) should be assumed unavailable. Sync
  objects are advertised on the GL side only (`GL_APPLE_sync`); they were not
  exercised.
- **One config.** RGBA8888, depth 24, stencil 8, pbuffer + window, no
  multisample configs.
- **Window surfaces are untested here.** `eglCreateWindowSurface` expects a
  `BitmapHook*` (`GetSize`/`SetBitmap`, declared in Mesa's private
  `src/gallium/winsys/sw/hgl/hgl_sw_winsys.h`), not a `BWindow` or `BView`, and
  the header is not installed. KunanyiOS qualified that path on ARM64 only.
  Presenting to a Haiku view currently means `glReadPixels` into a `BBitmap`
  or implementing `BitmapHook`.
- **`libGL.so.1` in the prefix shadows the system one.** Any process started
  with this `LIBRARY_PATH` that uses `BGLView` gets libglvnd's EGL-backed
  implementation instead of Mesa 22. That path was not run. Do not export the
  environment globally.
- **Not relocatable** without setting `__EGL_VENDOR_LIBRARY_FILENAMES` to a
  rewritten vendor file; the vendor directory and library path are absolute.
- **Software rendering only**, CPU-bound, and sharing twelve vCPUs with WebKit
  builds. No shader disk cache.
- **Not packaged.** It depends on `/SummitExtensions` being mounted (VM.md
  describes remounting after a restart) and on the system `llvm21_libs` and
  `zlib` packages.
- The pinned wheels were hashed after `pip download`; they were not compared
  with an independent source.
