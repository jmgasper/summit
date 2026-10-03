#!/bin/bash
# Cross-build fbcap with the X399 fork's toolchain and the SDK objects its nvscanout was built from
# (/mnt/HaikuWork/x399; nothing is written there). The binary lands next to this script; copy it to
# the workstation as /boot/home/summit/claude-fbcap.
set -euo pipefail
HERE=$(dirname "$(realpath "$0")")
BUILD=/mnt/HaikuWork/x399/build/x86_64
WORK=/mnt/HaikuWork/x399/build/nvidia_rm
SRC=/mnt/HaikuWork/x399/haiku/src/add-ons/accelerants/nvidia_rm
HAIKU=/mnt/HaikuWork/x399/haiku
OGKM=$WORK/open-gpu-kernel-modules
TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")
INCLUDES=(-I"$SRC" -I"$SRC/sdk" -I"$HAIKU/src/add-ons/kernel/drivers/graphics/nvidia_rm/sdk"
	-I"$OGKM/src/common/sdk/nvidia/inc" -I"$OGKM/src/common/unix/common/inc"
	-I"$OGKM/src/common/inc" -I"$OGKM/src/nvidia/arch/nvalloc/common/inc"
	-I"$OGKM/src/nvidia/arch/nvalloc/unix/include" -I"$OGKM/src/nvidia/inc/kernel"
	-I"$OGKM/src/nvidia/interface" -I"$OGKM/src/nvidia-modeset/interface"
	-I"$OGKM/src/nvidia-modeset/os-interface/include"
	-I"$OGKM/src/nvidia-modeset/kapi/interface"
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix" -I"$HAIKU/headers"
	-I"$HAIKU/headers/os" -I"$HAIKU/headers/os/add-ons/graphics"
	-I"$HAIKU/headers/os/app" -I"$HAIKU/headers/os/drivers"
	-I"$HAIKU/headers/os/interface" -I"$HAIKU/headers/os/kernel"
	-I"$HAIKU/headers/os/storage" -I"$HAIKU/headers/os/support"
	-I"$HAIKU/headers/private/graphics/common"
	-I"$HAIKU/headers/private/shared")
FLAGS=(-O2 -msse4.1 -fpic -nostdinc -D_DEFAULT_SOURCE -DNV_PLATFORM_MAX_IOCTL_SIZE=16384
	-Wno-missing-field-initializers "${INCLUDES[@]}")
T=$WORK/accelerant-obj/nvscanout
$TOOLS-g++ -std=c++20 "${FLAGS[@]}" -c "$HERE/fbcap.cpp" -o "$HERE/fbcap.o"
$TOOLS-g++ -nostdlib -o "$HERE/fbcap" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbegin.o" \
	"$OBJ/system/glue/start_dyn.o" "$OBJ/system/glue/init_term_dyn.o" \
	"$HERE/fbcap.o" "$T/ErrorUtils.o" "$T/NvRmApi.o" "$T/NvRmDevice.o" "$T/nvstatus.o" \
	"$OBJ/system/libroot/libroot.so" "$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" "$GCCLIB/libgcc.a" \
	"$GCCLIB/crtend.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
echo built "$HERE/fbcap"
