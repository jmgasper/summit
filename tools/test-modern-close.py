#!/usr/bin/env python3
"""Test real modern Summit close behavior using a dedicated local HTTP server.

Compile an isolated harness object now with --compile-only. Runtime requires
--bundle ABSOLUTE_GUEST_FULL_BROWSER_BUNDLE. The running legacy Summit and the
fixture servers on 8765/8767 are never targeted or restarted.
"""
import argparse
import hashlib
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


class FixtureServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port, token, report_path):
        super().__init__(('127.0.0.1', port), FixtureHandler)
        self.token = token
        self.report_path = report_path
        self.fixture = (ROOT / 'tests/fixtures/native-close.html').read_bytes()
        self.reports = []
        self.report_lock = threading.Lock()


class FixtureHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send(self, status, content, content_type='application/json'):
        if not isinstance(content, bytes):
            content = json.dumps(content).encode()
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(content)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(content)

    def route(self):
        route = urllib.parse.urlsplit(self.path)
        if urllib.parse.parse_qs(route.query).get('run') != [self.server.token]:
            self.send(403, {'error': 'Incorrect run token'})
            return None
        return route.path

    def do_GET(self):
        route = self.route()
        if route is None:
            return
        if route == '/native-close.html':
            self.send(200, self.server.fixture, 'text/html; charset=utf-8')
        else:
            self.send(404, {'error': 'Unknown fixture route'})

    def do_POST(self):
        route = self.route()
        if route is None:
            return
        try:
            if route != '/close-report':
                self.send(404, {'error': 'Unknown fixture route'})
                return
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= 16384:
                raise ValueError('Invalid report size')
            data = json.loads(self.rfile.read(size))
            if (not isinstance(data, dict) or data.get('run') != self.server.token
                    or data.get('name') not in {'A', 'B', 'C', 'D'}
                    or data.get('event') not in {'ready', 'arm', 'input', 'beforeunload'}):
                raise ValueError('Unexpected fixture report')
            with self.server.report_lock:
                self.server.reports.append(data)
                with self.server.report_path.open('a') as stream:
                    stream.write(json.dumps(data, sort_keys=True) + '\n')
            self.send(200, {'accepted': True})
        except (ValueError, TypeError, AttributeError) as error:
            self.send(400, {'error': str(error)})


def verify_reports(reports):
    errors = []
    for name in ('A', 'B', 'C', 'D'):
        if not any(item.get('name') == name and item.get('event') == 'ready' for item in reports):
            errors.append('Missing actual HTTP/JavaScript ready report for ' + name)
    for name in ('A', 'B'):
        if not any(item.get('name') == name and item.get('event') == 'arm'
                   and item.get('trusted') is True and item.get('armed') is True for item in reports):
            errors.append('Missing trusted native activation for ' + name)
    for value in ('X', 'seed'):
        if not any(item.get('name') == 'A' and item.get('event') == 'input'
                   and item.get('value') == value and item.get('trusted') is True for item in reports):
            errors.append('Missing actual edit/undo report for A value ' + value)
    for name, count in (('A', 2), ('B', 3)):
        attempts = {item.get('attempts') for item in reports if item.get('name') == name
                    and item.get('event') == 'beforeunload'}
        if not set(range(1, count + 1)).issubset(attempts):
            errors.append('Missing repeated beforeunload reports for ' + name)
    return errors


def remote(command, **kwargs):
    return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)


def stage_inputs():
    names = ('tests/ModernCloseTests.cpp', 'tools/test-modern-close-native.py', 'tools/native_crash_log.py',
             'src/ui/Messages.h', 'vendor/nlohmann/json.hpp')
    content = {name: (ROOT / name).read_bytes() for name in names}
    inputs = {'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in content.items()},
              'fixture_sha256': hashlib.sha256((ROOT / 'tests/fixtures/native-close.html').read_bytes()).hexdigest()}
    content['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o600
            stream.addfile(entry, io.BytesIO(data))
    stage = remote('mktemp -d /boot/home/summit/modern-close-inputs.XXXXXXXX',
                   check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/modern-close-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native staging directory')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    return stage


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--compile-only', action='store_true')
    mode.add_argument('--bundle', help='Absolute native frozen full-browser bundle path')
    parser.add_argument('--port', type=int, default=8769, help='Dedicated host port; 0 selects an available port')
    args = parser.parse_args()
    if args.port in (8765, 8767):
        parser.error('Use a dedicated close-test fixture port, not 8765 or 8767')
    token = secrets.token_hex(12)
    output = ROOT / '.vm' / ('modern-close-' + token)
    output.mkdir(parents=True, exist_ok=False)
    server = None
    thread = None
    try:
        if args.bundle:
            server = FixtureServer(args.port, token, output / 'reports.jsonl')
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
        stage = stage_inputs()
        command = ['python3.10', stage + '/tools/test-modern-close-native.py']
        if args.compile_only:
            command.append('--compile-only')
        else:
            command += ['--bundle', args.bundle, '--run-token', token,
                        '--base-url', 'http://10.0.2.2:' + str(server.server_port)]
        print('Native close stage: ' + stage, flush=True)
        with (output / 'native.log').open('w') as stream:
            result = remote(shlex.join(command), stdout=stream, stderr=subprocess.STDOUT)
        print((output / 'native.log').read_text(), end='', flush=True)
        saved = remote('cat ' + shlex.quote(stage + '/result.json'), text=True, stdout=subprocess.PIPE)
        native = json.loads(saved.stdout) if saved.returncode == 0 else {'error': 'No native result'}
        errors = []
        if server:
            with server.report_lock:
                errors = verify_reports(list(server.reports))
            if native.get('inputs', {}).get('fixture_sha256') != hashlib.sha256(server.fixture).hexdigest():
                errors.append('Served fixture differs from the recorded input')
        report = {'native': native, 'fixture_errors': errors, 'run': token,
                  'passed': result.returncode == 0 and native.get('passed') is True and not errors}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json')}), flush=True)
        return 0 if report['passed'] else 1
    finally:
        if server:
            server.shutdown()
            thread.join(timeout=5)
            server.server_close()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
