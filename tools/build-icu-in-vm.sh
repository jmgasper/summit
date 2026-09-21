#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# The build host is selected with SUMMIT_REMOTE_SHELL: tools/haiku.sh (the QEMU
# VM, the default) or tools/ws.sh (the workstation). SUMMIT_REMOTE_TAG names the
# host in the host-side lock files so both can build at the same time.
SUMMIT_REMOTE_SHELL=${SUMMIT_REMOTE_SHELL:-tools/haiku.sh}
SUMMIT_REMOTE_TAG=${SUMMIT_REMOTE_TAG:-vm}
SUMMIT_ICU_JOBS=${SUMMIT_ICU_JOBS:-6}
if [[ ! $SUMMIT_ICU_JOBS =~ ^[1-9][0-9]?$ ]]; then
    echo 'SUMMIT_ICU_JOBS must be an integer between 1 and 99.' >&2
    exit 2
fi
if [[ ! $SUMMIT_REMOTE_SHELL =~ ^tools/(haiku|ws)\.sh$ || ! $SUMMIT_REMOTE_TAG =~ ^[a-z][a-z0-9-]{0,15}$ ]]; then
    echo 'SUMMIT_REMOTE_SHELL must be tools/haiku.sh or tools/ws.sh.' >&2
    exit 2
fi
mkdir -p .cache .vm
exec 8>".vm/icu-build-$SUMMIT_REMOTE_TAG.lock"
flock -n 8 || { echo 'A private ICU build is already active.' >&2; exit 1; }
python3 - <<'PY'
import hashlib, json, pathlib, urllib.request
lock = json.loads(pathlib.Path('engine/icu.lock.json').read_text())
if lock['version'] != '78.3':
    raise SystemExit('Update the native build paths when changing the ICU version.')
path = pathlib.Path('.cache/icu4c-78.3-sources.tgz')
if not path.exists():
    temporary = path.with_suffix('.download')
    urllib.request.urlretrieve(lock['url'], temporary)
    if hashlib.sha256(temporary.read_bytes()).hexdigest() != lock['sha256']:
        raise SystemExit('ICU source hash mismatch.')
    temporary.rename(path)
if hashlib.sha256(path.read_bytes()).hexdigest() != lock['sha256']:
    raise SystemExit('ICU source hash mismatch.')
print('Verified ICU', lock['version'], lock['sha256'])
PY
bash "$SUMMIT_REMOTE_SHELL" 'mkdir -p /boot/home/summit-deps/icu-78.3 && tar -xzf - -C /boot/home/summit-deps/icu-78.3 --strip-components=1' < .cache/icu4c-78.3-sources.tgz
# Haiku resolves relative runtime library paths differently from Linux. Supply
# absolute paths throughout building, data packaging, installation and tests.
bash "$SUMMIT_REMOTE_SHELL" '
    export LIBRARY_PATH=/boot/home/summit-deps/icu-78.3/build/lib:/boot/home/summit-deps/icu-78.3/build/stubdata:/boot/home/summit-deps/icu-78.3/build/tools/ctestfw:/boot/system/lib
    cd /boot/home/summit-deps/icu-78.3 &&
    mkdir -p build && cd build &&
    ../source/configure --prefix=/boot/home/summit-deps/icu78 --disable-static --disable-samples &&
    make -j'"$SUMMIT_ICU_JOBS"' && make install && make -j'"$SUMMIT_ICU_JOBS"' check'
