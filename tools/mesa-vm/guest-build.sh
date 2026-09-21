#!/bin/bash
# Runs INSIDE the Haiku guest. Builds a private libglvnd + Mesa software GL
# stack (EGL + OpenGL ES 2/3) under $SUMMIT_MESA_ROOT without touching /boot
# or the system mesa packages. Driven by tools/mesa-vm/build.sh on the host.
#
# usage: guest-build.sh STAGE...
#   stages: check clean tools glvnd mesa-prepare mesa-configure mesa-build
#           mesa-install probe all   (all = every stage, from a clean prefix)
# environment:
#   SUMMIT_MESA_ROOT     default /SummitExtensions/mesa
#   SUMMIT_MESA_JOBS     default 4 (the VM has hung above that on BFS locks)
#   SUMMIT_MESA_DRIVERS  default "softpipe,llvmpipe" (LLVM 21 JIT + softpipe
#                        fallback); "softpipe" builds without LLVM; "zink"
#                        builds the Vulkan-backed driver (a real GPU)
set -euo pipefail

ROOT=${SUMMIT_MESA_ROOT:-/SummitExtensions/mesa}
JOBS=${SUMMIT_MESA_JOBS:-4}
DRIVERS=${SUMMIT_MESA_DRIVERS:-softpipe,llvmpipe}
PREFIX=$ROOT/prefix
INPUTS=$ROOT/inputs
WORK=$ROOT/work
BUILD=$ROOT/build
PYTOOLS=$ROOT/pytools
LOGS=$ROOT/logs

MESA_VERSION=25.3.6
MESA_SHA256=59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206
GLVND_SHA256=2b6e15b06aafb4c0b6e2348124808cbd9b291c647299eaaba2e3202f51ff2f3d
declare -A TOOL_SHA256=(
    [meson-1.8.4-py3-none-any.whl]=2d71499af23ea06abe1bb7ea16843973f53b47b1ce89810c32ec154e6c1dc1db
    [mako-1.3.10-py3-none-any.whl]=baef24a52fc4fc514a0887ac600f9f1cff3d82c61d4d700a1fa84d597b88db59
    [packaging-25.0-py3-none-any.whl]=29572ef2b1f17581046b3a2227d5c611fb25ec70ca1ba8554b24b0e69331a484
    [MarkupSafe-2.1.5.tar.gz]=d283d37a890ba4c1ae73ffadf8046435c76e7bc2247bbb63c00bd1a709c6544b
)

# zink turns OpenGL ES into Vulkan, which is how the workstation's GeForce
# reaches EGL: there is no Gallium driver for NVIDIA, but NVK (Vulkan) exists.
case "$DRIVERS" in
    softpipe,llvmpipe) FLAVOR=llvmpipe ;;
    softpipe) FLAVOR=softpipe ;;
    *) FLAVOR=$(echo "$DRIVERS" | tr ',' '-') ;;
esac
case "$DRIVERS" in
    *llvmpipe*) LLVM=enabled ;;
    *) LLVM=disabled ;;
esac
MESA_SRC=$WORK/mesa-$MESA_VERSION
MESA_BUILD=$BUILD/mesa-$FLAVOR
GLVND_SRC=$WORK/libglvnd-v1.7.0
GLVND_BUILD=$BUILD/glvnd

mkdir -p "$ROOT/tmp" "$WORK" "$BUILD" "$LOGS"
export TMPDIR=$ROOT/tmp
export PATH=$PYTOOLS/bin:$PATH
export PYTHONPATH=$PYTOOLS/site
export PYTHONDONTWRITEBYTECODE=1
# Meson's Haiku defaults: pkg-config files go to develop/lib/pkgconfig and
# datadir is "data" (not "share").
export PKG_CONFIG_PATH=$PREFIX/develop/lib/pkgconfig:$PREFIX/lib/pkgconfig
# Haiku's runtime_loader implements only general-dynamic TLS relocations
# (DTPMOD64/DTPOFF). Both projects probe -mtls-dialect=gnu2 with an executable,
# where the linker relaxes TLSDESC away, so a native configure would wrongly
# enable it for the shared libraries. Pin the traditional dialect.
TLS_ARGS=-mtls-dialect=gnu

boot_free_mib() {
    # Haiku's df prints "Block Size: 2.0 KiB (2048 byte)" and
    # "Free Blocks: 1.1 GiB (576806 blocks)" for a single volume.
    df /boot | awk '
        /Block Size/ { gsub(/[()]/, ""); size = $(NF-1) }
        /Free Blocks/ { gsub(/[()]/, ""); blocks = $(NF-1) }
        END { print int(blocks * size / 1048576) }'
}

