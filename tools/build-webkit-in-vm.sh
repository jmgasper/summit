#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
SUMMIT_ENGINE_MODE=legacy
case "${1:-}" in
    --modern) SUMMIT_ENGINE_MODE=modern; shift ;;
    --modern-extensions) SUMMIT_ENGINE_MODE=extensions; shift ;;
esac
SUMMIT_ENGINE_SOURCE=/boot/home/summit-webkit
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    # Keep the feature-enabled dependency closure separate from the browser preview.
    SUMMIT_ENGINE_SOURCE=/boot/home/summit-webkit-extensions
fi
SUMMIT_ENGINE_TARGET=${1:-all}
SUMMIT_WEBKIT_JOBS=${SUMMIT_WEBKIT_JOBS:-3}
if [[ ! $SUMMIT_WEBKIT_JOBS =~ ^[1-9][0-9]?$ ]] || (( SUMMIT_WEBKIT_JOBS > 64 )); then
    echo 'SUMMIT_WEBKIT_JOBS must be an integer between 1 and 64.' >&2
    exit 2
fi
case "$SUMMIT_ENGINE_MODE:$SUMMIT_ENGINE_TARGET" in
    legacy:all|legacy:JavaScriptCore|legacy:jsc|legacy:WebCore|legacy:WebKitLegacy) ;;
    modern:all|modern:JavaScriptCore|modern:jsc|modern:WebCore|modern:WebKit|modern:WebProcess|modern:NetworkProcess) ;;
    extensions:all|extensions:JavaScriptCore|extensions:jsc|extensions:WebCore|extensions:WebKit|extensions:WebProcess|extensions:NetworkProcess) ;;
    *) echo 'Usage: build-webkit-in-vm.sh [--modern|--modern-extensions] [all|JavaScriptCore|jsc|WebCore|WebKitLegacy|WebKit|WebProcess|NetworkProcess]' >&2; exit 2 ;;
esac
if (( $# > 1 )); then
    echo 'Only one engine target can be selected.' >&2
    exit 2
fi
mkdir -p .vm
exec 9>.vm/engine-build.lock
if ! flock -n 9; then
    echo 'Another Summit engine build script is active.' >&2
    exit 1
fi
# Also catch builds started directly over SSH before changing their sources.
bash tools/haiku.sh 'if ps | /bin/grep -Eq "[n]inja.*WebKitBuild/(Release|Modern)"; then echo "An engine build is active in the VM; inspect its existing log and process." >&2; exit 1; fi'
if [[ $SUMMIT_ENGINE_MODE != legacy ]]; then
    bash tools/haiku.sh 'test -f /boot/home/summit-deps/icu78/lib/libicuuc.so.78.3 || { echo "Build private ICU first with tools/build-icu-in-vm.sh." >&2; exit 1; }'
fi
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/tools /boot/home/summit/engine'
    tar -cf - tools/prepare-extension-deps.py engine/libzip.lock.json |
        bash tools/haiku.sh 'tar -xf - -C /boot/home/summit && python3.10 /boot/home/summit/tools/prepare-extension-deps.py'
fi
python3 tools/prepare-webkit.py
SUMMIT_ENGINE_PATCH_SHA=$(python3 -c 'import json; print(json.load(open("engine/sources.lock.json"))["patch"]["sha256"])')
if [[ ! $SUMMIT_ENGINE_PATCH_SHA =~ ^[0-9a-f]{64}$ ]]; then
    echo 'Invalid engine patch digest.' >&2
    exit 1
fi
bash tools/haiku.sh 'mkdir -p /boot/home/summit/tools && cat > /boot/home/summit/tools/sync-webkit-sources.py' < tools/sync-webkit-sources.py
# Source paths are fixed and passed as archive members, never as remote commands.
tar -C .cache/WebKit -czf - --exclude=__pycache__ CMakeLists.txt Configurations Source Tools |
    bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/sync-webkit-sources.py' "$SUMMIT_ENGINE_SOURCE" "$SUMMIT_ENGINE_PATCH_SHA"
# Keep large compiler temporaries on the engine volume, including when the
# extension source is a symlink to the separate native build disk.
if [[ $SUMMIT_ENGINE_MODE == extensions ]]; then
    bash tools/haiku.sh 'export PKG_CONFIG_PATH=/boot/home/summit-deps/libzip-1.11.4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}; cd /boot/home/summit-webkit-extensions && cmake -S . -B WebKitBuild/Modern -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-ftrack-macro-expansion=0 --param ggc-min-expand=10" -DENABLE_WEBKIT=ON -DENABLE_WEBKIT_LEGACY=OFF -DENABLE_WK_WEB_EXTENSIONS=ON -DENABLE_CONTENT_EXTENSIONS=ON -DENABLE_GPU_PROCESS=OFF -DENABLE_LAYOUT_TESTS=OFF -DICU_ROOT=/boot/home/summit-deps/icu78 -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-extensions-install && mkdir -p WebKitBuild/Modern/tmp && TMPDIR="$PWD/WebKitBuild/Modern/tmp" DISABLE_ASLR=1 ninja -C WebKitBuild/Modern -k 0 -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
elif [[ $SUMMIT_ENGINE_MODE == modern ]]; then
    bash tools/haiku.sh 'cd /boot/home/summit-webkit && cmake -S . -B WebKitBuild/Modern -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-ftrack-macro-expansion=0 --param ggc-min-expand=10" -DENABLE_WEBKIT=ON -DENABLE_WEBKIT_LEGACY=OFF -DENABLE_GPU_PROCESS=OFF -DENABLE_LAYOUT_TESTS=OFF -DICU_ROOT=/boot/home/summit-deps/icu78 -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-modern && DISABLE_ASLR=1 ninja -C WebKitBuild/Modern -k 0 -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
else
    bash tools/haiku.sh 'cd /boot/home/summit-webkit && cmake -S . -B WebKitBuild/Release -G Ninja -DPORT=Haiku -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-ftrack-macro-expansion=0 --param ggc-min-expand=10" -DENABLE_WEBKIT=OFF -DENABLE_WEBKIT_LEGACY=ON -DENABLE_LAYOUT_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/boot/home/summit-webkit-install && DISABLE_ASLR=1 ninja -C WebKitBuild/Release -j' "$SUMMIT_WEBKIT_JOBS" "$SUMMIT_ENGINE_TARGET"
fi
