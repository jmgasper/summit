#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
exec 9>.vm/engine-build.lock
if ! flock -n 9; then
    echo 'An engine build is active; wait for it before running component tests.' >&2
    exit 1
fi
bash tools/haiku.sh 'if ps | /bin/grep -q "[n]inja.*WebKitBuild/Release"; then echo "An engine build is active in the VM." >&2; exit 1; fi'
tar -cf - tests/EngineCookieTests.cpp tools/test-engine-cookies.py |
    bash tools/haiku.sh 'tar -C /boot/home/summit -xf -'
bash tools/haiku.sh 'cd /boot/home/summit-webkit && DISABLE_ASLR=1 ninja -C WebKitBuild/Release -j3 Source/WebCore/CMakeFiles/WebCore.dir/platform/network/curl/CookieUtil.cpp.o'
bash tools/haiku.sh 'DISABLE_ASLR=1 python3.10 /boot/home/summit/tools/test-engine-cookies.py'
