# Summit's Haiku test VM

Summit has an independent QEMU/KVM VM, with 12 vCPUs and 20 GiB RAM.
Its 24 GiB boot disk is `summit/.vm/work.qcow2`. A separate 64 GiB build
disk is `summit/.vm/extensions.qcow2`. Kiri's disk and VM are separate.

- SSH: `127.0.0.1:2225`, through `bash tools/haiku.sh`.
- VNC: `127.0.0.1:5905`.
- QMP: `.vm/qmp.sock`, through `python3 tools/vm.py`.
- Guest OS: Haiku R1/beta6, hrev59866+79, x86_64.
- Browser checkout: `/boot/home/summit`.
- Upstream engine checkout: `/boot/home/summit-webkit`.
- Engine install prefix: `/boot/home/summit-webkit-install`.

Connect a VNC viewer to `127.0.0.1:5905` on the development host. Viewers that
accept a display number can use `127.0.0.1:5`. From another computer, first
forward the port through SSH:

```sh
ssh -N -L 5905:127.0.0.1:5905 your-user@development-host
```

Then connect the viewer to local port 5905. The current development session has
a **Summit - live build progress** Terminal window following
`/SummitExtensions/summit/live-progress.log`; the host mirrors build output and milestone
updates into it. Compilation normally runs over SSH. GUI test windows appear
on this same desktop when those tests run. Move the pointer or press Shift if
the screen saver has blanked the display.

The initial guest disk was created with QEMU's live `drive-backup` of Kiri's
development VM. The backup job completed without error before this copy was
booted. Its SSH host key was checked against the source VM's trusted key.
The cloned test key is stored only in ignored `.vm/` state. The guest was then
upgraded independently using beta6's official Haiku and HaikuPorts repositories
and `pkgman full-sync -y`, followed by a reboot.

```sh
bash tools/run-vm.sh
bash tools/haiku.sh 'uname -a'
bash tools/build-in-vm.sh
python3 tools/vm.py screenshot
```

`run-vm.sh` refuses to restart when an existing QMP socket or live PID is
present. A transient SSH or QMP timeout is not proof that QEMU has stopped.
Inspect the existing process and handle before changing VM state.

With the extension disk mounted, test harness compilers can also use its
temporary directory. Create the directory first, then set `SUMMIT_NATIVE_TMPDIR`
on the host command:

```sh
bash tools/haiku.sh 'test -d /SummitExtensions/WebKit && mkdir -p /SummitExtensions/summit/tmp'
SUMMIT_NATIVE_TMPDIR=/SummitExtensions/summit/tmp \
  python3 tools/test-modern-darkreader.py --bundle /SummitExtensions/summit/build-modern-browser/BUNDLE
```

`haiku.sh` forwards this value as the guest command's `TMPDIR`, including for
commands launched by Python test runners. The directory must already exist;
otherwise some native tools may fall back to the boot volume. This setting
controls compiler temporary files; each test runner still chooses its own
profile and result directories. The extension engine build uses its own
`WebKitBuild/Modern/tmp`, and bundle staging and compiler temporaries follow
`SUMMIT_NATIVE_BUILD_ROOT`.

To recreate from scratch, install the official Haiku beta6 x86_64 release
into a new disk, add development tools and a dedicated SSH key, then use the
device and forwarding options in `run-vm.sh`. No hardware/ARM64 checkout or
NanoKVM configuration is used here.

Engine build prerequisites (package capability names):

```sh
pkgman install gcc haikuwebkit_devel cmd:cmake cmd:ninja cmd:gperf cmd:bison \
  cmd:flex cmd:pkg_config cmd:ruby cmd:perl cmd:python3 llvm21 llvm21_lld \
  devel:libavif devel:libcurl devel:libexecinfo devel:libgl icu74_devel \
  devel:liblcms2 devel:libpng16 devel:libpsl devel:libsqlite3 devel:libssl \
  devel:libwebp devel:libwoff2dec devel:libxml2 devel:libxslt devel:libz
```

The pinned source currently configures with GCC 13.3 and CMake 4.1.6. LLVM 21
provides archive and linker tools. Successful configuration does not imply
that the engine builds or that enabled features work.

Use `icu74_devel` explicitly. The generic `devel:libicuuc` capability can select
ICU 66, which is too old for the pinned JavaScriptCore source. If another ICU
development package is active, select its replacement in pkgman's solver.
The build script uses the HaikuPorts GCC memory-management flags for the VM.
It defaults to three compiler workers. During a six-worker WebCore build,
the 12 GiB guest used several GiB of swap and developed long SSH login delays
and compiler processes stuck in kernel waits. The precise kernel cause has
not been established.
Set `SUMMIT_WEBKIT_JOBS` to adjust concurrency for a different VM size.
After the first full Test262 run completed, the guest was shut down normally
and increased from 8 vCPUs/12 GiB to 12 vCPUs/20 GiB for subsequent builds.

The September 13 incident required a guest reset after compiler termination,
`sync` and normal shutdown stopped responding. A full QMP disk backup completed
first and is retained as `.vm/recovery-before-restart.qcow2`. The restarted guest
passed the 38 portable browser checks and JavaScriptCore runtime checks again.

