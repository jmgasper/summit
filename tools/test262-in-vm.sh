#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
SUMMIT_TEST262_SCOPE=${1:-smoke}
case "$SUMMIT_TEST262_SCOPE" in
    smoke|atomics|intl|full) ;;
    *) echo 'Usage: test262-in-vm.sh [smoke|atomics|intl|full]' >&2; exit 2 ;;
esac
mkdir -p .vm
exec 8>.vm/test262.lock
if ! flock -n 8; then
    echo 'Another Test262 run is active.' >&2
    exit 1
fi
python3 tools/prepare-webkit.py
if [[ $SUMMIT_TEST262_SCOPE != smoke ]]; then
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
elif [[ $SUMMIT_TEST262_SCOPE == atomics ]]; then
    SUMMIT_TEST262_OPTIONS=(--test-only test/built-ins/Atomics)
elif [[ $SUMMIT_TEST262_SCOPE == intl ]]; then
    SUMMIT_TEST262_OPTIONS=(--test-only test/intl402 --test-only test/language/identifiers
        --test-only test/staging/sm/String)
fi
SUMMIT_TEST262_JSC=${SUMMIT_TEST262_JSC:-/boot/home/summit-webkit/WebKitBuild/Release/bin/jsc}
SUMMIT_TEST262_LIBRARY=${SUMMIT_TEST262_LIBRARY:-/boot/home/summit-webkit/WebKitBuild/Release/lib/libJavaScriptCore.so.18.7.4}
if [[ ! $SUMMIT_TEST262_JSC =~ ^/[A-Za-z0-9/_.+-]+$ || ! $SUMMIT_TEST262_LIBRARY =~ ^/[A-Za-z0-9/_.+-]+$ ]]; then
    echo 'Native engine paths must be absolute and contain no shell metacharacters.' >&2
    exit 2
fi
# Each invocation gets a new report directory. An interrupted upstream run does
# not write reports and must never be mistaken for an earlier successful run.
SUMMIT_TEST262_RUN=$(bash tools/haiku.sh 'mktemp -d /boot/home/summit-test262.XXXXXXXX')
if [[ ! $SUMMIT_TEST262_RUN =~ ^/boot/home/summit-test262\.[A-Za-z0-9]+$ ]]; then
    echo 'Could not create a native Test262 working directory.' >&2
    exit 1
fi
echo "Test262 working directory: $SUMMIT_TEST262_RUN"
echo "JavaScriptCore shell: $SUMMIT_TEST262_JSC"
SUMMIT_JSC_BEFORE=$(bash tools/haiku.sh "sha256sum '$SUMMIT_TEST262_JSC' '$SUMMIT_TEST262_LIBRARY'")
bash tools/haiku.sh "cd '$SUMMIT_TEST262_RUN' && SUMMIT_WEBKIT_SOURCE=/boot/home/summit-webkit python3.10 /boot/home/summit/tools/test262-haiku.py --jsc '$SUMMIT_TEST262_JSC' --child-processes 1 --timeout 10000 --ignore-expectations" "${SUMMIT_TEST262_OPTIONS[@]}" || SUMMIT_TEST262_RESULT=$?
SUMMIT_JSC_AFTER=$(bash tools/haiku.sh "sha256sum '$SUMMIT_TEST262_JSC' '$SUMMIT_TEST262_LIBRARY'")
SUMMIT_TEST262_OUTPUT=.vm/test262-results
if [[ $SUMMIT_TEST262_SCOPE != smoke ]]; then
    SUMMIT_TEST262_OUTPUT=.vm/test262-$SUMMIT_TEST262_SCOPE-results
fi
if bash tools/haiku.sh "test -f '$SUMMIT_TEST262_RUN/test262-results/results.yaml'"; then
    if [[ -e $SUMMIT_TEST262_OUTPUT ]]; then
        mv "$SUMMIT_TEST262_OUTPUT" "$SUMMIT_TEST262_OUTPUT.previous.$(date +%Y%m%dT%H%M%S).$$"
    fi
    mkdir -p "$SUMMIT_TEST262_OUTPUT"
    bash tools/haiku.sh "tar -czf - -C '$SUMMIT_TEST262_RUN/test262-results' ." |
        tar -xzf - -C "$SUMMIT_TEST262_OUTPUT"
    printf '%s\n' "$SUMMIT_JSC_BEFORE" > "$SUMMIT_TEST262_OUTPUT/engine-sha256.txt"
else
    echo 'This Test262 invocation produced no result report.' >&2
    if [[ $SUMMIT_TEST262_RESULT == 0 ]]; then SUMMIT_TEST262_RESULT=1; fi
fi
if [[ $SUMMIT_JSC_BEFORE != "$SUMMIT_JSC_AFTER" ]]; then
    echo 'The JavaScriptCore shell or library changed during Test262; rerun before using these results.' >&2
    exit 1
fi
exit "$SUMMIT_TEST262_RESULT"
