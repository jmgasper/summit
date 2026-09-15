#!/usr/bin/env python3
"""Run real Haiku popup-window creation, focus, sizing and destruction checks.

No WebKit page or extension executes in this native-window harness.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shlex
import subprocess
import tarfile
ROOT = Path(__file__).resolve().parents[1]
RELATIVE = Path('Source/WebKit/UIProcess/haiku/NativeExtensionPopupHaiku.h')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
def native():
    from native_crash_log import NativeCrashLog
    manifest = json.loads((ROOT / 'inputs.json').read_text())
    for name, entry in manifest.items():
        assert digest(ROOT / name) == entry['sha256'], name
    executable = ROOT / 'run'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-I' + str(ROOT),
               str(ROOT / 'NativeExtensionPopupTests.cpp'), '-lbe', '-o', str(executable)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    report = {'scope': 'actual native window host and child view lifetime; no WebKit page, extension code, browser action, IPC or page rendering',
              'inputs': manifest, 'compile_exit': result.returncode, 'compile_output': result.stdout}
    if not result.returncode:
        report['executable_sha256'] = digest(executable)
        libraries = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
        assert 'libWebKit' not in libraries and 'libWebCore' not in libraries
        report['linked_webkit_or_webcore'] = False
        crash_log = NativeCrashLog()
        try:
            result = subprocess.run([str(executable)], timeout=115, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report.update(runtime_exit=result.returncode, output=result.stdout)
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ''
            report.update(runtime_exit=None, output=output.decode(errors='replace') if isinstance(output, bytes) else output, timed_out=True)
        report['native_crash_log'] = crash_log.finish()
        report['inputs_unchanged'] = all(digest(ROOT / name) == entry['sha256'] for name, entry in manifest.items())
        report['passed'] = (report['runtime_exit'] == 0 and 'EXTENSION_POPUP_RESULT PASS checks=' in report['output']
                            and report['native_crash_log']['passed'] and report['inputs_unchanged'])
        print(report['output'], end='', flush=True)
    else:
        report['passed'] = False
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1
def host(overlay):
    source = (Path(overlay).resolve() if overlay else ROOT / '.cache/WebKit') / RELATIVE
    files = {'NativeExtensionPopupHaiku.h': source, 'NativeExtensionPopupTests.cpp': ROOT / 'tests/NativeExtensionPopupTests.cpp',
             'tools/test-native-extension-popup.py': Path(__file__), 'tools/native_crash_log.py': ROOT / 'tools/native_crash_log.py'}
    content = {name: path.read_bytes() for name, path in files.items()}
    manifest = {name: {'source': str(files[name]), 'sha256': hashlib.sha256(data).hexdigest()} for name, data in content.items()}
    content['inputs.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            entry = tarfile.TarInfo(name); entry.size = len(data); stream.addfile(entry, io.BytesIO(data))
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/native-extension-popup.XXXXXXXX', text=True, capture_output=True, check=True).stdout.strip()
    assert stage.startswith('/boot/home/summit/native-extension-popup.') and stage.rsplit('.', 1)[-1].isalnum()
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    output = ROOT / '.vm' / Path(stage).name; output.mkdir(exist_ok=False)
    print('Native extension popup stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-native-extension-popup.py', '--native']), stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    report = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, capture_output=True)
    if not report.returncode: (output / 'result.json').write_text(report.stdout)
    print('Native popup result: ' + str(output / 'result.json'), flush=True)
    return result.returncode
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--overlay')
    args = parser.parse_args()
    raise SystemExit(native() if args.native else host(args.overlay))
