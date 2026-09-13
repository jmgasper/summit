#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebKit/Platform/haiku -cf - StoragePathsHaiku.h StoragePathsHaiku.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-storage-path-tests && tar -xf - -C /boot/home/summit/build-storage-path-tests'
tar -cf - tests/EngineStoragePathTests.cpp tools/test-engine-storage-paths.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-storage-paths.py'
