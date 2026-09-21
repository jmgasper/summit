"""Record real HTTPS prefetch, ping, beacon and navigation requests without filtering."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import hashlib
import json
import secrets
import ssl
import subprocess
import tempfile
import threading


class PrivacyNetworkFixture:
    def __init__(self):
        self.nonce = secrets.token_hex(16)
        self.directory = tempfile.TemporaryDirectory(prefix='summit-privacy-tls-')
        root = Path(self.directory.name)
        self.certificate = root / 'certificate.pem'
        key = root / 'key.pem'
        result = subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
            '-keyout', str(key), '-out', str(self.certificate), '-days', '2',
            '-subj', '/CN=127.0.0.1', '-addext', 'subjectAltName=IP:127.0.0.1'],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode:
            self.directory.cleanup()
            raise RuntimeError('Could not create the owned TLS fixture certificate: ' + result.stdout)
        self.certificate_sha256 = hashlib.sha256(self.certificate.read_bytes()).hexdigest()
        self.records = []
        self.lock = threading.Lock()
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def record(self, body=''):
                with owner.lock:
                    owner.records.append({'method': self.command, 'path': self.path, 'body': body,
                        'headers': {key.lower(): value for key, value in self.headers.items()}})

            def respond(self, body, content_type='text/html; charset=utf-8', status=200):
                self.send_response(status)
                self.send_header('Content-Type', content_type)
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    pass

            def do_GET(self):
                self.record()
                prefix = '/' + owner.nonce + '/'
                for round_number in range(5):
                    suffix = str(round_number)
                    if self.path == prefix + 'source/' + suffix:
                        self.respond(owner.source(round_number).encode())
                        return
                    if self.path == prefix + 'destination/' + suffix:
                        title = 'SUMMIT NETWORK ' + owner.nonce + ' ' + suffix + ' destination'
                        self.respond(('<!doctype html><title>' + title + '</title>').encode())
                        return
                    if self.path == prefix + 'normal/' + suffix + '.js':
                        self.respond(b'window.normalScriptLoaded = true;', 'text/javascript')
                        return
                    for kind in ('cached-link', 'new-link', 'cached-speculation'):
                        if self.path == prefix + 'prefetch/' + suffix + '/' + kind:
                            self.respond(b'<!doctype html><title>Prefetched resource</title>')
                            return
                self.respond(b'Not found', status=404)

            def do_POST(self):
                try:
                    length = int(self.headers.get('Content-Length', '0'))
                except ValueError:
                    length = -1
                if not 0 <= length <= 4096:
                    self.record('invalid body length')
                    self.respond(b'', status=400)
                    return
                self.record(self.rfile.read(length).decode('latin1'))
                self.respond(b'')

        self.server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(self.certificate, key)
        self.server.socket = context.wrap_socket(self.server.socket, server_side=True)
        self.base = 'https://127.0.0.1:' + str(self.server.server_port) + '/' + self.nonce + '/'
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def source(self, round_number):
        prefix = '/' + self.nonce + '/'
        suffix = str(round_number)
        rules = {'prefetch': [{'source': 'document', 'where': {'selector_matches': 'a.speculation-candidate'},
                              'eagerness': 'moderate'}]}
        return '''<!doctype html><meta charset="utf-8"><title>Network fixture starting</title>
<script type="speculationrules">''' + json.dumps(rules) + '''</script>
<a class="speculation-candidate" id="candidate" href="''' + prefix + 'prefetch/' + suffix + '''/cached-speculation">Speculation candidate</a>
<a id="navigate" href="''' + prefix + 'destination/' + suffix + '''" ping="''' + prefix + 'ping/' + suffix + '''">Navigate</a>
<script>
'use strict';
const prefix = ''' + json.dumps(prefix) + ''';
const round = ''' + suffix + ''';
const nonce = ''' + json.dumps(self.nonce) + ''';
const cached = document.createElement('link');
cached.rel = 'prefetch';
document.head.appendChild(cached);
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
window.runNetworkRound = async () => {
    try {
        cached.href = prefix + 'prefetch/' + round + '/cached-link';
        const fresh = document.createElement('link');
        fresh.rel = 'prefetch';
        fresh.href = prefix + 'prefetch/' + round + '/new-link';
        document.head.appendChild(fresh);
        document.getElementById('candidate').dispatchEvent(new MouseEvent('mousedown', {bubbles:true, button:0}));
        await new Promise((resolve, reject) => {
            const script = document.createElement('script');
            script.onload = resolve;
            script.onerror = () => reject(new Error('ordinary script request failed'));
            script.src = prefix + 'normal/' + round + '.js';
            document.head.appendChild(script);
        });
        if (!window.normalScriptLoaded) throw new Error('ordinary script did not execute');
        if (!navigator.sendBeacon(prefix + 'beacon/' + round, nonce)) throw new Error('beacon was not queued');
        await pause(2000);
        document.getElementById('navigate').click();
    } catch (error) { document.title = 'SUMMIT NETWORK FAIL ' + String(error); }
};
// Let rule parsing and its anchor scan complete before the native host toggles settings.
window.addEventListener('load', async () => {
    await pause(200);
    document.title = 'SUMMIT NETWORK ' + nonce + ' ' + round + ' ready';
});
</script>'''

    def finish(self):
        self.server.shutdown()
        self.thread.join(timeout=5)
        self.server.server_close()
        with self.lock:
            records = list(self.records)
        certificate_unchanged = hashlib.sha256(self.certificate.read_bytes()).hexdigest() == self.certificate_sha256
        checks = []
        expected = set()
        prefix = '/' + self.nonce + '/'
        for round_number in range(5):
            enabled = round_number in (0, 4)
            for kind in ('source', 'destination', 'normal', 'beacon', 'ping', 'cached-link', 'new-link', 'cached-speculation'):
                path = prefix + kind + '/' + str(round_number)
                if kind == 'normal':
                    path += '.js'
                speculative = kind in ('cached-link', 'new-link', 'cached-speculation')
                if speculative:
                    path = prefix + 'prefetch/' + str(round_number) + '/' + kind
                method = 'POST' if kind in ('beacon', 'ping') else 'GET'
                count = int(enabled) if speculative or kind == 'ping' else 1
                if count:
                    expected.add((method, path))
                matching = [row for row in records if row['method'] == method and row['path'] == path]
                passed = len(matching) == count
                if passed and matching:
                    row = matching[0]
                    if kind == 'beacon':
                        passed = row['body'] == self.nonce
                    elif kind == 'ping':
                        passed = (row['body'] == 'PING' and row['headers'].get('content-type') == 'text/ping'
                                  and row['headers'].get('ping-to') == self.base + 'destination/' + str(round_number))
                    elif kind == 'cached-speculation':
                        passed = row['headers'].get('sec-purpose') == 'prefetch'
                checks.append({'round': round_number, 'kind': kind, 'count': len(matching), 'expected_count': count, 'passed': passed})
        unexpected = [row for row in records if (row['method'], row['path']) not in expected
                      and not (row['method'] == 'GET' and row['path'] == '/favicon.ico')]
        self.directory.cleanup()
        return {'scope': 'unfiltered actual HTTPS requests; TLS acceptance is pinned to this fixture certificate by the native client',
                'nonce': self.nonce, 'base_url': self.base, 'certificate_sha256': self.certificate_sha256,
                'certificate_unchanged': certificate_unchanged, 'records': records, 'checks': checks, 'unexpected': unexpected,
                'passed': certificate_unchanged and not self.thread.is_alive() and all(item['passed'] for item in checks) and not unexpected}
