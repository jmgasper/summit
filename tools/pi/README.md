# Summit on the Raspberry Pi 4: build, stage and measure

The Pi runs air/OS (the rpi4 branch of the Haiku fork) with Summit as the
system packages `summit` and `summit_webkit`. Its engine is the GL
configuration (Skia, GL compositing, WebGL) cross-built in
`/mnt/HaikuWork/rpi4/summit-gl` from the arm64 tree
`/mnt/HaikuWork/build/summit-arm64/WebKit` (see that tree's README and
`rpi4/haiku/docs/rpi4/SUMMIT.md`). Notes and numbers: the "Raspberry Pi 4"
section of `docs/performance.md`.

## Reaching the board

- Telnet shell: `python3 /mnt/HaikuWork/rpi4/haiku/tools/rpi4/shell.py <ip> '<cmd>' [timeout]`.
  The address moves with DHCP: `ip neigh | grep e4:5f:01:9e:d8` (Ethernet
  `...:e2`, Wi-Fi `...:e4`). No `!` in commands (bash history expansion).
- The host's `/mnt/Documents` is the Pi's `/Documents` (SMB, ~40 MB/s).
  Writing to the Pi's SD card runs at ~1.4 MB/s: stage bundles in a RAM disk
  (`board/rd-bundle.sh`), never on the card.
- Haiku's `profile -a -i 500 -o FILE sleep 3` works on this kernel;
  `profile -l` (trace image loading) hangs the profiled team.
- The arm64 kernel counts all user time as kernel time (`time` shows `sys`
  only).

## Loop

1. Engine: edit `.cache/WebKit` (the x86 tree), copy changed files into
   `/mnt/HaikuWork/build/summit-arm64/WebKit` (the arm64 tree has a few local
   arm64 edits on top of the patch; files outside them can be copied), then
   `tools/pi/build-engine.sh TAG` (log `rpi4/summit-gl/ninja-ec-TAG.log`,
   incremental, ~5 min for a few files).
   To move the arm64 tree to a new engine patch, use the README of
   `build/summit-arm64` ("Updating to a newer summit commit").
2. Dependencies with GNU hash tables (once): `tools/pi/build-deps-gnu-hash.sh`
   (3 minutes; prefix `/mnt/HaikuWork/build/summit-arm64/deps-gnu/prefix`).
3. Bundle: `tools/pi/mkbundle.sh TAG b-NAME` packages engine + browser with
   the fork's packaging script and stages them in
   `/mnt/Documents/SummitPi/b-NAME`; `OUT_PACKAGES=dir` keeps the .hpkg
   files, `SUMMIT_WEBKIT_VERSION=1.10.0-N` sets the engine package's version.
4. Install on the Pi: copy the .hpkg files there (through /Documents) and
   `pkgman install -y ./summit_webkit-....hpkg ./summit-....hpkg`; for the
   next image, put them in `/mnt/HaikuWork/rpi4/packages-arm64` (it replaces
   the air/OS release's packages of the same name) and move the old ones out.
5. Probes and board scripts: `tools/pi/build-probes.sh` (into
   `/mnt/Documents/SummitPi/tools`); on the Pi copy them to
   `/boot/home/summit-ec/` and `chmod +x`.
6. On the Pi: `rd-bundle.sh b-NAME`, then
   `ab-start.sh $RD/b-NAME/Summit PROFILE TAG ROUNDS URL [K=V...]` prints, per
   launch, when Summit's code ran, when the window showed and when the first
   frame with tiles was composited (seconds after exec);
   `milestones.sh LOG` lists the `SUMMIT_STARTUP_TRACE=1` lines.
   `RD=dir` makes `rd-bundle.sh` (and `ab-bench.sh`) use another RAM disk.
7. Benchmarks that report through the page title: `ab-bench.sh ROUNDS URL
   PREFIX OUT A B...` alternates bundles in the RAM disk on a page such as
   `pages/dombench.html` (allocation-heavy DOM, style, layout and JSON work;
   the title gets the median of five rounds); `bench-title.sh` runs one.
   `kill-cycles.sh` launches, quits and force-kills a browser N times and
   reports processes left behind.

Running a bundle from a RAM disk is for measuring only: twice on 5 October a
network process left behind by a force-killed browser hung in the kernel
together with the RAM disk it ran from (`ls`, `df` and `profile -a` on it
hang too, `kill -9` does nothing). Use a new mount point when it happens;
only a restart frees it.

## Probes

- `stime CMD...`: prints the launch moment on `system_time()` and execs.
- `dltime LIB...`: `dlopen()` time, CPU time and page faults per library.
- `faultbench FILE`: cost of anonymous, file and copy-on-write page faults.
- `fctime`: fontconfig start with and without its cache.
- `egltime`: EGL display, context, shader compile and first draw.
- `readback W H`: `glReadPixels` into memory, through a PBO, from a pbuffer,
  and in 64-row bands.
- `lookup-scope.py EXE DIRS...` (host): which images every symbol lookup of
  the runtime loader walks through; `lookup-scenarios.py` models GNU hash
  tables and `-Bsymbolic-functions` for the bundled libraries.
