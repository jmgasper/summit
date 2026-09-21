#!/usr/bin/env bash
# Compile a native app and freeze an already completed modern engine. This never builds WebKit.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
# The build host is selected with SUMMIT_REMOTE_SHELL: tools/haiku.sh (the QEMU
# VM, the default) or tools/ws.sh (the workstation). SUMMIT_REMOTE_TAG names the
# host in the host-side lock files so both can build at the same time.
SUMMIT_REMOTE_SHELL=${SUMMIT_REMOTE_SHELL:-tools/haiku.sh}
SUMMIT_REMOTE_TAG=${SUMMIT_REMOTE_TAG:-vm}
if [[ ! $SUMMIT_REMOTE_SHELL =~ ^tools/(haiku|ws)\.sh$ || ! $SUMMIT_REMOTE_TAG =~ ^[a-z][a-z0-9-]{0,15}$ ]]; then
    echo 'SUMMIT_REMOTE_SHELL must be tools/haiku.sh or tools/ws.sh.' >&2
    exit 2
fi
SUMMIT_MODE=--bundle
SUMMIT_TARGET=preview
SUMMIT_VARIANT=modern
SUMMIT_MODE_SEEN=0
SUMMIT_NATIVE_BUILD_ROOT=${SUMMIT_NATIVE_BUILD_ROOT:-/boot/home/summit}
case "$SUMMIT_NATIVE_BUILD_ROOT" in
    /boot/home/summit|/SummitExtensions/summit) ;;
    *) echo 'SUMMIT_NATIVE_BUILD_ROOT must be /boot/home/summit or /SummitExtensions/summit.' >&2; exit 2 ;;
esac
for SUMMIT_ARGUMENT in "$@"; do
    case "$SUMMIT_ARGUMENT" in
        --bundle|--compile-only)
            if (( SUMMIT_MODE_SEEN )); then
                echo 'Select only one build mode.' >&2
                exit 2
            fi
            SUMMIT_MODE=$SUMMIT_ARGUMENT
            SUMMIT_MODE_SEEN=1
            ;;
        --browser) SUMMIT_TARGET=browser ;;
        --modern-extensions) SUMMIT_VARIANT=modern-extensions ;;
        *) echo 'Usage: build-modern-browser-in-vm.sh [--bundle|--compile-only] [--browser] [--modern-extensions]' >&2; exit 2 ;;
    esac
done
mkdir -p .vm
if [[ $SUMMIT_MODE == --bundle ]]; then
    exec 9>".vm/engine-build-$SUMMIT_REMOTE_TAG-${SUMMIT_ENGINE_BUILD_NAME:-Modern}.lock"
    flock -n 9 || { echo 'An engine build is active; wait for it before bundling the app.' >&2; exit 1; }
    exec 8>".vm/icu-build-$SUMMIT_REMOTE_TAG.lock"
    flock -n 8 || { echo 'A private ICU build is active; wait for it before bundling the app.' >&2; exit 1; }
fi
SUMMIT_STAGE=$(mktemp -d .vm/modern-preview-inputs.XXXXXX)
SUMMIT_RESULT=$(mktemp .vm/modern-preview-result.XXXXXX)
SUMMIT_REMOTE_STAGE=
cleanup() {
    rm -rf -- "$SUMMIT_STAGE"
    rm -f -- "$SUMMIT_RESULT"
    if [[ $SUMMIT_REMOTE_STAGE =~ ^(/boot/home/summit|/SummitExtensions/summit)/modern-preview-inputs\.[A-Za-z0-9]+$ ]]; then
        bash "$SUMMIT_REMOTE_SHELL" "rm -rf -- '$SUMMIT_REMOTE_STAGE'" || true
    fi
}
trap cleanup EXIT
python3 - "$SUMMIT_STAGE" "$SUMMIT_TARGET" "$SUMMIT_VARIANT" <<'PY'
import hashlib, json, os, pathlib, shutil, sys
root = pathlib.Path.cwd()
stage = pathlib.Path(sys.argv[1])
target = sys.argv[2]
lock = json.loads((root / 'engine/sources.lock.json').read_text())
patch = root / lock['patch']['path']
if hashlib.sha256(patch.read_bytes()).hexdigest() != lock['patch']['sha256']:
    raise SystemExit('Engine patch does not match sources.lock.json.')