check_boot() {
    local free
    free=$(boot_free_mib)
    echo "[$(date +%H:%M:%S)] /boot free: ${free} MiB"
    if [ "$free" -lt 600 ]; then
        echo "refusing to continue: /boot below 600 MiB" >&2
        exit 3
    fi
}

verify() {
    local file=$1 expected=$2 actual
    actual=$(sha256sum "$file" | awk '{print $1}')
    if [ "$actual" != "$expected" ]; then
        echo "sha256 mismatch for $file: $actual" >&2
        exit 2
    fi
}

stage_check() {
    check_boot
    df "$ROOT" | grep -E 'Mounted|Free Blocks'
    gcc --version | head -1
    ninja --version
    /boot/system/bin/python3.10 --version
    llvm-config --version || true
}

stage_clean() {
    # Inputs and logs are kept; everything derived is rebuilt.
    rm -rf "$PREFIX" "$WORK" "$BUILD" "$PYTOOLS" "$ROOT/probe"
    mkdir -p "$WORK" "$BUILD"
}

stage_tools() {
    rm -rf "$PYTOOLS"
    mkdir -p "$PYTOOLS/site" "$PYTOOLS/bin"
    local name
    for name in "${!TOOL_SHA256[@]}"; do
        verify "$INPUTS/$name" "${TOOL_SHA256[$name]}"
    done
    for name in meson-1.8.4-py3-none-any.whl mako-1.3.10-py3-none-any.whl \
            packaging-25.0-py3-none-any.whl; do
        /boot/system/bin/python3.10 -m zipfile -e "$INPUTS/$name" "$PYTOOLS/site"
    done
    # MarkupSafe ships a pure Python fallback; no C speedups are compiled.
    rm -rf "$ROOT/tmp/markupsafe" && mkdir -p "$ROOT/tmp/markupsafe"
    tar -xzf "$INPUTS/MarkupSafe-2.1.5.tar.gz" -C "$ROOT/tmp/markupsafe"
    cp -r "$ROOT/tmp/markupsafe/MarkupSafe-2.1.5/src/markupsafe" "$PYTOOLS/site/"
    rm -f "$PYTOOLS/site/markupsafe/_speedups.c"
    rm -rf "$ROOT/tmp/markupsafe"
    # Mesa looks up "python3" in PATH; the guest default (3.14) has no
    # yaml/mako, while python3.10 has the packaged pyyaml_python310.
    ln -sf /boot/system/bin/python3.10 "$PYTOOLS/bin/python3"
    cat > "$PYTOOLS/bin/meson" <<EOF
#!/boot/system/bin/python3.10
import sys
sys.path.insert(0, '$PYTOOLS/site')
from mesonbuild import mesonmain
sys.exit(mesonmain.main())
EOF
    chmod +x "$PYTOOLS/bin/meson"
    meson --version
    python3 -c 'import mako, yaml, packaging, markupsafe; print("mako", mako.__version__, "yaml", yaml.__version__)'
}

stage_glvnd() {
    check_boot
    verify "$INPUTS/libglvnd-v1.7.0.tar.gz" "$GLVND_SHA256"
    rm -rf "$GLVND_SRC" "$GLVND_BUILD"
    tar -xzf "$INPUTS/libglvnd-v1.7.0.tar.gz" -C "$WORK"
    local patch
    for patch in libglvnd-1.7.0.patchset libglvnd-haiku-mutex.patch \
            libglvnd-haiku-bitmap.patch libglvnd-haiku-private-prefix.patch; do
        echo "applying $patch"
        patch -d "$GLVND_SRC" -p1 < "$INPUTS/$patch"
    done
    meson setup "$GLVND_BUILD" "$GLVND_SRC" \
        --prefix="$PREFIX" --libdir=lib --includedir=include \
        --buildtype=release --wrap-mode=nofallback \
        -Dc_args=$TLS_ARGS -Dcpp_args=$TLS_ARGS \
        -Dx11=disabled -Dglx=disabled -Dhgl=true \
        -Dgles1=false -Dgles2=true -Degl=true
    ninja -C "$GLVND_BUILD" -j"$JOBS"
    meson install -C "$GLVND_BUILD" --no-rebuild
    check_boot
}

