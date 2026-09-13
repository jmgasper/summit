#!/usr/bin/env python3
"""Verify native WebKit profile/private contexts with a dedicated real HTTP fixture.

--compile-only stages public headers and compiles one native object without
linking or running WebKit. --bundle must name a completed, frozen native bundle.
The existing fixture server on port 8765 is never changed or restarted.
"""
import argparse
import hashlib
import http.cookies
import http.server
import io
import json
import pathlib
import secrets
import shlex
import subprocess
import sys
import tarfile
import threading
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
STEPS = {
    'a_seed': (None, 'A'), 'a_share': ('A', None),
    'b_seed': (None, 'B'), 'a_after_b': ('A', None),
    'p_seed': (None, 'P'), 'p_share': ('P', None),
    'q_seed': (None, 'Q'), 'p_after_q': ('P', None),
    'a_reopen': ('A', None), 'b_reopen': ('B', None),
    'p_reopen': (None, 'P2'), 'p_reopen_share': ('P2', None),
}
VALUES = {'A', 'B', 'P', 'Q', 'P2'}
OBSERVATIONS = {'domCookie', 'networkCookie', 'httpCookie', 'localStorage', 'indexedDB'}


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, token, report_path):
        super().__init__(('127.0.0.1', port), FixtureHandler)
        self.token = token
        self.report_path = report_path
        self.reports = []
        self.report_lock = threading.Lock()
        self.fixture = (ROOT / 'tests/fixtures/context-isolation.html').read_bytes()


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, data, content_type='application/json', headers=()):
        if not isinstance(data, bytes):
            data = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def route(self):
        route = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(route.query)
        if query.get('run') != [self.server.token]:
            self.send(403, {'error': 'Incorrect fixture run token'})
            return None
        return route.path

    def read_json(self):
        size = int(self.headers.get('Content-Length', '0'))
        if not 0 < size <= 65536:
            raise ValueError('Invalid JSON payload size')
        return json.loads(self.rfile.read(size))

    def do_GET(self):
        route = self.route()
        if route is None:
            return
        if route == '/context-isolation.html':
            self.send(200, self.server.fixture, 'text/html; charset=utf-8')
        elif route == '/context-cookie':
            cookies = http.cookies.SimpleCookie(self.headers.get('Cookie', ''))
            def value(name):
                return cookies[name].value if name in cookies else None
            self.send(200, {'visible': value('summit_context_visible'), 'httpOnly': value('summit_context_http')})
        else:
            self.send(404, {'error': 'Unknown fixture route'})

    def do_POST(self):
        route = self.route()
        if route is None:
            return
        try:
            data = self.read_json()
            if route == '/context-cookie':
                value = data.get('value')
                if value not in VALUES:
                    raise ValueError('Unexpected cookie value')
                self.send(200, {'stored': value}, headers=[('Set-Cookie',
                    'summit_context_http=' + value + '; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax')])
            elif route == '/context-report':
                if data.get('run') != self.server.token or data.get('step') not in STEPS:
                    raise ValueError('Unexpected report identity')
                with self.server.report_lock:
                    self.server.reports.append(data)
                    with self.server.report_path.open('a') as stream:
                        stream.write(json.dumps(data, sort_keys=True) + '\n')
                self.send(200, {'accepted': data['step']})
            else:
                self.send(404, {'error': 'Unknown fixture route'})
        except (ValueError, TypeError, AttributeError) as error:
            self.send(400, {'error': str(error)})


def verify_reports(reports):
    errors = []
    actual_steps = [report.get('step') for report in reports]
    if actual_steps != list(STEPS):
        errors.append('Reports are missing, duplicated, or out of order: ' + repr(actual_steps))
    for report in reports:
        name = report.get('step')
        if name not in STEPS:
            errors.append('Unknown step ' + repr(name))
            continue
        expected, write = STEPS[name]
        required = {'before_' + field for field in OBSERVATIONS} | {'httpOnlyHidden'}
        if write is not None:
            required |= {'after_' + field for field in OBSERVATIONS}
        checks = report.get('checks', {})
        if (report.get('expected'), report.get('write')) != (expected, write):
            errors.append(name + ': fixture parameters did not match the native scenario')
        if set(checks) != required or any(checks.get(key) is not True for key in required):
            errors.append(name + ': failed or incomplete storage assertions: ' + repr(checks))
        if report.get('error'):
            errors.append(name + ': ' + report['error'])
        before = report.get('before', {})
        if set(before) != OBSERVATIONS or any(value != expected for value in before.values()):
            errors.append(name + ': observed initial values do not match the required context')
        if write is not None:
            after = report.get('after', {})
            if set(after) != OBSERVATIONS or any(value != write for value in after.values()):
                errors.append(name + ': committed values were not read back correctly')
    return errors


