#!/usr/bin/env python3
"""Run native download lifecycle tests against a frozen browser and dedicated HTTP server."""
import argparse
import hashlib
import http.server
import importlib.util
import io
import json
import os
import pathlib
import secrets
import shlex
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = ('direct', 'collision', 'unsupported', 'cancel', 'truncated', 'crash', 'recovery', 'view-closed')
BROWSER_CASES = ('ui-complete', 'ui-cancel')


class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        route = urllib.parse.urlsplit(self.path)
        values = urllib.parse.parse_qs(route.query)
        if self.server.browser and route.path == '/native-close.html' and values.get('run') == [self.server.token]:
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(self.server.fixture)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(self.server.fixture)
            return
        name = values.get('case', [''])[0]
        if route.path != '/' + self.server.token + '/download' or name not in (BROWSER_CASES if self.server.browser else CASES):
            self.send_error(404)
            return
        with self.server.report_lock:
            self.server.requests.append(name)
        payload = ('summit download ' + name + '\n').encode()
        slow = name in ('cancel', 'crash', 'ui-cancel')
        blocks = 32768 if name == 'ui-cancel' else 1024
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream' if name == 'unsupported' else 'text/plain')
        if name != 'unsupported':
            filename = ('summit-test-' + self.server.token + '-' + name + '.txt') if self.server.browser else (
                '../../sample.txt' if name == 'direct' else 'sample.txt' if name == 'collision' else name + '.txt')
            self.send_header('Content-Disposition', 'attachment; filename="' + filename + '"')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(1024 * blocks if slow else len(payload) + (4096 if name == 'truncated' else 0)))
        self.end_headers()
        try:
            if name == 'view-closed':
                time.sleep(0.4)
            if slow:
                for _ in range(blocks):
                    self.wfile.write(b'x' * 1024)
                    self.wfile.flush()
                    time.sleep(0.01)
            else:
                self.wfile.write(payload)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        self.close_connection = True

    def do_POST(self):
        route = urllib.parse.urlsplit(self.path)
        if not self.server.browser or route.path != '/close-report' or urllib.parse.parse_qs(route.query).get('run') != [self.server.token]:
            self.send_error(404)
            return
        try:
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 16384:
                raise ValueError('Invalid report length')
            data = json.loads(self.rfile.read(size))
            if (not isinstance(data, dict) or data.get('run') != self.server.token or data.get('name') != 'A'
                    or data.get('event') not in ('ready', 'arm', 'input', 'beforeunload', 'download')):
                raise ValueError('Invalid fixture report')
            with self.server.report_lock:
                self.server.close_reports.append(data)
            self.send_response(200)
            self.send_header('Content-Length', '2')
            self.end_headers()
            self.wfile.write(b'{}')
        except (ValueError, TypeError):
            self.send_error(400)


