#!/usr/bin/env python3
"""Export verified IPC generator outputs from an isolated native compile stage."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shlex
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
NAMES = ('source-manifest.json', 'MessageNames.h', 'MessageNames.cpp',
         'GeneratedSerializers.h', 'GeneratedSerializersShared.cpp',
         'GeneratedSerializersSharedWebCoreArgumentCodersNetwork.cpp',
         'WebKitPlatformGeneratedSerializers.cpp')


def main(stage, output):
    stage = stage.rstrip('/')
    if not re.fullmatch(r'/boot/home/summit/extension-lifecycle-inputs\.[A-Za-z0-9]{8}', stage):
        raise RuntimeError('Expected an isolated native extension compile stage')
    source = stage + '/sources'
    if output:
        destination = Path(output).resolve()
        destination.mkdir(parents=True, exist_ok=False)
    else:
        (ROOT / '.vm').mkdir(exist_ok=True)
        destination = Path(tempfile.mkdtemp(prefix='ipc-generation-', dir=ROOT / '.vm'))

    def remote(command):
        return subprocess.check_output(['bash', str(ROOT / 'tools/haiku.sh'), command])

    archive_data = remote(shlex.join(['tar', '-C', source, '-czf', '-', *NAMES]))
    with tarfile.open(fileobj=io.BytesIO(archive_data), mode='r:gz') as archive:
        seen = set()
        for member in archive.getmembers():
            if member.name not in NAMES or not member.isfile() or member.name in seen:
                raise RuntimeError('Unexpected generated archive member: ' + member.name)
            seen.add(member.name)
            (destination / member.name).write_bytes(archive.extractfile(member).read())
        if seen != set(NAMES):
            raise RuntimeError('Generated output archive is incomplete')
    manifest = json.loads((destination / 'source-manifest.json').read_text())
    if 'ipc_generation' not in manifest or 'serialization_generation' not in manifest:
        raise RuntimeError('The native stage did not record both complete generation steps')
    hashes = {name: hashlib.sha256((destination / name).read_bytes()).hexdigest() for name in NAMES}
    for name in NAMES[1:]:
        expected = (manifest['serialization_generation']['generated_files'].get(name)
                    or manifest['ipc_generation']['generated_headers'].get(name))
        if expected and hashes[name] != expected:
            raise RuntimeError('Generated output differs from its recorded hash: ' + name)
        if not expected and name not in ('MessageNames.cpp', 'WebKitPlatformGeneratedSerializers.cpp'):
            raise RuntimeError('No generation record for output: ' + name)
    # Also verify the two auxiliary outputs and the manifest, and detect an
    # output being replaced while its archive was read.
    script = ('import hashlib,json,pathlib; p=pathlib.Path(' + repr(source) + '); '
              'print(json.dumps({n:hashlib.sha256((p/n).read_bytes()).hexdigest() for n in '
              + repr(NAMES) + '}))')
    remote_hashes = json.loads(remote('python3.10 -c ' + shlex.quote(script)))
    if hashes != remote_hashes:
        raise RuntimeError('Native generator output changed during export')
    report = {'native_stage': source, 'source_manifest': manifest, 'files': hashes, 'copied_unchanged': True}
    (destination / 'generation.json').write_text(json.dumps(report, indent=2) + '\n')
    print(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True)
    parser.add_argument('--output', help='New local export directory; defaults to an isolated .vm directory')
    args = parser.parse_args()
    main(args.stage, args.output)