stage_mesa_prepare() {
    check_boot
    verify "$INPUTS/mesa-$MESA_VERSION.tar.xz" "$MESA_SHA256"
    rm -rf "$MESA_SRC"
    tar -xJf "$INPUTS/mesa-$MESA_VERSION.tar.xz" -C "$WORK"
    local patch
    for patch in mesa-25.3.6-haiku-x86_64.patch $(cd "$INPUTS" && ls mesa-25.3.6-summit-*.patch 2>/dev/null | sort); do
        echo "applying $patch"
        patch -d "$MESA_SRC" -p1 < "$INPUTS/$patch"
    done
}

stage_mesa_configure() {
    check_boot
    rm -rf "$MESA_BUILD"
    meson setup "$MESA_BUILD" "$MESA_SRC" \
        --prefix="$PREFIX" --libdir=lib --includedir=include \
        --buildtype=release --wrap-mode=nofallback \
        -Dc_args=$TLS_ARGS -Dcpp_args=$TLS_ARGS \
        -Dplatforms=haiku \
        -Degl=enabled -Dglvnd=enabled -Dglx=disabled -Dgbm=disabled \
        -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
        -Dgallium-drivers="$DRIVERS" -Dvulkan-drivers= \
        -Dgallium-va=disabled -Dgallium-rusticl=false \
        -Dllvm=$LLVM \
        -Dshader-cache=disabled -Dxmlconfig=disabled -Dexpat=disabled \
        -Dzstd=disabled -Dzlib=enabled \
        -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
        -Dspirv-tools=disabled -Dbuild-tests=false -Dtools=
}

stage_mesa_build() {
    check_boot
    ninja -C "$MESA_BUILD" -j"$JOBS"
    check_boot
}

stage_mesa_install() {
    meson install -C "$MESA_BUILD" --no-rebuild
    # Make the vendor file independent of the library search path.
    local json=$PREFIX/data/glvnd/egl_vendor.d/50_mesa.json
    cat > "$json" <<EOF
{
    "file_format_version" : "1.0.0",
    "ICD" : {
        "library_path" : "$PREFIX/lib/libEGL_mesa.so.0"
    }
}
EOF
    echo "$FLAVOR" > "$PREFIX/summit-mesa-flavor"
    if readelf -rW "$PREFIX"/lib/lib*.so* 2>/dev/null | grep -E 'TLSDESC|R_X86_64_TPOFF|GOTTPOFF' | head -3 | grep .; then
        echo "unsupported TLS relocations found (Haiku runtime_loader)" >&2
        exit 4
    fi
    ls -la "$PREFIX/lib" "$PREFIX/data/glvnd/egl_vendor.d"
    check_boot
}

stage_probe() {
    local probe=$ROOT/probe name
    mkdir -p "$probe"
    cp "$INPUTS/summit-mesa-env.sh" "$ROOT/summit-mesa-env.sh"
    for name in eglprobe gles2-draw-probe; do
        cp "$INPUTS/$name.cpp" "$probe/$name.cpp"
        g++ -O2 -o "$probe/$name" "$probe/$name.cpp" \
            -I"$PREFIX/include" -L"$PREFIX/lib" -lEGL -lGLESv2 -lbe
    done
    # shellcheck disable=SC1091
    SUMMIT_MESA_PREFIX=$PREFIX . "$ROOT/summit-mesa-env.sh"
    echo "--- flavor: $(cat "$PREFIX/summit-mesa-flavor") GALLIUM_DRIVER=${GALLIUM_DRIVER:-<default>}"
    "$probe/eglprobe"
    "$probe/gles2-draw-probe"
}

for stage in "$@"; do
    echo "=== stage $stage ($(date)) flavor=$FLAVOR jobs=$JOBS"
    case "$stage" in
        check) stage_check ;;
        clean) stage_clean ;;
        tools) stage_tools ;;
        glvnd) stage_glvnd ;;
        mesa-prepare) stage_mesa_prepare ;;
        mesa-configure) stage_mesa_configure ;;
        mesa-build) stage_mesa_build ;;
        mesa-install) stage_mesa_install ;;
        probe) stage_probe ;;
        all)
            stage_check; stage_clean; stage_tools; stage_glvnd; stage_mesa_prepare
            stage_mesa_configure; stage_mesa_build; stage_mesa_install
            stage_probe ;;
        *) echo "unknown stage $stage" >&2; exit 1 ;;
    esac
    echo "=== stage $stage done ($(date))"
done
