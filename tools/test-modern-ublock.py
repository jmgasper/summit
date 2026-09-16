#!/usr/bin/env python3
"""Test unmodified published uBlock CRX installation, filtering, popup and restart.

The controlled server distinguishes a successfully loaded normal script from
an EasyList-matched script blocked before any request reaches the server.
Extension package bytes are never modified or instrumented.
"""
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
import signal
import struct
import subprocess
import tarfile
import threading
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).name
PHASES = ('setup', 'boot', 'enabled', 'popup-disabled', 'popup-reenabled', 'disabled', 'reenabled', 'restart')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): value.update(block)
    return value.hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    bundle, manifest, watched = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    if not all(digest(ROOT / name) == value for name, value in inputs.items()): raise RuntimeError('Staged inputs changed')
    report = {'scope': __doc__, 'bundle': str(bundle), 'engine': manifest['inputs']['engine'], 'inputs': inputs,
              'runs': [], 'passed': False, 'runtime_verified': False, 'extension_bytes_modified': False,
              'diagnostic_console_requested': args.trace_console}
    def save(): (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    temporary = ROOT / 'tmp'; temporary.mkdir(mode=0o700)
    environment = dict(os.environ, TMPDIR=str(temporary), WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for key in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR', 'SUMMIT_TRACE_EXTENSION_CONSOLE'): environment.pop(key, None)
    if args.trace_console: environment['SUMMIT_TRACE_EXTENSION_CONSOLE'] = '1'
    source = bundle / 'source'; executable = ROOT / 'ModernUBlockTests'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar', '-Wno-unused-function', '-fmax-errors=3',
        '-I' + str(source / 'src'), '-I' + str(source / 'vendor'), str(ROOT / 'tests/ModernUBlockTests.cpp'), '-lbe', '-o', str(executable)]
    result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
    report.update(compile_command=command, compile_exit=result.returncode, compile_output=result.stdout + result.stderr)
    print(report['compile_output'], end='', flush=True); save()
    if result.returncode: return 1
    report['executable_sha256'] = digest(executable)
    if args.compile_only:
        report['bundle_unchanged'] = all(digest(Path(name)) == value for name, value in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == value for name, value in inputs.items())
        report['passed'] = report['bundle_unchanged'] and report['inputs_unchanged']; save()
        return 0 if report['passed'] else 1
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(executable), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    archive = ROOT / 'published-package.crx'
    package = json.loads((ROOT / 'package-lock.json').read_text())
    if digest(archive) != package['sha256'] or archive.stat().st_size != package['size']: raise RuntimeError('Published CRX changed')
    profile = ROOT / 'profile'; profile.mkdir(mode=0o700)
    control = ROOT / 'control'; control.mkdir(mode=0o700)
    token = secrets.token_hex(16)
    html = (ROOT / 'target.html').read_text().replace('__SUMMIT_TOKEN__', json.dumps(token)).encode()
    requests = {}; observations = []; errors = []; guard = threading.Lock()
    def atomic(path, value):
        temporary = path.with_suffix('.tmp'); temporary.write_text(json.dumps(value) + '\n'); temporary.replace(path)
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def respond(self, data, kind):
            self.send_response(200); self.send_header('Content-Type', kind); self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store'); self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError): pass
        def do_GET(self):
            parts = self.path.split('/')
            if len(parts) < 4 or parts[1] != token or parts[3] not in PHASES: self.send_error(404); return
            phase = parts[3]
            if parts[2] == 'target' and len(parts) == 4:
                self.respond(html, 'text/html; charset=utf-8'); return
            leaf = '/'.join(parts[4:])
            if parts[2] != 'resource' or leaf not in ('content.js', 'ads/custom_ads.js'): self.send_error(404); return
            name = 'allowed' if leaf == 'content.js' else 'advert'
            with guard:
                row = requests.setdefault(phase, {'allowed': 0, 'advert': 0}); row[name] += 1
                atomic(control / 'requests.json', requests)
            code = 'globalThis.' + ('summitAllowed' if name == 'allowed' else 'summitAdvert') + ' = ' + json.dumps(token + ':' + phase) + ';'
            self.respond(code.encode(), 'application/javascript')
        def do_POST(self):
            parts = self.path.split('/')
            if len(parts) != 4 or parts[1] != token or parts[2] != 'report' or parts[3] not in PHASES: self.send_error(404); return
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size < 8192: raise ValueError('Invalid report size')
                value = json.loads(self.rfile.read(size)); phase = parts[3]
                if not isinstance(value, dict) or value.get('phase') != phase: raise ValueError('Invalid report phase')
                with guard:
                    if len(observations) >= 50: raise ValueError('Too many observations')
                    observations.append(value); atomic(control / (phase + '.json'), value)
                self.respond(b'{}', 'application/json')
            except (ValueError, TypeError) as error:
                errors.append(str(error)); self.send_error(400)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    target = 'http://127.0.0.1:' + str(server.server_port) + '/' + token + '/target'
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report.update(target_url=target, profile=str(profile), published_package=package, filter_rule=json.loads((ROOT / 'filter-rule.json').read_text()))
    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id': raise RuntimeError('Cannot enumerate native teams')
        values = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group: values.append(pid)
            except ProcessLookupError: pass
        return values
    def run(mode):
        row = {'mode': mode, 'forced_cleanup': False}; report['runs'].append(row)
        with Path('/SummitExtensions/summit/live-progress.log').open('a') as log: log.write('[Summit uBlock tests] Starting ' + mode + '\n')
        command = [str(bundle / 'Summit'), '--profile', str(profile), target + ('/setup' if mode == 'install' else '/boot')]
        row['command'] = command
        with (ROOT / (mode + '-browser.log')).open('w') as log:
            browser = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            row['pid'] = row['process_group'] = browser.pid; save()
            try:
                command = [str(executable), str(browser.pid), str(bundle / 'Summit'), str(archive), str(control), target, str(profile), mode, 'watch' if args.watch else 'run']
                row['harness_command'] = command
                with (ROOT / (mode + '-harness.log')).open('w') as output:
                    result = subprocess.run(command, env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=240)
                row['harness_exit'] = result.returncode
                row['browser_exit'] = browser.wait(timeout=15)
            except Exception as error: row['error'] = str(error)
            finally:
                if browser.poll() is None:
                    row['forced_cleanup'] = True; os.killpg(browser.pid, signal.SIGTERM)
                    try: browser.wait(timeout=5)
                    except subprocess.TimeoutExpired: os.killpg(browser.pid, signal.SIGKILL); browser.wait(timeout=5)
                row['browser_exit'] = browser.returncode
                deadline = time.monotonic() + 10
                while members(browser.pid) and time.monotonic() < deadline: time.sleep(.05)
                row['remaining_processes'] = members(browser.pid); row['group_drained'] = not row['remaining_processes']
                if not row['group_drained']: row['forced_cleanup'] = True; os.killpg(browser.pid, signal.SIGKILL)
        row['output'] = (ROOT / (mode + '-harness.log')).read_text(errors='replace')
        row['browser_output'] = (ROOT / (mode + '-browser.log')).read_text(errors='replace')
        row['passed'] = row.get('harness_exit') == 0 and row['browser_exit'] == 0 and row['group_drained'] and not row['forced_cleanup'] and 'UBLOCK_RESULT PASS mode=' + mode in row['output']
        save(); print(json.dumps({'mode': mode, 'passed': row['passed'], 'error': row.get('error')}), flush=True)
        with Path('/SummitExtensions/summit/live-progress.log').open('a') as log: log.write('[Summit uBlock tests] ' + mode + (' PASS' if row['passed'] else ' FAIL') + '\n')
        if not row['passed']: raise RuntimeError(mode + ' failed')
    monitor = NativeCrashLog()
    try:
        run('install')
        catalog = profile / 'Extensions/catalog.json'; approved_catalog = digest(catalog)
        run('restart')
        report['catalog_unchanged_on_restart'] = digest(catalog) == approved_catalog
        if not report['catalog_unchanged_on_restart']: raise RuntimeError('Startup changed installation catalog')
        report['passed'] = True
    except Exception as error: report['error'] = str(error)
    finally:
        server.shutdown(); server.server_close()
        report.update(observations=observations, requests=requests, server_errors=errors)
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(name)) == value for name, value in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == value for name, value in inputs.items())
        report['retained_crx_unchanged'] = False
        if (control / 'installed.json').exists():
            entry = json.loads((control / 'installed.json').read_text()); report['installed_extension'] = entry
            name = entry['package']
            if Path(name).name != name or not name.startswith('pkg-'): raise RuntimeError('Invalid installed package name')
            retained = profile / 'Extensions/packages' / name / 'package'
            report['retained_crx_unchanged'] = retained.is_file() and digest(retained) == package['sha256']
        report['screenshots'] = {path.name: digest(path) for path in control.glob('*.ppm')}
        report['passed'] = report['passed'] and report['bundle_unchanged'] and report['inputs_unchanged'] and report['retained_crx_unchanged'] and report['native_crash_log']['passed'] and not errors
        report['runtime_verified'] = report['passed'] and not args.trace_console; save()
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs): return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    names = ('tests/ModernUBlockTests.cpp', 'tests/ModernExtensionManagerTests.cpp', 'tests/ModernCloseTests.cpp',
             'tools/' + SCRIPT, 'tools/test-modern-close-native.py', 'tools/native_crash_log.py')
    files = {}; original = {}
    def add(name, path):
        data = path.read_bytes(); files[name] = data; original[str(path.resolve())] = hashlib.sha256(data).hexdigest()
    for name in names: add(name, ROOT / name)
    lock = ROOT / 'compatibility/extensions.lock.json'; original[str(lock)] = digest(lock)
    package = next(p for p in json.loads(lock.read_text())['packages'] if p['name'] == 'uBlock0_1.74.0.chromium.crx')
    archive = ROOT / '.cache/extension-corpus/packages' / package['sha256'] / package['name']
    add('published-package.crx', archive)
    data = files['published-package.crx']
    if len(data) != package['size'] or hashlib.sha256(data).hexdigest() != package['sha256']: raise RuntimeError('Published CRX pin mismatch')
    if data[:8] != b'Cr24\x03\x00\x00\x00': raise RuntimeError('Expected CRX3 package')
    with zipfile.ZipFile(io.BytesIO(data[12 + struct.unpack('<I', data[8:12])[0]:])) as zip:
        name = 'assets/thirdparties/easylist/easylist.txt'; rules = zip.read(name)
        rule = '/ads/custom_ads.js$script'
        if rule not in rules.decode().splitlines(): raise RuntimeError('Selected probe filter is missing from published package')
    files['filter-rule.json'] = json.dumps({'path': name, 'sha256': hashlib.sha256(rules).hexdigest(), 'rule': rule}).encode()
    files['package-lock.json'] = json.dumps(package, indent=2).encode()
    add('target.html', ROOT / 'tests/fixtures/published-ublock.html')
    files['inputs.json'] = json.dumps({name: hashlib.sha256(data).hexdigest() for name, data in files.items()}, indent=2).encode()
    stream = io.BytesIO()
    with tarfile.open(fileobj=stream, mode='w:gz') as tar:
        for name, data in files.items():
            item = tarfile.TarInfo(name); item.size = len(data); item.mode = 0o600; tar.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /SummitExtensions/summit/extension-ublock-browser.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/SummitExtensions/summit/extension-ublock-browser.') or not stage.rsplit('.', 1)[-1].isalnum(): raise RuntimeError('Invalid native stage')
    output = ROOT / '.vm' / ('modern-ublock-' + secrets.token_hex(12)); output.mkdir()
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'sources': original}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=stream.getvalue(), check=True)
    print(json.dumps({'stage': stage, 'output': str(output)}), flush=True)
    command = ['python3.10', stage + '/tools/' + SCRIPT, '--native', '--bundle', args.bundle]
    if args.watch: command.append('--watch')
    if args.compile_only: command.append('--compile-only')
    if args.trace_console: command.append('--trace-console')
    if not args.compile_only: subprocess.run(['python3', str(ROOT / 'tools/vm.py'), 'key', 'shift'], check=True)
    with (output / 'native.log').open('w') as log: result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    fetched = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True, check=True)
    report = {'native': json.loads(fetched.stdout), 'source_sha256': original, 'sources_unchanged': all(digest(Path(name)) == value for name, value in original.items())}
    report['passed'] = result.returncode == 0 and report['native']['passed'] and report['sources_unchanged']
    for name, value in report['native'].get('screenshots', {}).items():
        if Path(name).name != name or not name.endswith('.ppm'): raise RuntimeError('Invalid screenshot path')
        data = remote('cat ' + shlex.quote(stage + '/control/' + name), capture_output=True, check=True).stdout
        if hashlib.sha256(data).hexdigest() != value: raise RuntimeError('Screenshot changed')
        (output / name).write_bytes(data)
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json'), 'error': report['native'].get('error')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--trace-console', action='store_true', help='Enable extension console diagnostics; does not establish a clean runtime pass')
    parser.add_argument('--watch', action='store_true')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise SystemExit(native(args) if args.native else host(args))
