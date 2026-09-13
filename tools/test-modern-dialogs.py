#!/usr/bin/env python3
"""Compile copied modern bridge sources and run the actual native dialog widgets.

Uses the existing frozen context framework headers, without synchronizing engine
sources or invoking its build system. The native compiler commands run serially.
This checks the dialog components, not JavaScript IPC in a linked browser.
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


def native_main(widget_only=False):
    stage = ROOT
    snapshot = pathlib.Path('/boot/home/summit/build-context-compile-agent')
    inputs = json.loads((stage / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if hashlib.sha256((stage / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError('Staged dialog input changed: ' + name)
    (stage / 'WebKit').mkdir()
    for name in ('WebKitView.h', 'WebKitContext.h'):
        (stage / 'WebKit' / name).symlink_to(stage / name)
    flags = json.loads((snapshot / 'flags.json').read_text())
    flags = [flag.replace(str(snapshot), str(stage))
             if flag == '-I' + str(snapshot) or flag == str(snapshot) else flag for flag in flags]
    flags += ['-DRELEASE_WITHOUT_OPTIMIZATIONS']
    report = {'inputs': inputs, 'stage': str(stage), 'commands': [], 'compile_exits': []}
    commands = [['c++', '-std=c++23', '-O0', '-Wall', '-Wextra', '-Wno-multichar', '-I' + str(stage),
                 str(stage / 'tests/EngineNativeDialogTests.cpp'), '-lbe', '-o', str(stage / 'EngineNativeDialogTests')]]
    if not widget_only:
        commands += [['c++', *flags, '-c', str(stage / name), '-o', str(stage / (name + '.o'))]
                     for name in ('WebKitView.cpp', 'WebView.cpp')]
        commands += [['c++', *flags, '-fsyntax-only', str(stage / 'WebPageProxy.cpp')]]
    for command in commands:
        print('Compiling ' + command[-1], flush=True)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['commands'].append(command)
        report['compile_exits'].append(result.returncode)
        (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        if result.returncode:
            return 1
    result = subprocess.run([str(stage / 'EngineNativeDialogTests')], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    print(result.stdout, end='', flush=True)
    (stage / 'runtime.log').write_text(result.stdout)
    report['runtime_exit'] = result.returncode
    report['passed'] = result.returncode == 0 and 'Native dialog checks:' in result.stdout
    report['inputs_unchanged'] = all(hashlib.sha256((stage / name).read_bytes()).hexdigest() == expected
                                     for name, expected in inputs.items())
    (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(stage / 'result.json')}), flush=True)
    return 0 if report['passed'] and report['inputs_unchanged'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--widget-only', action='store_true', help='Only rebuild/run changed component tests after bridge compile passes')
    args = parser.parse_args()
    if args.native:
        return native_main(args.widget_only)
    directory = ROOT / '.cache/WebKit/Source/WebKit/UIProcess/haiku'
    names = ('JavaScriptDialogHaiku.h', 'JavaScriptDialogRequestsHaiku.h', 'WebViewPrivate.h',
             'WebViewStateHaiku.h', 'WebViewContextHaiku.h', 'WebView.cpp',
             'WebPopupMenuProxyHaiku.h', 'NativeSelectPopupHaiku.h')
    files = {name: directory / name for name in names}
    for name in ('WebKitView.h', 'WebKitView.cpp', 'WebKitContext.h'):
        files[name] = directory.parent / 'API/haiku' / name
    files['BitmapPresenterHaiku.h'] = directory.parent.parent / 'Platform/haiku/BitmapPresenterHaiku.h'
    files['APIUIClient.h'] = directory.parent / 'API/APIUIClient.h'
    files['WebPageProxy.cpp'] = directory.parent / 'WebPageProxy.cpp'
    files['WebPageProxyInternals.h'] = directory.parent / 'WebPageProxyInternals.h'
    files.update({'tests/EngineNativeDialogTests.cpp': ROOT / 'tests/EngineNativeDialogTests.cpp',
                  'tools/test-modern-dialogs.py': pathlib.Path(__file__)})
    content = {name: path.read_bytes() for name, path in files.items()}
    content['inputs.json'] = json.dumps({name: hashlib.sha256(data).hexdigest()
                                       for name, data in content.items()}, indent=2).encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o600
            stream.addfile(entry, io.BytesIO(data))

    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)

    stage = remote('mktemp -d /boot/home/summit/modern-dialog-inputs.XXXXXXXX', check=True,
                   text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/modern-dialog-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native staging directory')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    output = ROOT / '.vm' / pathlib.Path(stage).name
    output.mkdir(exist_ok=False)
    print('Native dialog stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as stream:
        native_command = ['python3.10', stage + '/tools/test-modern-dialogs.py', '--native']
        if args.widget_only:
            native_command.append('--widget-only')
        result = remote(shlex.join(native_command),
                        stdout=stream, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    report = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, stdout=subprocess.PIPE)
    if report.returncode == 0:
        (output / 'result.json').write_text(report.stdout)
    print('Host evidence: ' + str(output), flush=True)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