The selected upstream Test262 groups use Python 3.10 and `pyyaml_python310`:

```sh
pkgman install pyyaml_python310
```

Run `bash tools/test262-in-vm.sh` on the host after building `jsc`. The wrapper
preloads the packaged YAML module because upstream's requested wheel is not
available for Haiku. Test definitions, harness, strict/default execution and
failure reporting remain upstream's. Other pure Python harness dependencies
are installed by WebKit's own pinned dependency manager.

The engine test VM saves native crash reports for Summit and the development `jsc` paths
automatically, using an executable-specific `report` action in
`~/config/settings/system/debug_server/settings`. Other applications retain
Haiku's normal prompt. Reports are written to the guest Desktop; this avoids
leaving unattended conformance runs behind modal crash dialogs.


The extension build uses a second BFS volume named `SummitExtensions`, attached
through the existing USB 3 controller with serial `SUMMITEXTENSIONS01`.
`run-vm.sh` attaches this disk when `.vm/extensions.qcow2` exists. Its guest
mount point is `/SummitExtensions`; `/boot/home/summit-webkit-extensions` is
a symlink to `/SummitExtensions/WebKit`. If it is not automatically mounted
after a restart, identify that serial with `listusb -v` and mount its existing
BFS volume at `/SummitExtensions`. Do not initialize it again.

The disk was added live on September 14 after the boot volume reached
19.6/24 GiB used. Its new device was checked against the QMP image, USB serial
and blank first/last sectors before initialization. The isolated source tree
was copied and 76,741 files verified by SHA-256 before switching the symlink.
The original tree remains at
`/boot/home/summit-webkit-extensions.before-volume`; the normal engine tree
and browser bundles were untouched. Records: `.vm/extension-disk-setup.json`,
`.vm/extension-volume-migration.log`, `.vm/extension-build-environment.log`.

For the separate extension integration build:

```sh
SUMMIT_WEBKIT_JOBS=3 bash tools/build-webkit-in-vm.sh --modern-extensions all
```

This enables both `WK_WEB_EXTENSIONS` and `CONTENT_EXTENSIONS`. It prepares
libzip 1.11.4 under `/boot/home/summit-deps/libzip-1.11.4` from the exact native
packages in `engine/libzip.lock.json`, verifies package and extracted-file
hashes, and supplies a private pkg-config file. No system package is replaced.
A successful configuration or individual compile is not a working extension
runtime; consult `docs/webextensions-native-integration.md` for current gates.

Repeated engine source uploads compare files up to 8 MiB before creating a
temporary inode, and skip chmod when permissions already match. Identical
sources retain their inode and timestamps; changed files receive a current
mtime so Ninja cannot reuse stale generated outputs. Larger files retain the
streaming comparison/replacement path. Five synchronization checks passed on
the host and in Haiku (Python 3.10), covering both paths, mode-only changes,
symlink replacement, removal boundaries and manifest preservation on an invalid
archive path: `python3 -m unittest discover -s tests -p test_sync_webkit_sources.py`.

During the September 14 adapter upload, the previous uploader stopped making
progress and new writes on the boot volume waited; the separate build volume
continued working. Its exact Python image was verified before diagnostic
termination. Boot writes resumed afterward. The report contains process/image
information, without a stopped-thread stack; the underlying cause is not
established. This was a failed sync, and its manifest remained on the prior
patch. The optimized retry exited normally with 76,041 identical files and
published patch `fd009b76e291c55a5ba28f2400c9071c4e620667ccd46b1fadcb860c19f8c192`.
Evidence: `.vm/extension-native-source-sync-stall.report`,
`.vm/extension-native-source-sync-stall-debugger.log`, and
`.vm/extension-native-adapters-baseline-source-sync-retry.log`.

The first full feature-enabled build subsequently stalled at step 6746/7527
under eight compiler workers. Kernel inspection found a compiler waiting in
`ModifiedPageQueue::WaitIfOverQuota` through BFS `Inode::WriteAt`, other
compilers waiting on the extension volume's journal lock, and the separate
page-hook probe's assembler waiting on the boot volume's journal lock. Another
`cc1plus` was stopped after a null-PC exception. These are failed/incomplete
build attempts, not WebKit compile passes. The inspection used Alt-SysRq-D,
read-only thread/stack commands, then `cont`; no kernel or OS source was changed.

Before restarting, QEMU saved the `extension-stall-20260914-0713` checkpoint
across both disks, including 17.8 GiB of VM state. A QMP reset restored a
responsive guest. Both source manifests retained their expected hashes, and
the existing extension volume was remounted after checking its USB serial.
The checkpoint remains available; avoid deleting it while investigating this
failure. Use three workers for the next full attempt and finish isolated
probes first. Evidence: `.vm/extension-build-vm-stall.json`,
`.vm/extension-build-vm-stall-kernel-stacks.log`,
`.vm/extension-stall-checkpoint.json`, and `.vm/extension-stall-restart.json`.
