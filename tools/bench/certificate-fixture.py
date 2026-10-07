#!/usr/bin/env python3
"""HTTP/TLS fixture for ModernCertificateInfoTests (use a disposable profile)."""
import argparse
import http.server
import pathlib
import ssl
import subprocess
import threading

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', required=True, help='IP address reachable from the browser')
parser.add_argument('--bind', default='0.0.0.0')
parser.add_argument('--http-port', type=int, default=8768)
parser.add_argument('--https-port', type=int, default=8769)
parser.add_argument('--directory', type=pathlib.Path, required=True)
args = parser.parse_args()
# The address is interpolated into an OpenSSL extension and redirect URL.
import ipaddress
host = str(ipaddress.IPv4Address(args.host))
url_host = host
plain_url = f'http://{url_host}:{args.http_port}'
secure_url = f'https://{url_host}:{args.https_port}'
args.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
certificate, key = args.directory / 'cert.pem', args.directory / 'key.pem'
if not certificate.exists() or not key.exists():
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
        '-keyout', str(key), '-out', str(certificate), '-days', '2',
        '-subj', '/CN=Summit certificate fixture/O=Summit Tests',
        '-addext', f'subjectAltName=IP:{host}'], check=True)
    key.chmod(0o600)


class Handler(http.server.BaseHTTPRequestHandler):
    # HTTP/1.0 closes each connection. Later loads exercise TLS session
    # resumption, whose peer chain may be absent while the leaf is retained.
    def do_GET(self):
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', plain_url + '/plain')
            self.end_headers()
            return
        body = (b'<!doctype html><title>Summit certificate fixture</title>'
                b'<h1>Certificate fixture</h1><p>This page uses the test certificate.</p>')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(body)


plain = http.server.ThreadingHTTPServer((args.bind, args.http_port), Handler)
threading.Thread(target=plain.serve_forever, daemon=True).start()
secure = http.server.ThreadingHTTPServer((args.bind, args.https_port), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certificate, key)
secure.socket = context.wrap_socket(secure.socket, server_side=True)
fingerprint = subprocess.check_output(['openssl', 'x509', '-in', str(certificate),
    '-noout', '-fingerprint', '-sha256'], text=True).strip().split('=')[1].replace(':', '')
print(f'HTTPS base: {secure_url}\nHTTP base: {plain_url}\nSHA-256: {fingerprint}', flush=True)
secure.serve_forever()
