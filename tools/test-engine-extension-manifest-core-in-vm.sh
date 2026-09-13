#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
exec 9> .vm/extension-manifest-core.lock
flock -n 9 || { echo 'Another extension core preflight is already running.' >&2; exit 2; }

# Snapshot only the extension translation units and their local headers. This
# never writes to the native engine source/build directories or invokes Ninja.
python3 - <<'PY' |
import hashlib
import io
import json
import pathlib
import subprocess
import sys
import tarfile

root = pathlib.Path('.cache/WebKit')
files = {}
for directory in ('Source/WebKit/UIProcess/Extensions', 'Source/WebKit/Shared/Extensions'):
    for path in sorted((root / directory).iterdir()):
        if path.suffix == '.h' or path.name in ('WebExtension.cpp', 'WebExtensionMatchPattern.cpp',
                'WebExtensionLocalization.cpp', 'WebExtensionPermission.cpp', 'WebExtensionUtilities.cpp',
                'WebExtensionResources.cpp', 'WebExtensionMatchPatternProcessPool.cpp'):
            if path.name in files:
                raise SystemExit('Ambiguous snapshot header: ' + path.name)
            files[path.name] = (path, path.read_bytes())
for relative, output in (
        ('Source/WebKit/UIProcess/Extensions/haiku/WebExtensionHaiku.cpp', 'haiku/WebExtensionHaiku.cpp'),
        ('Source/WebKit/UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.h', 'haiku/WebExtensionArchiveHaiku.h'),
        ('Source/WebKit/UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.cpp', 'haiku/WebExtensionArchiveHaiku.cpp'),
        ('Source/WebKit/UIProcess/Extensions/haiku/WebExtensionResourcePathsHaiku.h', 'haiku/WebExtensionResourcePathsHaiku.h'),
        ('Source/WebCore/platform/graphics/Icon.h', 'WebCore/Icon.h')):
    path = root / relative
    files[output] = (path, path.read_bytes())
manifest = {'upstream_commit': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
            'files': {name: {'source': str(path.relative_to(root)),
                             'sha256': hashlib.sha256(data).hexdigest()}
                      for name, (path, data) in files.items()}}
with tarfile.open(fileobj=sys.stdout.buffer, mode='w|') as archive:
    for name, (_, data) in files.items():
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mode = 0o644
        archive.addfile(info, io.BytesIO(data))
    data = (json.dumps(manifest, indent=2) + '\n').encode()
    info = tarfile.TarInfo('source-manifest.json')
    info.size = len(data)
    info.mode = 0o644
    archive.addfile(info, io.BytesIO(data))
PY
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-extension-manifest-tests && tar -xf - -C /boot/home/summit/build-extension-manifest-tests'
tar -cf - tools/test-engine-extension-manifest-core.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
# Arguments are restricted to the literal source choices understood by Python.
case "$*" in
    '') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py' ;;
    '--source WebExtension.cpp') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --source WebExtension.cpp' ;;
    '--header-fix') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --header-fix' ;;
    '--header-fix --source WebExtension.cpp') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --header-fix --source WebExtension.cpp' ;;
    '--core-extraction') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --core-extraction' ;;
    '--core-extraction --source WebExtensionMatchPattern.cpp') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --core-extraction --source WebExtensionMatchPattern.cpp' ;;
    '--source haiku/WebExtensionHaiku.cpp') bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-manifest-core.py --source haiku/WebExtensionHaiku.cpp' ;;
    *) echo 'Use --header-fix or --core-extraction; see the script for supported single-source runs.' >&2; exit 2 ;;
esac
