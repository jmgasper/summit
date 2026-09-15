#!/usr/bin/env python3
"""Verify dynamic extension icons through real native input, SDK pixels and desktop captures."""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import signal
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, _, watched = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    if not all(digest(ROOT / name) == expected for name, expected in inputs.items()):
        raise RuntimeError('Staged input changed')
    executable = ROOT / 'ModernExtensionIconTests'
    source = bundle / 'source'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
               '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
               str(ROOT / 'tests/ModernExtensionIconTests.cpp'), '-lbe', '-o', str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    report = {'scope': 'actual native installation and toolbar clicks, dynamic path/ImageData icons, callbacks/errors, size selection, resets, inheritance, navigation and closed-tab rejection, SDK pixels and desktop alpha compositing',
              'bundle': str(bundle), 'inputs': inputs, 'compile_command': command, 'compile_exit': compiled.returncode,
              'compile_output': compiled.stdout + compiled.stderr, 'passed': False}
    def save():
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    print(report['compile_output'], end='', flush=True)
    if compiled.returncode:
        return 1
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(executable), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    report['executable_sha256'] = digest(executable)
    profile = ROOT / 'profile'
    profile.mkdir(mode=0o700)
    environment = dict(os.environ, WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for key in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(key, None)
    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id':
            raise RuntimeError('Cannot enumerate native teams')
        result = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group:
                    result.append(pid)
            except ProcessLookupError:
                pass
        return result
    monitor = NativeCrashLog()
    report['forced_cleanup'] = False
    try:
        with (ROOT / 'browser.log').open('w') as browser_log, (ROOT / 'harness.log').open('w') as harness_log:
            process = subprocess.Popen([str(bundle / 'Summit'), '--profile', str(profile), str(ROOT / 'pages/initial.html')], env=environment,
                stdout=browser_log, stderr=subprocess.STDOUT, start_new_session=True)
            report['pid'] = report['process_group'] = process.pid
            save()
            try:
                command = [str(executable), str(process.pid), str(bundle / 'Summit'), str(ROOT / 'package'), str(ROOT / 'package/expected.json')]
                if args.watch:
                    command.append('watch')
                report['harness_command'] = command
                result = subprocess.run(command, stdout=harness_log, stderr=subprocess.STDOUT, timeout=300)
                report['harness_exit'] = result.returncode
                report['browser_exit'] = process.wait(timeout=15)
            finally:
                if process.poll() is None:
                    report['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                report['browser_exit'] = process.returncode
                deadline = time.monotonic() + 10
                while members(process.pid) and time.monotonic() < deadline:
                    time.sleep(.05)
                report['remaining_processes'] = members(process.pid)
                report['group_drained'] = not report['remaining_processes']
                if report['remaining_processes']:
                    report['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGKILL)
        report['output'] = (ROOT / 'harness.log').read_text()
        print(report['output'], end='', flush=True)
        report['passed'] = (report.get('harness_exit') == 0 and report.get('browser_exit') == 0
            and report['group_drained'] and not report['forced_cleanup'] and 'EXTENSION_ICONS_RESULT PASS cases=49 observations=55 checks=' in report['output'])
    except Exception as error:
        report['error'] = str(error)
    finally:
        report['output'] = (ROOT / 'harness.log').read_text() if (ROOT / 'harness.log').exists() else ''
        report['browser_output'] = (ROOT / 'browser.log').read_text() if (ROOT / 'browser.log').exists() else ''
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(name)) == expected for name, expected in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = (report['passed'] and report['native_crash_log']['passed']
                            and report['bundle_unchanged'] and report['inputs_unchanged'])
        save()
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    names = ('tests/ModernExtensionIconTests.cpp', 'tests/ModernExtensionOverflowTests.cpp',
             'tests/ModernExtensionManagerTests.cpp', 'tests/ModernCloseTests.cpp',
             'tools/test-modern-extension-icons.py', 'tools/test-modern-close-native.py', 'tools/native_crash_log.py', 'tools/vm.py')
    files = {name: (ROOT / name).read_bytes() for name in names}
    original = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    for name, title in (('initial', 'Dynamic extension icons'), ('second', 'Second icon tab'), ('navigated', 'Icon navigation reset')):
        files['pages/' + name + '.html'] = ('<!doctype html><meta charset="utf-8"><title>' + title + '</title>'
            '<style>body{font:20px sans-serif;padding:32px;background:#f2f6f5;color:#203830}</style>'
            '<h1>' + title + '</h1><p>The native harness is checking extension toolbar icons for this tab.</p>').encode()
    fixture = ROOT / 'tests/fixtures/extensions/action-icons'
    for path in sorted(fixture.rglob('*')):
        if not path.is_file():
            continue
        data = path.read_bytes()
        files['package/' + str(path.relative_to(fixture))] = data
        original[str(path.relative_to(ROOT))] = hashlib.sha256(data).hexdigest()
    inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in files.items():
            item = tarfile.TarInfo(name); item.size = len(data); item.mode = 0o600
            stream.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /boot/home/summit/extension-icon-inputs.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/extension-icon-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Invalid native stage')
    output = ROOT / '.vm' / ('modern-extension-icons-' + secrets.token_hex(12))
    output.mkdir()
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'source_sha256': original}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print(json.dumps({'stage': stage, 'output': str(output)}), flush=True)
    command = ['python3.10', stage + '/tools/test-modern-extension-icons.py', '--native', '--bundle', args.bundle]
    if args.watch:
        command.append('--watch')
    # These assertions read desktop pixels, so wake the VM's screen first.
    subprocess.run(['python3', str(ROOT / 'tools/vm.py'), 'key', 'shift'], check=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    data = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True, check=True)
    report = {'native': json.loads(data.stdout), 'sources_unchanged': all(digest(ROOT / name) == expected for name, expected in original.items()),
              'source_sha256': original}
    report['passed'] = result.returncode == 0 and report['native']['passed'] and report['sources_unchanged']
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--watch', action='store_true', help='Keep each verified image visible briefly in VNC')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise SystemExit(native(args) if args.native else host(args))
