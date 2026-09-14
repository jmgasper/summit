#!/usr/bin/env python3
"""Exercise modern browser load errors with isolated, deliberately failing HTTP responses."""
import argparse
import hashlib
import http.server
import io
import json
import pathlib
import secrets
import shlex
import subprocess
import tarfile
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
CASES = {'good', 'drop', 'retry', 'reload-failure', 'partial', 'missing', 'slow', 'same', 'final'}


class Fixture(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        route = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(route.query)
        if query.get('run') != [self.server.token]:
            self.send_error(403)
            return
        name = route.path.removeprefix('/')
        if name == 'report':
            case = query.get('case', [''])[0]
            if case not in CASES and case != 'partial-reload':
                self.send_error(400)
                return
            with self.server.lock:
                self.server.reports.append(case)
            self.send_response(200)
            self.send_header('Content-Length', '2')
            self.end_headers()
            self.wfile.write(b'{}')
            return
        if name not in CASES:
            self.send_error(404)
            return
        with self.server.lock:
            self.server.requests.append(name)
            count = self.server.requests.count(name)
        try:
            if name == 'slow':
                time.sleep(5)
                return
            if name == 'drop' or (name == 'retry' and count == 1):
                return
            if name == 'same' and count == 1:
                time.sleep(3)
                return
            partial = name == 'partial' or (name == 'reload-failure' and count > 1)
            title = 'partial-reload' if name == 'reload-failure' and count > 1 else name
            report_url = '/report?run=' + self.server.token + '&case=' + title
            body = ('<!doctype html><meta charset="utf-8"><body><h1>' + title + '</h1>'
                    '<script>fetch(' + json.dumps(report_url) + ').then(r=>{'
                    'if(!r.ok)throw new Error("fixture report");document.title='
                    + json.dumps('ERROR TEST ' + title) + ';});</script>' + ' ' * 2048).encode()
            self.send_response(404 if name == 'missing' else 200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body) + (1024 * 1024 if partial else 0)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            if partial:
                time.sleep(3)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        finally:
            self.close_connection = True


def remote(command, **kwargs):
    return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, help='Absolute native frozen full-browser bundle')
    args = parser.parse_args()
    token = secrets.token_hex(12)
    output = ROOT / '.vm' / ('modern-load-errors-' + token)
    output.mkdir(parents=True, exist_ok=False)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Fixture)
    server.daemon_threads = True
    server.token, server.requests, server.reports, server.lock = token, [], [], threading.Lock()
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base_url = 'http://10.0.2.2:' + str(server.server_port)
    print(json.dumps({'run': token, 'base_url': base_url, 'output': str(output)}), flush=True)
    try:
        names = ('tests/ModernLoadErrorTests.cpp', 'tests/ModernCloseTests.cpp',
                 'tools/test-modern-close-native.py', 'tools/native_crash_log.py',
                 'tools/test-modern-load-errors.py')
        files = {name: (ROOT / name).read_bytes() for name in names}
        inputs = {'sha256': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
        files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in files.items():
                item = tarfile.TarInfo(name)
                item.mode, item.size = 0o600, len(data)
                stream.addfile(item, io.BytesIO(data))
        stage = remote('mktemp -d /boot/home/summit/modern-load-error-inputs.XXXXXXXX',
                       capture_output=True, text=True, check=True).stdout.strip()
        if not stage.startswith('/boot/home/summit/modern-load-error-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native staging path')
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
        command = ['python3.10', stage + '/tools/test-modern-close-native.py', '--load-errors',
                   '--bundle', args.bundle, '--base-url', base_url, '--run-token', token]
        with (output / 'native.log').open('w') as log:
            result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
        print((output / 'native.log').read_text(), end='', flush=True)
        native_read = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True)
        native = json.loads(native_read.stdout) if native_read.returncode == 0 else {'error': 'No native result'}
        errors = []
        with server.lock:
            requests, reports = list(server.requests), list(server.reports)
        if set(requests) != CASES:
            errors.append('Required HTTP cases were not all requested: ' + repr(requests))
        for name, count in {'retry': 2, 'reload-failure': 2, 'same': 2}.items():
            if requests.count(name) != count:
                errors.append(f'{name} did not make exactly {count} HTTP requests')
        expected_reports = {'good', 'retry', 'reload-failure', 'partial-reload', 'partial', 'missing', 'same', 'final'}
        if set(reports) != expected_reports:
            errors.append('JavaScript document reports differ: ' + repr(reports))
        report = {'run': token, 'stage': stage, 'native': native, 'requests': requests,
                  'document_reports': reports, 'fixture_errors': errors,
                  'passed': result.returncode == 0 and native.get('passed') is True and not errors}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json'),
                          'fixture_errors': errors}), flush=True)
        return 0 if report['passed'] else 1
    finally:
        server.shutdown()
        worker.join(timeout=5)
        server.server_close()


if __name__ == '__main__':
    raise SystemExit(main())
