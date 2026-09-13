# Summit's Haiku test VM

Summit has an independent QEMU/KVM VM, with 8 vCPUs and 12 GiB RAM.
Its 24 GiB disk is `summit/.vm/work.qcow2`. Kiri's disk and VM are separate.

- SSH: `127.0.0.1:2225`, through `bash tools/haiku.sh`.
- VNC: `127.0.0.1:5905`.
- QMP: `.vm/qmp.sock`, through `python3 tools/vm.py`.
- Guest OS: Haiku R1/beta6, hrev59866+79, x86_64.
- Browser checkout: `/boot/home/summit`.
- Upstream engine checkout: `/boot/home/summit-webkit`.
- Engine install prefix: `/boot/home/summit-webkit-install`.

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

The selected upstream Test262 groups use Python 3.10 and `pyyaml_python310`:

```sh
pkgman install pyyaml_python310
```

Run `bash tools/test262-in-vm.sh` on the host after building `jsc`. The wrapper
preloads the packaged YAML module because upstream's requested wheel is not
available for Haiku. Test definitions, harness, strict/default execution and
failure reporting remain upstream's. Other pure Python harness dependencies
are installed by WebKit's own pinned dependency manager.
