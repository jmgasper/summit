#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
exec 9> .vm/extension-manifest-core.lock
flock -n 9 || { echo 'Another extension probe is already running.' >&2; exit 2; }
python3 tools/test-engine-extension-archives-fixtures.py .vm/extension-archive-fixtures
tar -C .cache/WebKit/Source/WebKit/UIProcess/Extensions/haiku -cf - WebExtensionArchiveHaiku.h WebExtensionArchiveHaiku.cpp WebExtensionResourcePathsHaiku.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-extension-archive-tests && tar -xf - -C /boot/home/summit/build-extension-archive-tests'
tar -C .vm/extension-archive-fixtures -cf - . |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-extension-archive-tests/fixtures && tar -xf - -C /boot/home/summit/build-extension-archive-tests/fixtures'
tar -cf - tests/EngineExtensionArchiveTests.cpp tools/test-engine-extension-archives.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-archives.py'
