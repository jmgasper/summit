#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
mkdir -p .vm artifacts
exec 8>.vm/current-bundle-copy.lock
flock -n 8 || { echo 'A current-browser bundle copy is already active.' >&2; exit 1; }
python3 - <<'PY'
import json
with open('.vm/current-browser-build.json') as file:
    built = json.load(file)['engine_inputs']
with open('engine/sources.lock.json') as file:
    current = json.load(file)
if built != current:
    raise SystemExit('The native bundle and local engine sources differ; rebuild before copying source with it.')
PY
python3 tools/prepare-webkit.py
SUMMIT_BUNDLE=$(python3 -c 'import json; print(json.load(open(".vm/current-browser-build.json"))["bundle"])')
if [[ ! $SUMMIT_BUNDLE =~ ^/boot/home/summit/build-current/bundle-[A-Za-z0-9_-]+$ ]]; then
    echo 'Unexpected native bundle path.' >&2
    exit 1
fi
SUMMIT_IMPORT=$(mktemp -d artifacts/current-browser-import.XXXXXX)
trap 'rm -rf -- "$SUMMIT_IMPORT"' EXIT
SUMMIT_PAYLOAD="$SUMMIT_IMPORT/${SUMMIT_BUNDLE##*/}"
mkdir -p "$SUMMIT_PAYLOAD"
bash tools/haiku.sh "tar -C $SUMMIT_BUNDLE -czf - ." |
    tar -xzf - --no-same-owner -C "$SUMMIT_PAYLOAD"
mkdir -p "$SUMMIT_PAYLOAD/engine-source"
cp engine/sources.lock.json engine/README.md engine/patches/0001-haiku-port.patch "$SUMMIT_PAYLOAD/engine-source/"
cp tools/build-webkit-in-vm.sh tools/prepare-webkit.py "$SUMMIT_PAYLOAD/engine-source/"
tar -C .cache/WebKit -I 'xz -T4 -3' --exclude=__pycache__ -cf "$SUMMIT_PAYLOAD/WebKit-sources.tar.xz" \
    CMakeLists.txt Configurations Source Tools
python3 tools/prepare-webkit.py
if [[ -e artifacts/current-browser ]]; then
    mv artifacts/current-browser "artifacts/current-browser.previous.$(date +%Y%m%d-%H%M%S)"
fi
mv "$SUMMIT_PAYLOAD" artifacts/current-browser
printf 'Native current-engine browser: %s/Summit\nHost bundle: %s/artifacts/current-browser\n' "$SUMMIT_BUNDLE" "$SUMMIT_ROOT"
