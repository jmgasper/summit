#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
mkdir -p .vm artifacts
SUMMIT_INPUTS=$(mktemp .vm/current-engine-inputs.XXXXXX)
trap 'rm -f -- "$SUMMIT_INPUTS"' EXIT
cp engine/sources.lock.json "$SUMMIT_INPUTS"
bash tools/build-webkit-in-vm.sh WebKitLegacy
exec 9>.vm/engine-build.lock
if ! flock -n 9; then
    echo 'Another engine build started before the browser could be bundled.' >&2
    exit 1
fi
bash tools/haiku.sh 'if ps | /bin/grep -q "[n]inja.*WebKitBuild/Release"; then echo "An engine build is active in the VM." >&2; exit 1; fi'
tar -czf - CMakeLists.txt README.md LICENSE src tests resources vendor tools/bundle-current-browser.py |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit && tar -xzf - -C /boot/home/summit'
bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-current && cat > /boot/home/summit/build-current/engine-inputs.json' < "$SUMMIT_INPUTS"
bash tools/haiku.sh 'cd /boot/home/summit && cmake -S . -B build-current -DCMAKE_BUILD_TYPE=Release -DWEBKIT_INCLUDE_DIR=/boot/home/summit-webkit/Source/WebKitLegacy/haiku/API -DWEBKIT_LIBRARY=/boot/home/summit-webkit/WebKitBuild/Release/lib/libWebKitLegacy.so -DCMAKE_BUILD_RPATH=/boot/home/summit-webkit/WebKitBuild/Release/lib && cmake --build build-current -j2 && ctest --test-dir build-current --output-on-failure'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/bundle-current-browser.py' > .vm/current-browser-build.json
bash tools/copy-current-browser-bundle.sh
