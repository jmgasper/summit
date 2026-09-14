#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# Test the production helper in an isolated directory while the engine builds.
tar -C .cache/WebKit/Source -cf - WebKitLegacy/haiku/API/DownloadFilename.h WebKit/Shared/haiku/DownloadFilenameHaiku.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-download-tests/Source && tar -xf - -C /boot/home/summit/build-download-tests/Source'
tar -cf - tests/EngineDownloadTests.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit && g++ -std=c++17 -Wall -Wextra -Werror -Ibuild-download-tests/Source/WebKitLegacy/haiku/API tests/EngineDownloadTests.cpp -lbe -o build-download-tests/run && build-download-tests/run'
