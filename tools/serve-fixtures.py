#!/usr/bin/env python3
"""Real HTTP/DOM/JS/CSS fixtures for Summit in QEMU (guest host = 10.0.2.2)."""
import argparse
import http.server
import json
import pathlib
import re
import struct
import time
import urllib.parse
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]


def pixel_png():
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b'\x00\x1e\x90\xff\xff')) + chunk(b'IEND', b''))


PAGE = b'''<!doctype html><meta charset="utf-8"><title>Fixture loading</title>
<link rel="stylesheet" href="/fixture.css">
<h1>Summit engine check</h1><p id="result">Running JavaScript checks...</p>
<div id="grid"><span>Native browser</span><span>Real WebKit</span></div>
<p><a href="/second">Next page</a> &nbsp; <a target="_blank" href="/second">Open in new tab</a></p>
<p><a href="/download">Download test file</a></p><input id="editable" placeholder="Type here">
<script src="/fixture.js"></script>'''
SCRIPT = b'''(async () => {
 const results = {};
 results.promise = await Promise.resolve(42) === 42;
 results.dom = document.querySelectorAll('#grid span').length === 2;
 results.css = getComputedStyle(document.querySelector('#grid')).display === 'grid';
 localStorage.setItem('summit-fixture', 'stored');
 results.storage = localStorage.getItem('summit-fixture') === 'stored';
 document.cookie = 'summit_visible=yes; Path=/';
 results.cookie = document.cookie.includes('summit_visible=yes');
 results.httpOnly = !document.cookie.includes('summit_http=');
 const response = await fetch('/echo-cookie');
 const cookies = await response.text();
 results.fetch = response.ok;
 results.networkCookie = cookies.includes('summit_http=yes') && cookies.includes('summit_visible=yes');
 const passed = Object.values(results).every(Boolean);
 const report = await fetch('/report', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(results)});
 if (!report.ok) throw new Error('Fixture report failed: ' + report.status);
 // The native harness navigates as soon as this title appears. Finish all
 // requests first so back/forward cannot abort the report and overwrite PASS.
 document.querySelector('#result').textContent = JSON.stringify(results, null, 2);
 document.title = passed ? 'Summit fixture PASS' : 'Summit fixture FAIL';
})().catch(error => { document.title = 'Summit fixture ERROR'; document.querySelector('#result').textContent=String(error); });'''


class Handler(http.server.BaseHTTPRequestHandler):
    def send(self, data, content_type='text/html; charset=utf-8', headers=()):
        self.send_response(200)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path).path
        if path in ('/', '/basic'):
            self.send(PAGE, headers=[('Set-Cookie', 'summit_http=yes; HttpOnly; Path=/')])
        elif path == '/fixture.js':
            self.send(SCRIPT, 'application/javascript')
        elif path == '/fixture.css':
            self.send(b'body{font:18px system-ui;margin:48px;background:#f7faf6;color:#234c3b}#grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}#grid span{padding:24px;background:#e0eddb}a{color:#246548}#result{white-space:pre-wrap}', 'text/css')
        elif path == '/platform':
            self.send((ROOT / 'tests/fixtures/platform.html').read_bytes())
        elif path == '/platform.js':
            self.send((ROOT / 'tests/fixtures/platform.js').read_bytes(), 'application/javascript')
        elif path == '/module.js':
            self.send(b'export const answer = 42; export function value() { return "module loaded"; }', 'application/javascript')
        elif path == '/worker.js':
            self.send(b'onmessage = event => postMessage(new Uint8Array(event.data).reduce((sum, value) => sum + value, 0));', 'application/javascript')
        elif path == '/pixel.png':
            self.send(pixel_png(), 'image/png')
        elif path == '/square.svg':
            self.send(b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16"><rect width="16" height="16" fill="#1e90ff"/></svg>', 'image/svg+xml')
        elif path == '/echo-cookie':
            self.send(self.headers.get('Cookie', '').encode(), 'text/plain')
        elif path == '/second':
            self.send(b'<!doctype html><title>Summit second page</title><h1>Second page</h1><a href="/basic">Back to engine checks</a>')
        elif path == '/slow':
            self.send(b'''<!doctype html><title>Loading delayed fixture</title><h1>Delayed image</h1>
<script>document.addEventListener('DOMContentLoaded', () => { document.title = 'Summit pending image'; });
window.addEventListener('load', () => { document.title = 'Summit completed image'; });</script>
<img src="/slow-image" alt="delayed fixture">''')
        elif path == '/slow-image':
            time.sleep(3)
            self.send(bytes.fromhex('47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b'), 'image/gif', [('Cache-Control', 'no-store')])
        elif path == '/download':
            query = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
            token = query.get('token', [''])[0]
            if token and not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', token):
                return self.send_error(400)
            filename = 'summit-test' + ('-' + token if token else '') + '.txt'
            self.send(b'Summit download fixture\n', 'application/octet-stream', [('Content-Disposition', f'attachment; filename="{filename}"')])
        else:
            self.send_error(404)

    def do_POST(self):
        if self.path != '/report':
            return self.send_error(404)
        length = int(self.headers.get('Content-Length', 0))
        if length > 4096:
            return self.send_error(413)
        report = json.loads(self.rfile.read(length))
        with (ROOT / '.vm/engine-reports.jsonl').open('a') as file:
            file.write(json.dumps(report) + '\n')
        self.send(b'OK', 'text/plain')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int, default=8765)
    args = parser.parse_args()
    http.server.ThreadingHTTPServer(('127.0.0.1', args.port), Handler).serve_forever()
