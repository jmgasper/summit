#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebKit/Platform/IPC/haiku -cf - SocketMonitorHaiku.h SocketMonitorHaiku.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-socket-monitor-tests && tar -xf - -C /boot/home/summit/build-socket-monitor-tests'
tar -cf - tests/EngineSocketMonitorTests.cpp tools/test-engine-socket-monitor.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-socket-monitor.py'