def remote(command, **kwargs):
    return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)


def stage_inputs():
    paths = {
        'tests/ModernContextTests.cpp': ROOT / 'tests/ModernContextTests.cpp',
        'tools/test-modern-contexts-native.py': ROOT / 'tools/test-modern-contexts-native.py',
    }
    engine = ROOT / '.cache/WebKit/Source/WebKit'
    for name, relative in {
        'WebKitView.h': 'UIProcess/API/haiku/WebKitView.h',
        'WebKitContext.h': 'UIProcess/API/haiku/WebKitContext.h',
        'WKBase.h': 'Shared/API/c/WKBase.h',
        'WKDeclarationSpecifiers.h': 'Shared/API/c/WKDeclarationSpecifiers.h',
        'WKBaseHaiku.h': 'Shared/API/c/haiku/WKBaseHaiku.h',
    }.items():
        paths['include/WebKit/' + name] = engine / relative
    content = {name: path.read_bytes() for name, path in paths.items()}
    manifest = {'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in content.items()},
                'fixture_sha256': hashlib.sha256((ROOT / 'tests/fixtures/context-isolation.html').read_bytes()).hexdigest()}
    content['inputs.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mode = 0o600
            stream.addfile(info, io.BytesIO(data))
    result = remote('mkdir -p /boot/home/summit && mktemp -d /boot/home/summit/modern-context-inputs.XXXXXXXX',
                    check=True, text=True, stdout=subprocess.PIPE)
    stage = result.stdout.strip()
    if not stage.startswith('/boot/home/summit/modern-context-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native staging directory')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--compile-only', action='store_true')
    mode.add_argument('--serve-only', action='store_true')
    mode.add_argument('--bundle', help='Absolute native path to the frozen modern bundle')
    parser.add_argument('--port', type=int, default=8767, help='Dedicated host fixture port; 0 selects an available port')
    parser.add_argument('--run-token')
    args = parser.parse_args()
    token = args.run_token or secrets.token_hex(12)
    if not token.isalnum() or not 8 <= len(token) <= 64:
        parser.error('run token must contain 8–64 letters/digits')
    output = ROOT / '.vm' / ('modern-context-' + token)
    output.mkdir(parents=True, exist_ok=False)
    server = None
    thread = None
    try:
        if not args.compile_only:
            server = FixtureServer(args.port, token, output / 'reports.jsonl')
            info = {'run': token, 'base_url': 'http://10.0.2.2:' + str(server.server_port), 'output': str(output)}
            print(json.dumps(info), flush=True)
            if args.serve_only:
                server.serve_forever()
                return 0
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
        stage = stage_inputs()
        command = ['python3.10', stage + '/tools/test-modern-contexts-native.py']
        if args.compile_only:
            command.append('--compile-only')
        else:
            command += ['--bundle', args.bundle, '--base-url', info['base_url'], '--run-token', token]
        print('Native context test stage: ' + stage, flush=True)
        with (output / 'native.log').open('w') as log:
            result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
        print((output / 'native.log').read_text(), end='', flush=True)
        result_read = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, stdout=subprocess.PIPE)
        native_result = json.loads(result_read.stdout) if result_read.returncode == 0 else {'error': 'Native result was not written'}
        errors = []
        if server:
            with server.report_lock:
                errors = verify_reports(list(server.reports))
        served_hash = hashlib.sha256(server.fixture).hexdigest() if server else None
        if server and native_result.get('inputs', {}).get('fixture_sha256') != served_hash:
            errors.append('Served fixture differs from the recorded test input')
        report = {'run': token, 'stage': stage, 'native': native_result, 'fixture_errors': errors,
                  'served_fixture_sha256': served_hash,
                  'passed': result.returncode == 0 and not errors
                  and (args.compile_only or native_result.get('passed') is True)}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json'), 'fixture_errors': errors}), flush=True)
        return 0 if report['passed'] else 1
    finally:
        if server:
            if thread:
                server.shutdown()
                thread.join(timeout=5)
            server.server_close()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
