#!/usr/bin/env bash
# Compile a native app and freeze an already completed modern engine. This never builds WebKit.
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
SUMMIT_MODE=--bundle
SUMMIT_TARGET=preview
SUMMIT_VARIANT=modern
SUMMIT_MODE_SEEN=0
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
    exec 9>.vm/engine-build.lock
    flock -n 9 || { echo 'An engine build is active; wait for it before bundling the app.' >&2; exit 1; }
    exec 8>.vm/icu-build.lock
    flock -n 8 || { echo 'A private ICU build is active; wait for it before bundling the app.' >&2; exit 1; }
fi
SUMMIT_STAGE=$(mktemp -d .vm/modern-preview-inputs.XXXXXX)
SUMMIT_RESULT=$(mktemp .vm/modern-preview-result.XXXXXX)
SUMMIT_REMOTE_STAGE=
cleanup() {
    rm -rf -- "$SUMMIT_STAGE"
    rm -f -- "$SUMMIT_RESULT"
    if [[ $SUMMIT_REMOTE_STAGE =~ ^/boot/home/summit/modern-preview-inputs\.[A-Za-z0-9]+$ ]]; then
        bash tools/haiku.sh "rm -rf -- '$SUMMIT_REMOTE_STAGE'" || true
    fi
}
trap cleanup EXIT
python3 - "$SUMMIT_STAGE" "$SUMMIT_TARGET" "$SUMMIT_VARIANT" <<'PY'
import hashlib, json, pathlib, shutil, sys
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
    'public_headers': headers,
    'target': target,
    'sha256': hashes,
}
if sys.argv[3] == 'modern-extensions':
    inputs['libzip'] = json.loads((root / 'engine/libzip.lock.json').read_text())
(stage / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
PY
SUMMIT_REMOTE_STAGE=$(bash tools/haiku.sh 'mkdir -p /boot/home/summit && mktemp -d /boot/home/summit/modern-preview-inputs.XXXXXXXX')
if [[ ! $SUMMIT_REMOTE_STAGE =~ ^/boot/home/summit/modern-preview-inputs\.[A-Za-z0-9]+$ ]]; then
    echo 'The VM returned an unexpected staging path.' >&2
    exit 1
fi
tar -C "$SUMMIT_STAGE" -czf - . |
    bash tools/haiku.sh "tar -xzf - -C '$SUMMIT_REMOTE_STAGE'"
bash tools/haiku.sh "python3.10 '$SUMMIT_REMOTE_STAGE/tools/build-modern-browser.py' '$SUMMIT_MODE'" |
    tee "$SUMMIT_RESULT"
if [[ $SUMMIT_MODE == --compile-only ]]; then
    mv -- "$SUMMIT_RESULT" ".vm/$SUMMIT_VARIANT-$SUMMIT_TARGET-compile.json"
else
    mv -- "$SUMMIT_RESULT" ".vm/$SUMMIT_VARIANT-$SUMMIT_TARGET-bundle.json"
fi
