#!/usr/bin/env python3
"""Serve an MJPEG stream that goes quiet, the way a KVM does without a signal.

NanoKVM keeps its multipart/x-mixed-replace response open and simply stops
sending frames while the target machine shows no picture. A browser must keep
such a stream open; giving up turns the <img> into an error and NanoKVM's page
then hides the screen until it is reloaded.

GET /           test page: <img> on the stream, reports load/error events and
                a picture checksum to /log every second
GET /stream     ?before=S&pause=S&after=S&fps=N (defaults 3, 70, 30, 10)
POST /log       appends the page's JSON report to the --log file
"""
import argparse
import http.server
import io
import json
import socketserver
import time

PAGE = """<!doctype html><meta charset="utf-8"><title>mjpeg stall</title>
<style>body{margin:0;background:#000;color:#ccc;font:14px sans-serif}img{display:block;width:640px;height:360px}</style>
<img id="screen" alt="screen"><pre id="out"></pre>
<script>
const query = location.search || '?';
const img = document.getElementById('screen');
const start = performance.now();
const now = () => Math.round(performance.now() - start);
const report = {events: [], samples: []};
img.onload = () => report.events.push(['load', now()]);
img.onerror = () => report.events.push(['error', now()]);
img.src = '/stream' + query + '&v=' + Date.now();
const canvas = document.createElement('canvas');
canvas.width = 8; canvas.height = 8;
const context = canvas.getContext('2d', {willReadFrequently: true});
setInterval(() => {
  let colour = 'none';
  try {
    context.drawImage(img, 0, 0, 8, 8);
    const d = context.getImageData(4, 4, 1, 1).data;
    colour = d[0] > 128 ? 'red' : d[2] > 128 ? 'blue' : d[1] > 128 ? 'green' : 'dark';
  } catch (e) { colour = 'exception'; }
  report.samples.push([now(), colour, img.complete, img.naturalWidth]);
  document.getElementById('out').textContent = JSON.stringify(report.events) + ' ' + colour;
  document.title = 'mjpeg stall ' + colour + ' ' + report.events.map(e => e[0]).join(',');
  fetch('/log', {method: 'POST', body: JSON.stringify({ua: navigator.userAgent, query, at: now(), events: report.events, last: report.samples.slice(-1)[0]})});
}, 1000);
</script>
"""


def jpeg(colour):
    from PIL import Image
    out = io.BytesIO()
    Image.new('RGB', (320, 180), colour).save(out, 'JPEG', quality=80)
    return out.getvalue()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        path, _, query = self.path.partition('?')
        if path == '/':
            body = PAGE.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path != '/stream':
            self.send_error(404)
            return
        params = dict(item.partition('=')[::2] for item in query.split('&') if item)
        before, pause, after = (float(params.get(k, d)) for k, d in (('before', 3), ('pause', 70), ('after', 30)))
        fps = float(params.get('fps', 10))
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('Transfer-Encoding', 'chunked')
        self.end_headers()
        frames = {'red': jpeg('red'), 'blue': jpeg('blue')}
        started = time.monotonic()
        try:
            while True:
                elapsed = time.monotonic() - started
                if elapsed > before + pause + after:
                    break
                if before <= elapsed < before + pause:
                    time.sleep(0.1)
                    continue
                data = frames['red' if elapsed < before else 'blue']
                part = (b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n' % len(data)) + data + b'\r\n'
                self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
                self.wfile.flush()
                time.sleep(1 / fps)
            self.wfile.write(b'0\r\n\r\n')
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)
        with open(self.server.log_path, 'ab') as log:
            log.write(body + b'\n')
        self.send_response(204)
        self.send_header('Content-Length', '0')
        self.end_headers()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', type=int, default=8934)
    parser.add_argument('--log', default='mjpeg-stall.jsonl')
    args = parser.parse_args()
    server = Server(('', args.port), Handler)
    server.log_path = args.log
    server.serve_forever()


if __name__ == '__main__':
    main()
