#!/usr/bin/env python3
"""Verify rel=prefetch requests and cache reuse in an owned native Summit instance.

SUMMIT_BENCH_HOST=workstation python3 tools/bench/test-prefetch.py --bundle PATH
The bundle must not already be in use. Results and server requests are retained.
"""
import argparse
import http.server
import json
import pathlib
import secrets
import sys
import threading
import time
import urllib.parse

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--output', type=pathlib.Path)
    args = parser.parse_args()
    nonce = secrets.token_hex(8)
    output = args.output or guest.ROOT / '.vm/bench' / ('prefetch-' + nonce)
    output.mkdir(parents=True, exist_ok=True)
    records, checks = [], []
    lock = threading.Lock()
    servers = []
    ctl = guest.ensure_ctl()
    if guest.foreign_browsers(ctl, args.bundle):
        raise RuntimeError('Selected bundle is already in use')

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            with lock:
                records.append({'port': self.server.server_port, 'path': self.path,
                                'headers': dict(self.headers)})
            url = urllib.parse.urlsplit(self.path)
            query = urllib.parse.parse_qs(url.query)
            mode = query.get('mode', ['cache'])[0]
            body = '<!doctype html><title>Prefetch destination</title><h1>Prefetched page</h1>'
            headers = {'Content-Type': 'text/html; charset=utf-8',
                       'Cache-Control': 'no-store' if mode == 'no-store' or url.path.endswith('/source') else 'max-age=600'}
            status = 200
            if url.path.endswith('/source'):
                target = query['target'][0]
                headers['Set-Cookie'] = 'prefetch_fixture=same-origin; Path=/'
                if mode == 'csp':
                    headers['Content-Security-Policy'] = "default-src 'none'; script-src 'unsafe-inline'"
                body = '''<!doctype html><title>Prefetch starting</title><script>
const link = document.createElement('link'); link.rel = 'prefetch';
const result = { supported: link.relList.supports('prefetch'), event: 'none' };
function report() { document.title = 'PREFETCH '+JSON.stringify(result); }
link.onload = () => { result.event = 'load'; report(); };
link.onerror = () => { result.event = 'error'; report(); };
// Set href after insertion: exercises dynamically supplied hints.
document.head.append(link); link.href = ''' + json.dumps(target) + ''';
setTimeout(report, 1500);
</script>'''
            elif mode == 'error':
                self.close_connection = True
                return
            elif mode == 'http-error':
                status = 404
            data = body.encode()
            self.send_response(status)
            for name, value in headers.items():
                self.send_header(name, value)
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass

    for _ in range(2):
        server = http.server.ThreadingHTTPServer((guest.SERVER_BIND, 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        servers.append((server, thread))
    bases = [f'http://{guest.HOST_ADDRESS}:{s.server_port}/{nonce}' for s, _ in servers]
    report = {'bundle': args.bundle, 'checks': checks, 'requests': records, 'passed': False}
    team = None
    watch = guest.CrashWatch()

    def check(label, condition, detail=None):
        checks.append({'label': label, 'passed': bool(condition), 'detail': detail})
        print(json.dumps(checks[-1]), flush=True)
        if not condition:
            raise AssertionError(label)

    def selected():
        state = guest.state(ctl, team)
        return next((tab for tab in state.get('tabs', []) if tab['id'] == state['selected']), {})

    def wait(predicate, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return True
            time.sleep(.1)
        return False

    def target_requests(target):
        url = urllib.parse.urlsplit(target)
        path = url.path + ('?' + url.query if url.query else '')
        with lock:
            return [r for r in records if r['port'] == url.port and r['path'] == path]

    try:
        for enabled in (True, False):
            remote = f'{guest.GUEST_ROOT}/prefetch-{nonce}-{int(enabled)}'
            env = {'SUMMIT_LIBRARY_PATH_PREFIX': '/boot/home/summit-mesa/prefix-20261002/lib',
                   '__EGL_VENDOR_LIBRARY_FILENAMES': '/boot/home/summit-mesa/prefix-20261002/data/glvnd/egl_vendor.d/50_mesa.json'} if not guest.IS_VM else {}
            if not enabled:
                env['SUMMIT_PREFETCH'] = '0'
            team = guest.launch(args.bundle, remote + '/profile', 'about:blank', remote + '/browser.log', env)
            check('owned browser starts', wait(lambda: guest.state(ctl, team).get('ok')))
            for mode, cross in [('cache', False), ('cache', True), ('no-store', False), ('no-store', True), ('http-error', False), ('error', False), ('csp', False)]:
                ident = f'{int(enabled)}-{mode}-{int(cross)}'
                target = bases[int(cross)] + '/' + ident + '/target?mode=' + mode
                source = bases[0] + '/source?' + urllib.parse.urlencode({'target': target, 'mode': mode, 'id': ident})
                check(ident + ' navigate source', guest.ctl(ctl, team, 'navigate', source)[0] == 0)
                check(ident + ' result', wait(lambda: selected().get('url') == source and selected().get('title', '').startswith('PREFETCH ')))
                result = json.loads(selected()['title'][9:])
                check(ident + ' relList support', result['supported'] == enabled, result)
                expected = 1 if enabled and mode != 'csp' else 0
                requests = target_requests(target)
                check(ident + ' request count', len(requests) == expected, requests)
                if not enabled:
                    check(ident + ' disabled hint has no event', result['event'] == 'none')
                    continue
                if mode in ('error', 'csp'):
                    check(ident + ' rejected hint fires error', result['event'] == 'error')
                    continue
                check(ident + ' completed hint fires load', result['event'] == 'load')
                if mode == 'http-error':
                    continue
                request_headers = {k.lower(): v for k, v in requests[0]['headers'].items()}
                check(ident + ' purpose header', request_headers.get('sec-purpose') == 'prefetch', request_headers)
                # A link without crossorigin uses credentials, and cookies
                # are shared across ports of the same host (RFC 6265).
                check(ident + ' default link credentials', 'prefetch_fixture=' in request_headers.get('cookie', ''))
                check(ident + ' navigate destination', guest.ctl(ctl, team, 'navigate', target)[0] == 0)
                check(ident + ' destination renders', wait(lambda: selected().get('url') == target and selected().get('title') == 'Prefetch destination' and not selected().get('loading')))
                check(ident + ' HTTP cache policy', len(target_requests(target)) == (2 if mode == 'no-store' else 1), target_requests(target))
            cleanup = guest.terminate(ctl, team)
            check('normal browser exit', cleanup.get('method') == 'quit-request', cleanup)
            check('no owned helpers remain', not guest.members(ctl, team))
            team = None
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if team is not None:
            report['cleanup'] = guest.terminate(ctl, team)
        for server, thread in servers:
            server.shutdown(); thread.join(timeout=5); server.server_close()
        report['crashes'] = watch.poll()
        report['passed'] = report['passed'] and not report['crashes']['newReports'] and not report['crashes']['syslogEvents']
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'output': str(output)}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
