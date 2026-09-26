#!/usr/bin/env bash
# Host test for the engine's StreamingMediaIO (media fetched with range
# requests as Media Kit reads it): builds it against minimal Haiku stubs with
# AddressSanitizer and reads a 9 MB file from a local server that honours
# ranges behind a redirect, delivers slowly, ignores ranges, or answers chunked.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
engine=$here/../../.cache/WebKit/Source/WebCore/platform/graphics/haiku
work=$(mktemp -d)
trap 'rm -rf "$work"; kill $(jobs -p) 2>/dev/null || true' EXIT
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$here/stubs" -I"$engine" "$here/test.cpp" -lcurl -lpthread -o "$work/test"
status=0
for mode in range slow norange chunked; do
    port=$((20000 + RANDOM % 20000))
    python3 "$here/server.py" "$port" 9000000 "$mode" 2>/dev/null &
    server=$!
    sleep 1
    target=/file
    [[ $mode == range ]] && target=/redirect
    echo "== $mode"
    "$work/test" "http://127.0.0.1:$port$target" || status=1
    kill "$server"
done
exit $status