def native(args):
    from native_crash_log import NativeCrashLog
    if args.browser:
        return subprocess.run(['python3.10', str(ROOT / 'tools/test-modern-close-native.py'),
                               '--downloads', '--bundle', args.bundle, '--base-url', args.url,
                               '--run-token', args.run_token]).returncode
    spec = importlib.util.spec_from_file_location('bundle_verification', ROOT / 'tools/test-modern-contexts-native.py')
    verifier = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verifier)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if verifier.digest(ROOT / name) != expected:
            raise RuntimeError('Changed native input: ' + name)
    bundle, include, manifest, before = verifier.verified_bundle(args.bundle)
    target = ROOT / 'ModernDownloadTests'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Werror', '-DBUILDING_HAIKU__=1',
               '-I' + str(include), str(ROOT / 'tests/ModernDownloadTests.cpp'),
               '-L' + str(bundle / 'lib'), '-lWebKit', '-lbe', '-lnetwork',
               '-Wl,-rpath,' + str(bundle / 'lib'), '-o', str(target)]
    result = subprocess.run(command, capture_output=True, text=True)
    report = {'kind': 'modern-native-downloads', 'inputs': inputs, 'stage': str(ROOT),
              'bundle': str(bundle), 'engine': manifest['inputs']['engine'],
              'compile_command': command, 'compile_exit': result.returncode,
              'diagnostics': result.stdout + result.stderr, 'passed': False}
    print(report['diagnostics'], end='', flush=True)
    if result.returncode == 0:
        directory = ROOT / 'downloads'
        directory.mkdir(mode=0o700)
        environment = dict(os.environ)
        for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
            environment.pop(name, None)
        environment.update(WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
        runtime = [str(target), args.url, str(directory), str(bundle)]
        report.update(run_command=runtime, executable_sha256=verifier.digest(target))
        crash_log = NativeCrashLog()
        try:
            result = subprocess.run(runtime, env=environment, capture_output=True, text=True, timeout=300)
            report.update(runtime_exit=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired as error:
            def decoded(value):
                return value.decode(errors='replace') if isinstance(value, bytes) else value or ''
            report.update(runtime_exit=None, timeout=True, stdout=decoded(error.stdout), stderr=decoded(error.stderr))
        print(report['stdout'] + report['stderr'], end='', flush=True)
        (ROOT / 'runtime.log').write_text(report['stdout'] + report['stderr'])
        report['native_crash_log'] = crash_log.finish()
        report['download_files'] = {str(p.relative_to(directory)): {'sha256': verifier.digest(p), 'size': p.stat().st_size}
                                    for p in directory.rglob('*') if p.is_file()}
        verifier.verify_symlinks(bundle, manifest)
        report['bundle_unchanged'] = all(verifier.digest(pathlib.Path(p)) == expected for p, expected in before.items())
        report['passed'] = (report['runtime_exit'] == 0 and report['bundle_unchanged']
                            and report['native_crash_log']['passed']
                            and 'DOWNLOAD_RESULT PASS steps=8 ' in report['stdout'])
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    token = secrets.token_hex(12)
    output = ROOT / '.vm' / ('modern-downloads-' + token)
    output.mkdir(parents=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Fixture)
    server.daemon_threads = True
    server.browser = args.browser
    server.token, server.requests, server.report_lock = token, [], threading.Lock()
    server.fixture = (ROOT / 'tests/fixtures/native-close.html').read_bytes() if args.browser else b''
    if args.browser:
        server.fixture += ('<a id="fixture-download" style="position:absolute;left:20px;top:320px;width:240px;height:48px;display:block" '
                           'download="suggested.txt" href="/' + token + '/download?case=ui-complete">Download fixture</a>'
                           '<script>document.getElementById("fixture-download").addEventListener("click", '
                           'event => report("download", event.isTrusted));</script>').encode()
    server.close_reports = []
    url = 'http://10.0.2.2:' + str(server.server_port) + '/' + token + '/download'
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    print(json.dumps({'run': token, 'url': url, 'output': str(output)}), flush=True)
    try:
        stage = remote('mktemp -d /boot/home/summit/modern-download-inputs.XXXXXXXX',
                       check=True, capture_output=True, text=True).stdout.strip()
        if not stage.startswith('/boot/home/summit/modern-download-inputs.'):
            raise RuntimeError('Unexpected native staging directory')
        paths = ['tests/ModernDownloadTests.cpp', 'tools/test-modern-downloads.py',
                 'tools/test-modern-contexts-native.py', 'tools/native_crash_log.py']
        if args.browser:
            paths += ['tests/ModernCloseTests.cpp', 'tests/ModernBrowserDownloadTests.cpp',
                      'tests/fixtures/native-close.html', 'tools/test-modern-close-native.py']
        files = {name: (ROOT / name).read_bytes() for name in paths}
        inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        files['inputs.json'] = (json.dumps({'sha256': inputs} if args.browser else inputs, indent=2) + '\n').encode()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in files.items():
                item = tarfile.TarInfo(name)
                item.size = len(data)
                stream.addfile(item, io.BytesIO(data))
                destination = output / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
        (output / 'stage.json').write_text(json.dumps({'stage': stage, 'bundle': args.bundle, 'url': url}, indent=2) + '\n')
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
        command = ['python3.10', stage + '/tools/test-modern-downloads.py', '--native',
                   '--bundle', args.bundle, '--url', url]
        if args.browser:
            command += ['--browser', '--run-token', token]
        with (output / 'native.log').open('w') as log:
            result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
        print((output / 'native.log').read_text(), end='', flush=True)
        fetched = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True)
        native_result = json.loads(fetched.stdout) if fetched.returncode == 0 else {'error': 'Native result missing'}
        with server.report_lock:
            requests = list(server.requests)
            close_reports = list(server.close_reports)
        close_verified = (not args.browser or (
            any(x.get('event') == 'download' and x.get('trusted') is True for x in close_reports)
            and any(x.get('event') == 'arm' and x.get('trusted') is True for x in close_reports)
            and any(x.get('event') == 'input' and x.get('value') == 'X' and x.get('trusted') is True for x in close_reports)
            and {x.get('attempts') for x in close_reports if x.get('event') == 'beforeunload'} == {1, 2}))
        report = {'run': token, 'native': native_result, 'requests': requests,
                  'close_reports': close_reports, 'close_fixture_verified': close_verified,
                  'served_fixture_sha256': hashlib.sha256(server.fixture).hexdigest() if args.browser else None,
                  'passed': result.returncode == 0 and native_result.get('passed') is True and close_verified
                  and requests == list(BROWSER_CASES if args.browser else CASES)}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json')}), flush=True)
        return 0 if report['passed'] else 1
    finally:
        server.shutdown()
        worker.join(timeout=5)
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--port', type=int, default=0)
    parser.add_argument('--browser', action='store_true', help='Test real browser downloads and quit UI')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--url', help=argparse.SUPPRESS)
    parser.add_argument('--run-token', help=argparse.SUPPRESS)
    args = parser.parse_args()
    sys.exit(native(args) if args.native else host(args))
