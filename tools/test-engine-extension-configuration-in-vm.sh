#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
exec 9> .vm/extension-manifest-core.lock
flock -n 9 || { echo 'Another extension compile preflight is already running.' >&2; exit 2; }

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
    for path in sorted((root / directory).glob('*.h')):
        if path.name in files:
            raise SystemExit('Ambiguous snapshot header: ' + path.name)
        files[path.name] = (path, path.read_bytes())
for relative, output in (
    ('Source/WebKit/UIProcess/Extensions/WebExtensionControllerConfiguration.cpp', 'WebExtensionControllerConfiguration.cpp'),
    ('Source/WebKit/UIProcess/Extensions/haiku/WebExtensionControllerConfigurationHaiku.cpp', 'haiku/WebExtensionControllerConfigurationHaiku.cpp'),
    ('Source/WebKit/Platform/haiku/StoragePathsHaiku.h', 'StoragePathsHaiku.h')):
    path = root / relative
    files[output] = (path, path.read_bytes())
test = pathlib.Path('tests/EngineExtensionConfigurationTests.cpp')
files[test.name] = (test, test.read_bytes())
manifest = {'upstream_commit': subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip(),
            'files': {name: {'source': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
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
    bash tools/haiku.sh 'mkdir -p /boot/home/summit/build-extension-configuration-tests && tar -xf - -C /boot/home/summit/build-extension-configuration-tests'
tar -cf - tools/test-engine-extension-manifest-core.py tools/test-engine-extension-configuration.py |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'python3.10 /boot/home/summit/tools/test-engine-extension-configuration.py'