headers = {
    'WebKitView.h': 'UIProcess/API/haiku/WebKitView.h',
    'WebKitContext.h': 'UIProcess/API/haiku/WebKitContext.h',
    'WebKitExtensionPermission.h': 'UIProcess/API/haiku/WebKitExtensionPermission.h',
    'WebKitInfo.h': 'UIProcess/API/haiku/WebKitInfo.h',
    'WKBase.h': 'Shared/API/c/WKBase.h',
    'WKDeclarationSpecifiers.h': 'Shared/API/c/WKDeclarationSpecifiers.h',
    'WKBaseHaiku.h': 'Shared/API/c/haiku/WKBaseHaiku.h',
}
files = {'tests/ModernBrowser.cpp': 'tests/ModernBrowser.cpp',
         'tools/build-modern-browser.py': 'tools/build-modern-browser.py',
         'LICENSE-Summit': 'LICENSE'}
if target == 'browser':
    for directory in ['src', 'vendor', 'resources']:
        for source in sorted((root / directory).rglob('*')):
            if source.is_file():
                relative = str(source.relative_to(root))
                files[relative] = relative
for name, path in headers.items():
    files['include/WebKit/' + name] = '.cache/WebKit/Source/WebKit/' + path
hashes = {}
for destination, source in files.items():
    output = stage / destination
    output.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / source, output)
    hashes[destination] = hashlib.sha256(output.read_bytes()).hexdigest()
inputs = {
    'engine': lock,
    'icu': json.loads((root / 'engine/icu.lock.json').read_text()),
    'engine_variant': sys.argv[3],
    'engine_build_name': os.environ.get('SUMMIT_ENGINE_BUILD_NAME', 'Modern'),
    'public_headers': headers,
    'target': target,
    'sha256': hashes,
}
if sys.argv[3] == 'modern-extensions':
    inputs['libzip'] = json.loads((root / 'engine/libzip.lock.json').read_text())
(stage / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
PY
SUMMIT_REMOTE_STAGE=$(bash "$SUMMIT_REMOTE_SHELL" "mkdir -p '$SUMMIT_NATIVE_BUILD_ROOT' '$SUMMIT_NATIVE_BUILD_ROOT/tmp' && mktemp -d '$SUMMIT_NATIVE_BUILD_ROOT/modern-preview-inputs.XXXXXXXX'")
if [[ ! $SUMMIT_REMOTE_STAGE =~ ^(/boot/home/summit|/SummitExtensions/summit)/modern-preview-inputs\.[A-Za-z0-9]+$ ]]; then
    echo 'The VM returned an unexpected staging path.' >&2
    exit 1
fi
tar -C "$SUMMIT_STAGE" -czf - . |
    bash "$SUMMIT_REMOTE_SHELL" "tar -xzf - -C '$SUMMIT_REMOTE_STAGE'"
bash "$SUMMIT_REMOTE_SHELL" "env TMPDIR='$SUMMIT_NATIVE_BUILD_ROOT/tmp' python3.10 '$SUMMIT_REMOTE_STAGE/tools/build-modern-browser.py' '$SUMMIT_MODE' --build-root '$SUMMIT_NATIVE_BUILD_ROOT'" |
    tee "$SUMMIT_RESULT"
if [[ $SUMMIT_MODE == --compile-only ]]; then
    mv -- "$SUMMIT_RESULT" ".vm/$SUMMIT_REMOTE_TAG-$SUMMIT_VARIANT-$SUMMIT_TARGET-compile.json"
else
    mv -- "$SUMMIT_RESULT" ".vm/$SUMMIT_REMOTE_TAG-$SUMMIT_VARIANT-$SUMMIT_TARGET-bundle.json"
fi
