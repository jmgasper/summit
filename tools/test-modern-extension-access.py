#!/usr/bin/env python3
"""Verify extension access and Window APIs across saved-settings browser restarts."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import signal
import subprocess
import tarfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).name


def digest(path):
    with path.open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, _, watched = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    if not all(digest(ROOT / name) == expected for name, expected in inputs.items()):
        raise RuntimeError('Staged input changed')
    executable = ROOT / 'ModernExtensionAccessTests'
    source = bundle / 'source'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
        '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
        str(ROOT / 'tests/ModernExtensionAccessTests.cpp'), '-lbe', '-o', str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    report = {'scope': __doc__, 'settings_scope': 'Initial native consent denies file/private access. Subsequent launches reload changed catalog grants while the browser is stopped; this does not test a post-install permission settings UI.',
        'bundle': str(bundle), 'inputs': inputs, 'compile_command': command, 'compile_exit': compiled.returncode,
        'compile_output': compiled.stdout + compiled.stderr, 'passed': False, 'runtime_verified': False, 'sessions': []}
    def save():
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    print(report['compile_output'], end='', flush=True)
    if compiled.returncode: return 1
    report['executable_sha256'] = digest(executable)
    if args.compile_only:
        report['bundle_unchanged'] = all(digest(Path(name)) == expected for name, expected in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = report['bundle_unchanged'] and report['inputs_unchanged']
        save(); return 0 if report['passed'] else 1
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(executable), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    profile = ROOT / 'profile'; profile.mkdir(mode=0o700)
    controls = ROOT / 'controls'; controls.mkdir(mode=0o700)
    current_control = None
    token = secrets.token_hex(16)
    server_errors = []
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def respond(self, data, content_type='application/json'):
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError): pass
        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')
            self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
            self.end_headers()
        def route(self):
            prefix = '/' + token + '/'
            if not self.path.startswith(prefix): return None
            parts = self.path[len(prefix):].split('/')
            return parts if len(parts) >= 2 and parts[0] in ('a', 'b') and current_control else None
        def do_GET(self):
            if self.path.startswith('/' + token + '/target/'):
                self.respond(b'<!doctype html><meta charset="utf-8"><title>Extension access</title><h1>Native extension access and Window API tests</h1>', 'text/html; charset=utf-8')
                return
            parts = self.route()
            if parts and parts[1:] == ['command']:
                path = current_control / parts[0] / 'command.json'
                self.respond(path.read_bytes() if path.exists() else b'{"id":0}')
            else: self.send_error(404)
        def do_POST(self):
            parts = self.route()
            if not parts: self.send_error(404); return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 131072: raise ValueError('Invalid report size')
                value = json.loads(self.rfile.read(length))
                if not isinstance(value, dict): raise ValueError('Invalid report type')
                directory = current_control / parts[0]
                if parts[1:] == ['ready']: destination = directory / 'ready.json'
                elif len(parts) == 3 and parts[1] == 'result' and parts[2].isdigit():
                    destination = directory / 'responses' / (parts[2] + '.json')
                elif len(parts) == 3 and parts[1] == 'view' and (parts[2] == 'tab' or parts[2].isdigit()):
                    destination = directory / ('view-' + parts[2] + '.json')
                else: raise ValueError('Unexpected report route')
                if destination.exists(): raise ValueError('Duplicate report: ' + str(destination))
                temporary = destination.with_suffix('.tmp')
                temporary.write_text(json.dumps(value) + '\n'); temporary.replace(destination)
                self.respond(b'{}')
            except (ValueError, OSError) as error:
                server_errors.append(str(error)); self.send_error(400)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    control_url = 'http://127.0.0.1:' + str(server.server_port) + '/' + token
    target_url = control_url + '/target'
    packages = ROOT / 'runtime-packages'; packages.mkdir()
    package_inputs = {}
    for identity in ('a', 'b'):
        package = packages / identity; shutil.copytree(ROOT / 'package', package)
        manifest = package / 'manifest.json'
        manifest.write_text(manifest.read_text().replace('__EXTENSION_ID__', 'summit-extension-access-' + identity))
        config = package / 'config.js'
        if config.read_text().count('__CONTROL_JSON__') != 1: raise RuntimeError('Invalid control placeholder')
        config.write_text(config.read_text().replace('__CONTROL_JSON__', json.dumps(control_url + '/' + identity)))
        package_inputs[identity] = {str(p.relative_to(package)): digest(p) for p in package.rglob('*') if p.is_file()}
    report.update(control_url=control_url, target_url=target_url, runtime_package_sha256=package_inputs)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    environment = dict(os.environ, WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for key in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR', 'SUMMIT_TRACE_EXTENSION_CONSOLE'): environment.pop(key, None)
    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id': raise RuntimeError('Cannot enumerate native teams')
        result = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group: result.append(pid)
            except ProcessLookupError: pass
        return result
    def settings(a_files=False, a_private=False, b_files=False, b_private=False):
        return {'a': {'files': a_files, 'private': a_private}, 'b': {'files': b_files, 'private': b_private}}
    phases = [('initial', settings()), ('granted', settings(a_files=True, b_private=True)),
        ('swapped', settings(a_private=True, b_files=True)), ('revoked', settings())]
    catalog_path = profile / 'Extensions/catalog.json'
    original_catalog = None
    monitor = NativeCrashLog()
    try:
        previous_startups = {}
        for phase, expected in phases:
            current_control = controls / phase; current_control.mkdir()
            for identity in ('a', 'b'): (current_control / identity / 'responses').mkdir(parents=True)
            (current_control / 'expected.json').write_text(json.dumps(expected) + '\n')
            if phase != 'initial':
                catalog = json.loads(catalog_path.read_text())
                assert len(catalog['extensions']) == 2
                for entry in catalog['extensions']:
                    identity = entry['identifier'].removeprefix('summit-extension-access-')
                    assert identity in expected
                    entry['allow_file_urls'] = expected[identity]['files']
                    entry['allow_private_browsing'] = expected[identity]['private']
                temporary = catalog_path.with_suffix('.tmp')
                temporary.write_text(json.dumps(catalog, indent=2) + '\n'); temporary.replace(catalog_path)
            session = {'phase': phase, 'expected': expected, 'forced_cleanup': False}
            report['sessions'].append(session); save()
            with (current_control / 'browser.log').open('w') as browser_log, (current_control / 'harness.log').open('w') as harness_log:
                process = subprocess.Popen([str(bundle / 'Summit'), '--profile', str(profile), target_url + '/setup'], env=environment,
                    stdout=browser_log, stderr=subprocess.STDOUT, start_new_session=True)
                session['pid'] = session['process_group'] = process.pid; save()
                try:
                    command = [str(executable), str(process.pid), str(bundle / 'Summit'), str(packages), str(current_control), target_url, str(profile), phase]
                    session['harness_command'] = command
                    result = subprocess.run(command, stdout=harness_log, stderr=subprocess.STDOUT, timeout=240)
                    session['harness_exit'] = result.returncode
                    session['browser_exit'] = process.wait(timeout=15)
                finally:
                    if process.poll() is None:
                        session['forced_cleanup'] = True; os.killpg(process.pid, signal.SIGTERM)
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired: os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=5)
                    session['browser_exit'] = process.returncode
                    deadline = time.monotonic() + 10
                    while members(process.pid) and time.monotonic() < deadline: time.sleep(.05)
                    session['remaining_processes'] = members(process.pid)
                    session['group_drained'] = not session['remaining_processes']
                    if session['remaining_processes']:
                        session['forced_cleanup'] = True; os.killpg(process.pid, signal.SIGKILL)
                    session['output'] = (current_control / 'harness.log').read_text()
                    session['browser_output'] = (current_control / 'browser.log').read_text()
                    save()
            print(session['output'], end='', flush=True)
            session['passed'] = session.get('harness_exit') == 0 and session['browser_exit'] == 0 and session['group_drained'] and not session['forced_cleanup'] and 'EXTENSION_ACCESS_RESULT PASS phase=' in session['output']
            save()
            if not session['passed']: raise RuntimeError('Runtime phase failed: ' + phase)
            startups = {identity: json.loads((current_control / identity / 'ready.json').read_text())['startup'] for identity in ('a', 'b')}
            assert all(startups[identity] != previous_startups.get(identity) for identity in startups), 'Restart reused background instance'
            assert startups['a'] != startups['b'], 'Extension backgrounds share startup identity'
            session['startups'] = startups; previous_startups = startups
            session['api_results'] = {identity: {p.name: json.loads(p.read_text()) for p in (current_control / identity).rglob('*.json') if p.name != 'command.json'} for identity in ('a', 'b')}
            catalog = json.loads(catalog_path.read_text())
            stable = json.loads(json.dumps(catalog))
            for entry in stable['extensions']:
                identity = entry['identifier'].removeprefix('summit-extension-access-')
                assert entry['allow_file_urls'] == expected[identity]['files'] and entry['allow_private_browsing'] == expected[identity]['private']
                entry.pop('allow_file_urls'); entry.pop('allow_private_browsing')
            if original_catalog is None: original_catalog = stable
            assert stable == original_catalog, 'Catalog identity changed across restart'
            session['catalog_identity_unchanged'] = True; save()
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        server.shutdown(); server.server_close()
        report['server_errors'] = server_errors
        report['runtime_package_unchanged'] = all({str(p.relative_to(packages / identity)): digest(p) for p in (packages / identity).rglob('*') if p.is_file()} == expected for identity, expected in package_inputs.items())
        report['installed_package_unchanged'] = False
        try:
            catalog = json.loads(catalog_path.read_text())
            assert len(catalog['extensions']) == 2
            for entry in catalog['extensions']:
                identity = entry['identifier'].removeprefix('summit-extension-access-')
                name = entry['package']
                if not isinstance(name, str) or Path(name).name != name or not name.startswith('pkg-'): raise RuntimeError('Invalid installed package path')
                package = profile / 'Extensions/packages' / name / 'package'
                assert {str(p.relative_to(package)): digest(p) for p in package.rglob('*') if p.is_file()} == package_inputs[identity]
            report['installed_package_unchanged'] = True
        except (OSError, ValueError, KeyError, TypeError, RuntimeError, AssertionError) as error:
            report['package_validation_error'] = str(error)
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(name)) == expected for name, expected in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = report['passed'] and report['native_crash_log']['passed'] and not server_errors and report['bundle_unchanged'] and report['inputs_unchanged'] and report['runtime_package_unchanged'] and report['installed_package_unchanged']
        report['runtime_verified'] = report['passed']; save()
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    names = ('tests/ModernExtensionAccessTests.cpp', 'tests/ModernExtensionOverflowTests.cpp',
             'tests/ModernExtensionManagerTests.cpp', 'tests/ModernCloseTests.cpp',
             'tools/' + SCRIPT, 'tools/test-modern-close-native.py', 'tools/native_crash_log.py', 'tools/vm.py')
    files = {name: (ROOT / name).read_bytes() for name in names}
    original = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    for name, target in (('extension-access', 'package'),):
        fixture = ROOT / 'tests/fixtures/extensions' / name
        for path in sorted(fixture.rglob('*')):
            if path.is_file():
                data = path.read_bytes()
                files[target + '/' + str(path.relative_to(fixture))] = data
                original[str(path.relative_to(ROOT))] = hashlib.sha256(data).hexdigest()
    inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in files.items():
            item = tarfile.TarInfo(name); item.size = len(data); item.mode = 0o600
            stream.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /boot/home/summit/extension-access-browser.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/extension-access-browser.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Invalid native stage')
    output = ROOT / '.vm' / ('modern-extension-extension-access-' + secrets.token_hex(12))
    output.mkdir()
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'source_sha256': original}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print(json.dumps({'stage': stage, 'output': str(output)}), flush=True)
    command = ['python3.10', stage + '/tools/' + SCRIPT, '--native', '--bundle', args.bundle]
    if args.compile_only:
        command.append('--compile-only')
    else:
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
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise SystemExit(native(args) if args.native else host(args))
