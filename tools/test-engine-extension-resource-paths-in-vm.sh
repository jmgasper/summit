#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
exec 9> .vm/extension-manifest-core.lock
flock -n 9 || { echo 'Another extension probe is already running.' >&2; exit 2; }
tar -C .cache/WebKit/Source/WebKit/UIProcess/Extensions/haiku -cf - WebExtensionResourcePathsHaiku.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-extension-resource-path-tests && tar -xf - -C /boot/home/summit/build-extension-resource-path-tests'
tar -cf - tests/EngineExtensionResourcePathTests.cpp tools/test-engine-extension-resource-paths.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-resource-paths.py'
