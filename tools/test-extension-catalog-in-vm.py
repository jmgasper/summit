#!/usr/bin/env python3
"""Compile and run Summit's extension catalog filesystem tests in its Haiku VM."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('src/core/ExtensionCatalog.h', 'src/core/ExtensionCatalog.cpp',
           'tests/ExtensionCatalogTests.cpp', 'vendor/nlohmann/json.hpp')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native():
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    assert all(digest(ROOT / path) == expected for path, expected in inputs.items())
    report = {'scope': 'actual Summit extension catalog and private package staging on native Haiku filesystems; no WebKit activation, signature authentication or installer UI',
              'inputs': inputs, 'runtime_executed': False, 'passed': False}
    command = ['c++', '-std=c++17', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Isrc', '-Ivendor',
               'src/core/ExtensionCatalog.cpp', 'tests/ExtensionCatalogTests.cpp', '-o', 'run']
    compile_result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=180)
    report['compile'] = {'command': command, 'exit': compile_result.returncode,
                         'output': compile_result.stdout + compile_result.stderr}
    if not compile_result.returncode:
        from native_crash_log import NativeCrashLog
        crash_log = NativeCrashLog()
        report['executable_sha256'] = digest(ROOT / 'run')
        try:
            result = subprocess.run([str(ROOT / 'run')], cwd=ROOT, capture_output=True, text=True, timeout=60)
            report.update(exit=result.returncode, output=result.stdout + result.stderr, runtime_executed=True)
        except subprocess.TimeoutExpired:
            report.update(exit=None, output='Catalog runtime timed out and was terminated', runtime_executed=True)
        report['native_crash_log'] = crash_log.finish()
        report['inputs_unchanged'] = all(digest(ROOT / path) == expected for path, expected in inputs.items())
        report['passed'] = report['exit'] == 0 and report['inputs_unchanged'] and report['native_crash_log']['passed']
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['compile']['output'], end='')
    print(report.get('output', ''), end='')
    return 0 if report['passed'] else 1


def host():
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    files = {path: (ROOT / path).read_bytes() for path in SOURCES}
    inputs = {path: hashlib.sha256(data).hexdigest() for path, data in files.items()}
    files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    for name in ('test-extension-catalog-in-vm.py', 'native_crash_log.py'):
        files['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
    packed = io.BytesIO()
    with tarfile.open(fileobj=packed, mode='w:gz') as archive:
        for path, data in files.items():
            item = tarfile.TarInfo(path)
            item.size, item.mode = len(data), 0o600
            archive.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /boot/home/summit/extension-catalog.XXXXXXXX', check=True,
                   text=True, capture_output=True).stdout.strip()
    assert stage.startswith('/boot/home/summit/extension-catalog.') and stage.rsplit('.', 1)[-1].isalnum()
    output = ROOT / '.vm' / Path(stage).name
    output.mkdir(exist_ok=False)
    remote('tar -xzf - -C ' + shlex.quote(stage), input=packed.getvalue(), check=True)
    print('Native extension catalog stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-extension-catalog-in-vm.py', '--native']),
                        stdout=log, stderr=subprocess.STDOUT)
    read = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native result'},
              'host_sources_unchanged': all(digest(ROOT / path) == value for path, value in inputs.items())}
    report['passed'] = not result.returncode and report['native'].get('passed', False) and report['host_sources_unchanged']
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print((output / 'native.log').read_text(), end='')
    print(json.dumps({'result': str(output / 'result.json'), 'passed': report['passed']}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    raise SystemExit(native() if args.native else host())
