#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebCore/platform/graphics -cf - FontPlatformData.h FontPlatformData.cpp FontCustomPlatformData.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-font-tests && tar -xf - -C /boot/home/summit/build-font-tests'
tar -C .cache/WebKit/Source/WebCore/platform/graphics/haiku -cf - FontPlatformDataHaiku.cpp FontCustomPlatformData.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-font-tests'
tar -C .cache/WebKit/Source/WebCore/platform/graphics/opentype -cf - OpenTypeUtilities.cpp OpenTypeUtilities.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-font-tests'
tar -cf - tests/EngineFontTests.cpp tools/test-engine-fonts.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-fonts.py'
