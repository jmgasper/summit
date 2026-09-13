#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebKit/Platform -cf - Module.h Module.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-module-tests && tar -xf - -C /boot/home/summit/build-module-tests'
tar -C .cache/WebKit/Source/WebKit/Platform/haiku -cf - ModuleHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-module-tests'
tar -cf - tests/EngineModuleTests.cpp tests/EngineModuleFixture.cpp tools/test-engine-modules.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-modules.py'
