#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -czf - tests/EngineSmoke.js |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit && tar xzf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit-webkit/WebKitBuild/Release && DISABLE_ASLR=1 ./bin/jsc /boot/home/summit/tests/EngineSmoke.js && DISABLE_ASLR=1 ./bin/jsc --useJIT=false -e "globalThis.summitJavaScriptOnly = true" /boot/home/summit/tests/EngineSmoke.js'
