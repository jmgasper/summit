"""Observe real ping, beacon and navigation traffic without filtering requests."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import secrets
import threading


class HyperlinkAuditingFixture:
    def __init__(self):
        self.nonce = secrets.token_hex(16)
        self.records = []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def respond(self, body, status=200):
                self.send_response(status)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def record(self, body=''):
                with owner.lock:
                    owner.records.append({'method': self.command, 'path': self.path, 'body': body,
                                          'headers': {key.lower(): value for key, value in self.headers.items()}})

            def do_GET(self):
                self.record()
                for round_number in range(4):
                    prefix = '/' + owner.nonce + '/'
                    if self.path == prefix + 'source/' + str(round_number):
                        suffix = str(round_number)
                        destination = prefix + 'destination/' + suffix
                        pings = prefix + 'ping/' + suffix + '/a ' + prefix + 'ping/' + suffix + '/b'
                        beacon = json.dumps(prefix + 'beacon/' + suffix)
                        body = f'''<!doctype html><meta charset="utf-8"><title>Hyperlink auditing fixture</title>
<a id="go" href="{destination}" ping="{pings}">Navigate with auditing pings</a>
<script>
if (!navigator.sendBeacon({beacon}, {json.dumps(owner.nonce)}))
    document.title = 'FAIL beacon was not queued';
else
    setTimeout(() => document.getElementById('go').click(), 50);
</script>'''
                        self.respond(body.encode())
                        return
                    if self.path == prefix + 'destination/' + str(round_number):
                        title = 'SUMMIT HYPERLINK ' + owner.nonce + ' ' + str(round_number) + ' ready'
                        self.respond(('<!doctype html><title>' + title + '</title><p>Navigation completed.</p>').encode())
                        return
                self.respond(b'Not found', 404)

            def do_POST(self):
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 <= length <= 4096:
                    self.record('invalid body length')
                    self.respond(b'', 400)
                    return
                self.record(self.rfile.read(length).decode('latin1'))
                self.respond(b'')

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.base = 'http://127.0.0.1:' + str(self.server.server_port) + '/' + self.nonce + '/'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def finish(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        with self.lock:
            records = list(self.records)
        checks = []
        expected = set()
        for round_number in range(4):
            prefix = '/' + self.nonce + '/'
            for kind in ('source', 'destination', 'beacon'):
                path = prefix + kind + '/' + str(round_number)
                method = 'POST' if kind == 'beacon' else 'GET'
                expected.add((method, path))
                matching = [r for r in records if r['method'] == method and r['path'] == path]
                passed = len(matching) == 1 and (kind != 'beacon' or matching[0]['body'] == self.nonce)
                checks.append({'round': round_number, 'kind': kind, 'count': len(matching), 'passed': passed})
            for suffix in ('a', 'b'):
                path = prefix + 'ping/' + str(round_number) + '/' + suffix
                matching = [r for r in records if r['method'] == 'POST' and r['path'] == path]
                enabled = round_number in (0, 2)
                if enabled:
                    expected.add(('POST', path))
                passed = len(matching) == (1 if enabled else 0)
                if enabled and passed:
                    passed = (matching[0]['body'] == 'PING' and matching[0]['headers'].get('content-type') == 'text/ping'
                              and matching[0]['headers'].get('ping-to') == self.base + 'destination/' + str(round_number))
                checks.append({'round': round_number, 'kind': 'ping-' + suffix, 'count': len(matching), 'passed': passed})
        # A favicon request is unrelated to the privacy preference. All other traffic must be accounted for.
        unexpected = [r for r in records if (r['method'], r['path']) not in expected
                      and not (r['method'] == 'GET' and r['path'] == '/favicon.ico')]
        return {'scope': 'unfiltered native HTTP server observations', 'nonce': self.nonce, 'base_url': self.base,
                'records': records, 'checks': checks, 'unexpected': unexpected,
                'passed': all(check['passed'] for check in checks) and not unexpected}
