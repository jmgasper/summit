#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
SUMMIT_TEST262_SCOPE=${1:-smoke}
case "$SUMMIT_TEST262_SCOPE" in
    smoke|full) ;;
    *) echo 'Usage: test262-in-vm.sh [smoke|full]' >&2; exit 2 ;;
esac
mkdir -p .vm
exec 8>.vm/test262.lock
if ! flock -n 8; then
    echo 'Another Test262 run is active.' >&2
    exit 1
fi
python3 tools/prepare-webkit.py
if [[ $SUMMIT_TEST262_SCOPE == full ]]; then
    git -C .cache/WebKit sparse-checkout add JSTests/test262
else
    git -C .cache/WebKit sparse-checkout add JSTests/test262/harness \
        JSTests/test262/test/intl402/ListFormat \
        JSTests/test262/test/built-ins/Array/prototype/toSorted \
        JSTests/test262/test/built-ins/Object/groupBy
fi
tar -C .cache/WebKit -czf - JSTests/test262 |
    bash tools/haiku.sh 'tar xzf - -C /boot/home/summit-webkit'
tar -czf - tools/test262-haiku.py |
    bash tools/haiku.sh 'tar xzf - -C /boot/home/summit'
SUMMIT_TEST262_RESULT=0
# Preserve the runner's skip configuration for the full suite: it distinguishes
# unsupported proposals from the language features enabled in this engine.
SUMMIT_TEST262_OPTIONS=()
if [[ $SUMMIT_TEST262_SCOPE == smoke ]]; then
    SUMMIT_TEST262_OPTIONS=(--ignore-config --test-only test/intl402/ListFormat
        --test-only test/built-ins/Array/prototype/toSorted --test-only test/built-ins/Object/groupBy)
fi
SUMMIT_JSC_BEFORE=$(bash tools/haiku.sh 'sha256sum /boot/home/summit-webkit/WebKitBuild/Release/lib/libJavaScriptCore.so.18.7.4')
bash tools/haiku.sh 'cd /boot/home/summit-webkit && python3.10 /boot/home/summit/tools/test262-haiku.py --jsc /boot/home/summit-webkit/WebKitBuild/Release/bin/jsc --child-processes 1 --timeout 10000 --ignore-expectations' "${SUMMIT_TEST262_OPTIONS[@]}" || SUMMIT_TEST262_RESULT=$?
SUMMIT_JSC_AFTER=$(bash tools/haiku.sh 'sha256sum /boot/home/summit-webkit/WebKitBuild/Release/lib/libJavaScriptCore.so.18.7.4')
SUMMIT_TEST262_OUTPUT=.vm/test262-results
if [[ $SUMMIT_TEST262_SCOPE == full ]]; then
    SUMMIT_TEST262_OUTPUT=.vm/test262-full-results
fi
mkdir -p "$SUMMIT_TEST262_OUTPUT"
bash tools/haiku.sh 'tar -czf - -C /boot/home/summit-webkit/test262-results .' |
    tar -xzf - -C "$SUMMIT_TEST262_OUTPUT"
if [[ $SUMMIT_JSC_BEFORE != "$SUMMIT_JSC_AFTER" ]]; then
    echo 'The JavaScriptCore library changed during Test262; rerun before using these results.' >&2
    exit 1
fi
exit "$SUMMIT_TEST262_RESULT"
