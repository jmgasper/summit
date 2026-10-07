#!/usr/bin/env python3
"""Local native Web Bluetooth test fixture; no hardware access without a chooser."""
import argparse
import http.server
from pathlib import Path
from urllib.parse import urlparse, parse_qs
PAGE = b'''<!doctype html><meta charset="utf-8"><title>Summit Bluetooth tests</title>
<style>body{font:18px sans-serif;margin:40px;background:#f2f6fc;color:#14233b}button{padding:16px;font-size:18px}pre{white-space:pre-wrap}</style>
<h1>Summit Bluetooth tests</h1><p>Native permissions, GATT operations, and connection lifecycle.</p>
<button id="request">Choose test peripheral</button><pre id="results">Ready</pre><script src="/bluetooth-fixture.js"></script>'''
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == '/bluetooth-fixture.js':
            data = Path(__file__).with_suffix('.js').read_bytes(); kind = 'text/javascript'
        else:
            data = PAGE; kind = 'text/html'
        self.send_response(200)
        self.send_header('Content-Type', kind)
        policy = parse_qs(parsed.query).get('policy', [''])[0]
        if policy in ('deny', 'self', 'all'):
            self.send_header('Permissions-Policy', {'deny':'bluetooth=()', 'self':'bluetooth=(self)', 'all':'bluetooth=*'}[policy])
        self.send_header('Cache-Control','no-store'); self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('--port', type=int, default=8775); args = parser.parse_args()
    http.server.ThreadingHTTPServer(('0.0.0.0', args.port), Handler).serve_forever()
