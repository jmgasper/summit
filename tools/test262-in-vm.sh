#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
python3 tools/prepare-webkit.py
git -C .cache/WebKit sparse-checkout add JSTests/test262/harness \
    JSTests/test262/test/intl402/ListFormat \
    JSTests/test262/test/built-ins/Array/prototype/toSorted \
    JSTests/test262/test/built-ins/Object/groupBy
tar -C .cache/WebKit -czf - JSTests/test262 |
    bash tools/haiku.sh 'tar xzf - -C /boot/home/summit-webkit'
tar -czf - tools/test262-haiku.py |
    bash tools/haiku.sh 'tar xzf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit-webkit && python3.10 /boot/home/summit/tools/test262-haiku.py --jsc /boot/home/summit-webkit/WebKitBuild/Release/bin/jsc --child-processes 1 --timeout 10000 --ignore-expectations --ignore-config --test-only test/intl402/ListFormat --test-only test/built-ins/Array/prototype/toSorted --test-only test/built-ins/Object/groupBy'
mkdir -p .vm/test262-results
bash tools/haiku.sh 'tar -czf - -C /boot/home/summit-webkit/test262-results .' |
    tar -xzf - -C .vm/test262-results
