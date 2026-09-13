#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebKit/Platform/IPC/haiku -cf - MessagePacketHaiku.h MessagePacketHaiku.cpp |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-message-packet-tests && tar -xf - -C /boot/home/summit/build-message-packet-tests'
tar -C .cache/WebKit/Source/WebKit/Platform/IPC/unix -cf - MessageInfo.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-message-packet-tests'
tar -C .cache/WebKit/Source/WebKit/Platform/IPC -cf - Attachment.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-message-packet-tests'
tar -C .cache/WebKit/Source/WebCore/platform -cf - SharedMemory.cpp SharedMemory.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-message-packet-tests'
tar -C .cache/WebKit/Source/WebCore/platform/haiku -cf - SharedMemoryHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-message-packet-tests'
tar -cf - tests/EngineMessagePacketTests.cpp tools/test-engine-message-packets.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-message-packets.py'
