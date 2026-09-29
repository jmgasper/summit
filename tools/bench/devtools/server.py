#!/usr/bin/env python3
"""A small site for trying Summit's developer tools (docs/developer-tools.md).

    python3 tools/bench/devtools/server.py [--port 8790]

index.html asks for every kind of thing the Network tab shows (a document, a
style sheet, a script, an image, JSON, XML and HTML from an API, JSON that
calls itself text, a POST with a body, a redirect, a page that is not there,
a host that does not answer, a slow answer and a large one) and writes every
kind of message the Console tab shows. next.html is somewhere to navigate
to, for the logs that are kept and the logs that are not.
"""
import argparse
import base64
import http.server
import json
import pathlib
import time

ROOT = pathlib.Path(__file__).resolve().parent
PNG = base64.b64decode(
    'iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAAm0lEQVR42u2WSw6AIAxEaeNR9Aq61KPrUq+gd8GViSGgfErp'
    'om9dpsMEKMYoiqIkYq21lHqYs6ifRzITkLt7AIBmCVCnEL17FwqDWGIq1CQlHSw5+ee6fzaLMdLFGBiWqdrZQArBp8ZX+7ce'
    'TWOA66pd2wEiE0CuhyakJzOBWs+sT1deArWHjKsvKwGuEfvuIycB7g8G+4dGCXEDQkxKG9+Wnf8AAAAASUVORK5CYII=')
ITEMS = {'items': [{'id': 1, 'name': 'Kunanyi', 'height': 1271.0, 'tags': ['mountain', 'hobart']},
                   {'id': 12345678901234567890, 'name': 'Summit "quoted"', 'height': 1e3, 'tags': []}],
         'total': 2, 'next': None, 'ok': True}
FEED = ('<?xml version="1.0" encoding="UTF-8"?><rss version="2.0"><channel><title>Summit</title>'
        '<item><title>First &amp; foremost</title><link>http://example.com/1</link><description><![CDATA[<b>bold</b>]]></description></item>'
        '<item><title>Second</title><link>http://example.com/2</link></item><!-- end --></channel></rss>')
FRAGMENT = ('<!DOCTYPE html><html><head><title>Fragment</title><style>p{color:red}</style></head><body><div class="a"><p>One '
            '<b>bold</b> word<p>Two<ul><li>x<li>y</ul><script>if (1 < 2) { go(); }</script></div></body></html>')


class Handler(http.server.SimpleHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def __init__(self, *arguments, **options):
        super().__init__(*arguments, directory=str(ROOT), **options)

    def log_message(self, *arguments):
        pass

    def answer(self, status, mime, body, headers=()):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Summit-Test', 'developer tools')
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path == '/api/items.json':
            self.answer(200, 'application/json; charset=utf-8', json.dumps(ITEMS, separators=(',', ':')),
                        [('Set-Cookie', 'session=abc123; Path=/; HttpOnly')])
        elif path == '/api/text-json':
            self.answer(200, 'text/plain', json.dumps({'served': 'as text', 'list': [1, 2, 3]}))
        elif path == '/api/broken.json':
            self.answer(200, 'application/json', '{"unfinished": [1, 2')
        elif path == '/api/feed.xml':
            self.answer(200, 'application/rss+xml', FEED)
        elif path == '/api/fragment.html':
            self.answer(200, 'text/html; charset=utf-8', FRAGMENT)
        elif path == '/api/large.json':
            self.answer(200, 'application/json', json.dumps(
                {'rows': [{'index': i, 'text': 'row %d' % i, 'values': [i, i * 2, i * 3]} for i in range(20000)]}))
        elif path == '/api/slow.json':
            time.sleep(1.5)
            self.answer(200, 'application/json', '{"slow":true}')
        elif path == '/api/bytes':
            self.answer(200, 'application/octet-stream', bytes(range(256)))
        elif path == '/redirect':
            self.answer(302, 'text/plain', 'moved', [('Location', '/api/items.json?redirected=1')])
        elif path == '/pixel.png':
            self.answer(200, 'image/png', PNG)
        elif path == '/api/missing':
            self.answer(404, 'application/json', '{"error":"not found","code":404}')
        elif path == '/api/failing':
            self.answer(500, 'text/html', '<html><body><h1>Internal error</h1></body></html>')
        else:
            super().do_GET()

    def do_POST(self):
        length = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(length)
        self.answer(201, 'application/json', json.dumps(
            {'received': body.decode('utf-8', 'replace'), 'type': self.headers.get('Content-Type'), 'length': length}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=8790)
    port = parser.parse_args().port
    with http.server.ThreadingHTTPServer(('0.0.0.0', port), Handler) as server:
        print('http://0.0.0.0:%d/' % port, flush=True)
        server.serve_forever()
