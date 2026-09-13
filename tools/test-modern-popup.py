#!/usr/bin/env python3
"""Compile the isolated modern popup proxy and run its actual native menu checks.

Uses the existing frozen context header snapshot, never synchronizes engine
sources or invokes its build system. Only one compiler is launched at a time.
"""
import argparse
import hashlib
import io
import json
import pathlib
import shlex
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def native_main():
    stage = ROOT
    snapshot = pathlib.Path('/boot/home/summit/build-context-compile-agent')
    inputs = json.loads((stage / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if hashlib.sha256((stage / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Staged popup input changed: ' + name)
    flags = json.loads((snapshot / 'flags.json').read_text())
    flags = [flag.replace(str(snapshot), str(stage)) if flag == '-I' + str(snapshot) or flag == str(snapshot) else flag for flag in flags]
    flags += ['-DRELEASE_WITHOUT_OPTIMIZATIONS']
    report = {'inputs': inputs, 'stage': str(stage), 'commands': [], 'compile_exits': []}
    commands = [
        ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + str(stage), str(stage / 'tests/ModernPopupMenuTests.cpp'), '-lbe', '-o', str(stage / 'ModernPopupMenuTests')],
        ['c++', *flags, '-c', str(stage / 'WebPopupMenuProxyHaiku.cpp'), '-o', str(stage / 'WebPopupMenuProxyHaiku.o')],
    ]
    for command in commands:
        print('Compiling ' + pathlib.Path(command[-1]).name, flush=True)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['commands'].append(command)
        report['compile_exits'].append(result.returncode)
        if result.returncode:
            (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
            return 1
    try:
        result = subprocess.run([str(stage / 'ModernPopupMenuTests')], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=110)
        output = result.stdout
        report['runtime_exit'] = result.returncode
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ''
        if isinstance(output, bytes):
            output = output.decode('utf-8', errors='replace')
        report['runtime_exit'] = None
        report['timeout'] = True
    print(output, end='', flush=True)
    (stage / 'runtime.log').write_text(output)
    report['passed'] = report['runtime_exit'] == 0 and 'POPUP_RESULT PASS checks=' in output
    (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(stage / 'result.json')}), flush=True)
    return 0 if report['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    if args.native:
        return native_main()
    directory = ROOT / '.cache/WebKit/Source/WebKit/UIProcess/haiku'
    files = {name: directory / name for name in ('NativeSelectPopupHaiku.h', 'WebPopupMenuProxyHaiku.h', 'WebPopupMenuProxyHaiku.cpp', 'WebViewStateHaiku.h', 'JavaScriptDialogRequestsHaiku.h')}
    files.update({'tests/ModernPopupMenuTests.cpp': ROOT / 'tests/ModernPopupMenuTests.cpp', 'tools/test-modern-popup.py': pathlib.Path(__file__)})
    content = {name: path.read_bytes() for name, path in files.items()}
    content['inputs.json'] = json.dumps({name: hashlib.sha256(data).hexdigest() for name, data in content.items()}, indent=2).encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o600
            stream.addfile(entry, io.BytesIO(data))
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/modern-popup-inputs.XXXXXXXX', check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/modern-popup-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native staging directory')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    output = ROOT / '.vm' / pathlib.Path(stage).name
    output.mkdir(exist_ok=False)
    print('Native popup stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as stream:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-modern-popup.py', '--native']), stdout=stream, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    report = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, stdout=subprocess.PIPE)
    if report.returncode == 0:
        (output / 'result.json').write_text(report.stdout)
    print('Host evidence: ' + str(output), flush=True)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
