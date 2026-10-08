#!/usr/bin/env python3
"""Verify canvas hit regions using native mouse events in an owned browser."""
import argparse
import hashlib
import http.server
import json
import pathlib
import shlex
import subprocess
import threading
import time

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('.vm/hit-regions'))
    parser.add_argument('--port', type=int, default=8778)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    bundle = args.bundle.rstrip('/')
    ctl = guest.ensure_ctl()
    guest.wait_for_free_gui(ctl, bundle)
    remote = guest.GUEST_ROOT + '/hit-regions-' + str(time.time_ns())
    sources = ['tests/ModernHitRegionTests.cpp', 'tests/ModernExtensionManagerTests.cpp',
               'tests/ModernCloseTests.cpp', 'src/ui/Messages.h', 'vendor/nlohmann/json.hpp']
    digest = hashlib.sha256(b'g++-std=c++23-O1-lbe' + b''.join((guest.ROOT / path).read_bytes() for path in sources)).hexdigest()[:20]
    binary = guest.GUEST_ROOT + '/bin/hit-region-check-' + digest
    guest.ssh('mkdir -p ' + shlex.quote(remote) + ' ' + shlex.quote(guest.GUEST_ROOT + '/bin'))
    if guest.ssh('test -x ' + shlex.quote(binary), check=False).returncode:
        archive = subprocess.run(['tar', '-cf', '-', '-C', str(guest.ROOT), *sources], stdout=subprocess.PIPE, check=True).stdout
        guest.ssh('tar -xf - -C ' + shlex.quote(remote), input_bytes=archive)
        compilation = guest.ssh(shlex.join(['g++', '-std=c++23', '-O1', '-Wno-multichar', '-I' + remote + '/vendor',
                               '-I' + remote + '/src', remote + '/tests/ModernHitRegionTests.cpp', '-lbe', '-o', remote + '/check']),
                               timeout=300, check=False)
        (args.output / 'compile.log').write_text(compilation.stdout + compilation.stderr)
        if compilation.returncode:
            raise guest.GuestError('hit-region driver compilation failed; see compile.log')
        guest.ssh('mv ' + shlex.quote(remote + '/check') + ' ' + shlex.quote(binary))
    else:
        (args.output / 'compile.log').write_text('Reused driver ' + digest + '\n')

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != '/test.html':
                self.send_error(404)
                return
            data = (guest.ROOT / 'tests/fixtures/hit-regions.html').read_bytes()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            size = int(self.headers.get('Content-Length', 0))
            if self.path != '/result' or size > 300000:
                self.send_error(400)
                return
            result = json.loads(self.rfile.read(size))
            (args.output / 'dom.json').write_text(json.dumps(result, indent=2) + '\n')
            self.send_response(204)
            self.end_headers()

    server = http.server.ThreadingHTTPServer((guest.SERVER_BIND, args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    result = {'bundle': bundle, 'remote': remote, 'loadBefore': guest.load_report(ctl)}
    watch = guest.CrashWatch()
    team = driver = None
    try:
        team = guest.launch(bundle, remote + '/profile', 'about:blank', remote + '/browser.log',
                            {'SUMMIT_ENABLE_INPUT_SYNTHESIS': '1',
                             'SUMMIT_LIBRARY_PATH_PREFIX': '/boot/home/summit-mesa/prefix-20261002/lib',
                             '__EGL_VENDOR_LIBRARY_FILENAMES': '/boot/home/summit-mesa/prefix-20261002/data/glvnd/egl_vendor.d/50_mesa.json'})
        result['team'] = team
        launch = '''import subprocess,sys
remote,binary,team,bundle,url=sys.argv[1:]
p=subprocess.Popen([binary,team,bundle+'/Summit',url],stdin=subprocess.DEVNULL,
 stdout=open(remote+'/native.log','wb'),stderr=subprocess.STDOUT,start_new_session=True)
print(p.pid)
'''
        launched = guest.ssh('python3.10 - ' + shlex.join([remote, binary, str(team), bundle,
                            f'http://{guest.HOST_ADDRESS}:{args.port}/test.html']), input_bytes=launch.encode())
        driver = int(launched.stdout.strip().splitlines()[-1])
        result['driver'] = driver
        (args.output / 'active.json').write_text(json.dumps(result, indent=2) + '\n')
        deadline = time.monotonic() + 240
        while guest.alive(driver) and time.monotonic() < deadline:
            time.sleep(1)
        if guest.alive(driver):
            result['timedOut'] = True
    finally:
        if driver is not None:
            if guest.alive(driver):
                result['driverCleanup'] = guest.terminate(ctl, driver)
            guest.fetch_file(remote + '/native.log', args.output / 'native.log')
        if team is not None:
            if guest.alive(team):
                result['cleanup'] = guest.terminate(ctl, team)
            result['members'] = guest.members(ctl, team)
            guest.fetch_file(remote + '/browser.log', args.output / 'browser.log')
        server.shutdown()
        server.server_close()
        result['crashes'] = watch.poll()
        dom = json.loads((args.output / 'dom.json').read_text()) if (args.output / 'dom.json').exists() else {}
        log = (args.output / 'native.log').read_text() if (args.output / 'native.log').exists() else ''
        result['checks'] = len(dom.get('checks', []))
        result['clicks'] = len(dom.get('clicks', []))
        result['passed'] = (bool(dom.get('passed')) and all(check['passed'] for check in dom.get('checks', []))
                            and 'HitRegion_RESULT PASS' in log and not result.get('members')
                            and not result.get('timedOut') and not result['crashes']['newReports']
                            and not result['crashes']['syslogEvents'])
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
