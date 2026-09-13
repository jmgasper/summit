#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# All outputs live outside WebKitBuild, allowing focused checks during a build.
tar -C .cache/WebKit/Source/WTF/wtf/haiku -cf - RunLoopHaiku.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-runloop-tests && tar -xf - -C /boot/home/summit/build-runloop-tests'
tar -C .cache/WebKit/Source/WTF/wtf -cf - WTFProcess.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-runloop-tests'
tar -cf - tests/EngineRunLoopTests.cpp tools/test-engine-runloop.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-runloop.py'
