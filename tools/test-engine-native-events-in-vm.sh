#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -C .cache/WebKit/Source/WebKit/Shared -cf - NativeWebKeyboardEvent.h NativeWebMouseEvent.h NativeWebWheelEvent.h \
    WebEvent.h WebKeyboardEvent.h WebMouseEvent.h WebWheelEvent.h WebEventModifier.h WebEventType.h WebEventPhase.h \
    WebEvent.cpp WebKeyboardEvent.cpp WebMouseEvent.cpp WebWheelEvent.cpp WebEventConversion.cpp WebEventConversion.h EditingRange.h |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-native-event-tests && tar -xf - -C /boot/home/summit/build-native-event-tests'
tar -C .cache/WebKit/Source/WebKit/Shared/haiku -cf - NativeWebEventsHaiku.cpp EditingCommandsHaiku.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-native-event-tests'
tar -C .cache/WebKit/Source/WebCore/platform/haiku -cf - PlatformKeyboardEventHaiku.cpp |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit/build-native-event-tests'
tar -cf - tests/EngineNativeEventTests.cpp tools/test-engine-native-events.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-native-events.py'
