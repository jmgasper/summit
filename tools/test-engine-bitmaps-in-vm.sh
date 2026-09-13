#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebCore/platform/graphics -cf - ShareableBitmap.h ShareableBitmap.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-bitmap-tests && tar -xf - -C /boot/home/summit/build-bitmap-tests'
tar -C .cache/WebKit/Source/WebCore/platform/graphics/haiku -cf - ShareableBitmapHaiku.cpp GraphicsContextHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-bitmap-tests'
tar -C .cache/WebKit/Source/WebKit/Platform/haiku -cf - BitmapFrameHaiku.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-bitmap-tests'
tar -cf - tests/EngineBitmapTests.cpp tools/test-engine-bitmaps.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-bitmaps.py'
