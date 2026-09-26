import http.server, sys, time, re
SIZE = int(sys.argv[2]); MODE = sys.argv[3]
def body(a, b): return bytes(((i * 2654435761) >> 13) & 0xff for i in range(a, b))
DATA = body(0, SIZE)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.startswith('/redirect'):
            self.send_response(302); self.send_header('Location', '/file'); self.send_header('Content-Length', '0'); self.end_headers(); return
        r = self.headers.get('Range'); start, end = 0, SIZE - 1
        if r and MODE != 'norange':
            m = re.match(r'bytes=(\d+)-(\d*)', r); start = int(m.group(1)); end = int(m.group(2)) if m.group(2) else SIZE - 1
            self.send_response(206); self.send_header('Content-Range', f'bytes {start}-{end}/{SIZE}')
        else:
            self.send_response(200)
        if MODE == 'chunked':
            self.send_header('Transfer-Encoding', 'chunked'); self.end_headers()
            self.wfile.write(b'%x\r\n' % (end + 1 - start) + DATA[start:end + 1] + b'\r\n0\r\n\r\n'); return
        self.send_header('Content-Length', str(end + 1 - start)); self.end_headers()
        try:
            for off in range(start, end + 1, 65536):
                self.wfile.write(DATA[off:min(end + 1, off + 65536)])
                if MODE == 'slow': time.sleep(0.01)
        except (BrokenPipeError, ConnectionResetError): pass
http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
