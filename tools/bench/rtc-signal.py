#!/usr/bin/env python3
"""A signaling server for tools/bench/pages/rtc-call.html: two browsers on
different machines call each other through it (no third party involved).

    python3 tools/bench/rtc-signal.py [PORT]      (default 8765)

Then open http://HOST:PORT/rtc-call.html?role=a on one machine and
?role=b on the other (same &room=NAME). Messages are kept per room and
recipient until fetched; GET /signal/ROOM/ME[?wait=SECONDS] waits up to 20 s
for one.
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

PAGES = Path(__file__).resolve().parent / 'pages'
queues = {}
condition = threading.Condition()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        if '/signal/' not in self.path:
            sys.stderr.write('%s %s\n' % (self.address_string(), format % args))

    def reply(self, code, body, kind='application/json'):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path, _, query = self.path.partition('?')
        parts = path.strip('/').split('/')
        if len(parts) == 3 and parts[0] == 'signal':
            key = (parts[1], parts[2])
            wait = 20.0
            for pair in query.split('&'):
                if pair.startswith('wait='):
                    try:
                        wait = min(max(float(pair[5:]), 0.0), 20.0)
                    except ValueError:
                        pass
            deadline = time.time() + wait
            with condition:
                while not queues.get(key) and time.time() < deadline:
                    condition.wait(deadline - time.time())
                messages = queues.pop(key, [])
            return self.reply(200, json.dumps(messages))
        name = parts[-1] if parts and parts[-1] else 'rtc-call.html'
        page = PAGES / name
        if len(parts) != 1 or not page.is_file():
            return self.reply(404, 'not found', 'text/plain')
        return self.reply(200, page.read_bytes(), 'text/html; charset=utf-8')

    def do_POST(self):
        parts = self.path.split('?', 1)[0].strip('/').split('/')
        length = int(self.headers.get('Content-Length', '0'))
        body = self.rfile.read(length)
        if len(parts) == 2 and parts[0] == 'reset':
            with condition:
                for key in [k for k in queues if k[0] == parts[1]]:
                    del queues[key]
            return self.reply(200, '{}')
        if len(parts) == 3 and parts[0] == 'signal':
            with condition:
                queues.setdefault((parts[1], parts[2]), []).append(json.loads(body or b'null'))
                condition.notify_all()
            return self.reply(200, '{}')
        return self.reply(404, '{}')


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f'rtc-signal on port {port}', flush=True)
    ThreadingHTTPServer(('', port), Handler).serve_forever()
