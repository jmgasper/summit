#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# Keep focused outputs separate from an active renderer build.
tar -C .cache/WebKit/Source/WebKit/UIProcess/Launcher/haiku -cf - HaikuProcess.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-process-tests && tar -xf - -C /boot/home/summit/build-process-tests'
tar -C .cache/WebKit/Source/WebKit/Shared -cf - ProcessExecutablePath.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-process-tests'
tar -C .cache/WebKit/Source/WebKit/Shared/haiku -cf - ProcessExecutablePathHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-process-tests'
tar -cf - tests/EngineProcessTests.cpp tests/EngineProcessPathTests.cpp tools/test-engine-process-paths.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit && c++ -std=c++17 -Wall -Wextra -Werror -Ibuild-process-tests tests/EngineProcessTests.cpp -lnetwork -o build-process-tests/run && python3.10 -c "import subprocess; subprocess.run([\"build-process-tests/run\"], timeout=20, check=True)"'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-process-paths.py'
