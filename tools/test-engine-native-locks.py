#!/usr/bin/env python3
"""Exercise real Haiku locks and loopers using a frozen browser's library order."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def native(bundle_path):
    spec = importlib.util.spec_from_file_location(
        'bundle_verification', ROOT / 'tools/test-modern-contexts-native.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if verifier.digest(ROOT / name) != expected:
            raise RuntimeError('Changed staged input: ' + name)
    bundle, _, manifest, before = verifier.verified_bundle(bundle_path)
    library = bundle / 'lib'
    target = ROOT / 'NativeLockTests'
    # WebKit must be direct, while JavaScriptCore remains transitive, exactly as
    # in Summit. Linking JavaScriptCore directly hides the loader-order bug.
    command = ['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
               '-I/boot/system/develop/headers/private/shared',
               str(ROOT / 'tests/EngineNativeLockTests.cpp'), '-L' + str(library),
               '-Wl,--no-as-needed', '-lWebKit', '-lbe', '-lnetwork',
               '-Wl,-rpath,' + str(library), '-o', str(target)]
    compile_result = subprocess.run(command, capture_output=True, text=True)
    report = {'kind': 'native-browser-lock-regression', 'stage': str(ROOT),
              'bundle': str(bundle), 'engine': manifest['inputs']['engine'],
              'inputs': inputs, 'compile_command': command,
              'compile_exit': compile_result.returncode,
              'diagnostics': compile_result.stdout + compile_result.stderr,
              'passed': False}
    print(report['diagnostics'], end='', flush=True)
    if compile_result.returncode == 0:
        environment = dict(os.environ)
        for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
            environment.pop(name, None)
        environment['LIBRARY_PATH'] = str(library) + ':/boot/system/lib'
        runtime = [str(target), str((library / 'libWebKit.so').resolve(strict=True))]
        report['run_command'] = runtime
        report['executable_sha256'] = verifier.digest(target)
        try:
            result = subprocess.run(runtime, env=environment, capture_output=True,
                                    text=True, timeout=30)
            report.update(runtime_exit=result.returncode, stdout=result.stdout,
                          stderr=result.stderr)
        except subprocess.TimeoutExpired as error:
            def decoded(value):
                return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
            report.update(runtime_exit=None, timeout=True, stdout=decoded(error.stdout),
                          stderr=decoded(error.stderr))
        print(report['stdout'] + report['stderr'], end='', flush=True)
        verifier.verify_symlinks(bundle, manifest)
        report['bundle_unchanged'] = all(verifier.digest(pathlib.Path(name)) == expected
                                         for name, expected in before.items())
        report['passed'] = (report['runtime_exit'] == 0 and report['bundle_unchanged']
                            and 'NATIVE_LOCK_RESULT PASS checks=14' in report['stdout'])
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


def host(bundle):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/native-lock-inputs.XXXXXXXX',
                   check=True, capture_output=True, text=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/native-lock-inputs.'):
        raise RuntimeError('Unexpected native staging path')
    output = pathlib.Path(tempfile.mkdtemp(prefix='native-lock-inputs.', dir=ROOT / '.vm'))
    paths = ['tests/EngineNativeLockTests.cpp', 'tools/test-engine-native-locks.py',
             'tools/test-modern-contexts-native.py']
    files = {name: (ROOT / name).read_bytes() for name in paths}
    inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in files.items():
            item = tarfile.TarInfo(name)
            item.size = len(data)
            stream.addfile(item, io.BytesIO(data))
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'bundle': bundle}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native stage: ' + stage + '\nHost evidence: ' + str(output), flush=True)
    with (output / 'native.log').open('w') as stream:
        result = remote('python3.10 ' + shlex.quote(stage + '/tools/test-engine-native-locks.py')
                        + ' --native --bundle ' + shlex.quote(bundle),
                        stdout=stream, stderr=subprocess.STDOUT)
    fetched = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True)
    if fetched.returncode == 0:
        (output / 'result.json').write_bytes(fetched.stdout)
    print((output / 'native.log').read_text(), end='')
    print('Result: ' + str(output / 'result.json'))
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, help='Completed frozen native browser bundle')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    sys.exit(native(args.bundle) if args.native else host(args.bundle))
