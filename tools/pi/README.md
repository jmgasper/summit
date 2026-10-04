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
   (3 minutes; prefix `$SUMMIT_PI_WORK/deps-gnu/prefix`).
3. Bundle: `DEPS_PREFIX=<prefix> TLS_PREFIX=<prefix> tools/pi/mkbundle.sh TAG b-NAME`
   packages engine + browser with the fork's packaging script and stages them
   in `/mnt/Documents/SummitPi/b-NAME`.
4. Probes and board scripts: `tools/pi/build-probes.sh` (into
   `/mnt/Documents/SummitPi/tools`); on the Pi copy them to
   `/boot/home/summit-ec/` and `chmod +x`.
5. On the Pi: `rd-bundle.sh b-NAME`, then
   `ab-start.sh $RD/b-NAME/Summit PROFILE TAG ROUNDS URL [K=V...]` prints, per
   launch, when Summit's code ran, when the window showed and when the first
   frame with tiles was composited (seconds after exec);
   `milestones.sh LOG` lists the `SUMMIT_STARTUP_TRACE=1` lines.

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
