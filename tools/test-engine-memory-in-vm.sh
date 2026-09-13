#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# Local copies and outputs avoid changing the active renderer build.
tar -C .cache/WebKit/Source/WebCore/platform -cf - SharedMemory.h SharedMemory.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-memory-tests && tar -xf - -C /boot/home/summit/build-memory-tests'
tar -C .cache/WebKit/Source/WebCore/platform/haiku -cf - SharedMemoryHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-memory-tests'
tar -C .cache/WebKit/Source/WebKit/UIProcess/Launcher/haiku -cf - HaikuProcess.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-memory-tests'
tar -cf - tests/EngineSharedMemoryTests.cpp tools/test-engine-memory.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-memory.py'
