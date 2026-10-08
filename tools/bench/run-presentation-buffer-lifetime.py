#!/usr/bin/env python3
"""Check retained graphics state across hiding, background updates and resizing."""
import argparse
import http.server
import json
import pathlib
import threading
import time

from PIL import Image, ImageChops

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--port', type=int, default=8950)
    parser.add_argument('--label', default='')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    args = parser.parse_args()
    run_id = time.strftime('presentation-lifetime-%Y%m%d-%H%M%S')
    if args.label:
        run_id += '-' + args.label
    output = guest.ROOT / '.vm/bench' / run_id
    output.mkdir(parents=True)
    remote = guest.GUEST_ROOT + '/' + run_id
    fixture = (guest.BENCH / 'pages/presentation-buffer-lifetime.html').read_bytes()
    events = []
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(fixture)))
            self.end_headers()
            self.wfile.write(fixture)

        def do_POST(self):
            size = int(self.headers.get('Content-Length', 0))
            if self.path != '/event' or not 0 < size < 65536:
                self.send_error(400)
                return
            event = json.loads(self.rfile.read(size))
            event['at'] = time.time()
            with lock:
                events.append(event)
            self.send_response(204)
            self.end_headers()

    ctl = guest.ensure_ctl()
    guest.wait_for_free_gui(ctl, args.bundle)
    server = http.server.ThreadingHTTPServer((guest.SERVER_BIND, args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    watch = guest.CrashWatch()
    team = group = None
    env = dict(item.split('=', 1) for item in args.env)
    env.setdefault('SUMMIT_ENABLE_INPUT_SYNTHESIS', '1')
    result = {'bundle': args.bundle, 'env': env, 'passed': False}

    def save():
        with lock:
            result['events'] = list(events)
        (output / 'run.json').write_text(json.dumps(result, indent=2) + '\n')

    def command(*values):
        code, text, error = guest.ctl(ctl, team, *values, timeout_ms=15000)
        if code:
            raise guest.GuestError(f'{values}: {error or text}')

    def wait_event(phase, after=0, timeout=70):
        deadline = time.monotonic() + timeout
        next_wake = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_wake:
                guest.wake_display()
                next_wake = time.monotonic() + 10
            with lock:
                found = [event for event in events
                         if event['phase'] == phase and event['at'] > after]
            if found:
                event = found[-1]
                save()
                if not event['passed'] or event['contexts'] != 2:
                    raise RuntimeError(f'Graphics state check failed: {event}')
                return event
            if not guest.alive(team):
                raise RuntimeError('Owned browser exited')
            time.sleep(.25)
        raise TimeoutError(f'No {phase} event')

    def capture(name):
        guest.wake_display()
        time.sleep(.25)
        guest.screenshot(output / name)

    try:
        guest.wake_display()
        group = guest.launch(args.bundle, remote + '/profile', 'about:blank',
                             remote + '/browser.log', env)
        team = guest.find_browser(ctl, group)
        result.update(team=team, group=group)
        command('frame', '4', '1', '1283', '997')
        command('sidebar')
        command('navigate', f'http://{guest.HOST_ADDRESS}:{args.port}/')
        wait_event('initial')
        capture('initial.png')
        tab = next(tab for tab in guest.state(ctl, team)['tabs']
                   if f':{args.port}/' in tab['url'])
        command('newtab', 'about:blank')
        for _ in range(4):
            guest.wake_display()
            time.sleep(10)
        before = time.time()
        command('selecttab', str(tab['id']))
        restored = wait_event('restored', before)
        if restored['hidden'] or restored['mutated'] or restored['hides'] != 1:
            raise RuntimeError(f'Unexpected restoration state: {restored}')
        capture('restored.png')
        # Page content in the fixed native window, excluding browser chrome
        # and the right edge. The same crop is used for both captures.
        box = (8, 130, 1268, 970)
        initial = Image.open(output / 'initial.png').convert('RGB')
        restored_image = Image.open(output / 'restored.png').convert('RGB')
        if initial.size != restored_image.size or initial.width < box[2] or initial.height < box[3]:
            raise RuntimeError('Unexpected screenshot dimensions')
        if all(low == high for low, high in initial.crop(box).getextrema()):
            raise RuntimeError('Initial screenshot contains no page detail')
        difference = ImageChops.difference(initial.crop(box), restored_image.crop(box))
        result['restoredPixelMatch'] = difference.getbbox() is None
        if not result['restoredPixelMatch']:
            difference.save(output / 'difference.png')
            raise RuntimeError('Restored page pixels differ')
        blank = next(item for item in guest.state(ctl, team)['tabs'] if item['id'] != tab['id'])
        before = time.time()
        command('selecttab', str(blank['id']))
        mutation = wait_event('hidden-mutation', before)
        if not mutation['hidden'] or not mutation['mutated']:
            raise RuntimeError(f'Background update did not run while hidden: {mutation}')
        command('frame', '4', '1', '1183', '897')
        before = time.time()
        command('selecttab', str(tab['id']))
        restored = wait_event('restored', before)
        if not restored['mutated'] or restored['hidden'] or restored['width'] != 1180:
            raise RuntimeError(f'Updated page did not restore after resizing: {restored}')
        capture('mutated-resized.png')
        result['passed'] = True
    except Exception as error:
        result['error'] = repr(error)
    finally:
        if group is not None:
            result['shutdown'] = guest.terminate(ctl, team or group, group=group, grace=40)
            result['leftovers'] = guest.members(ctl, group)
            guest.fetch_file(remote + '/browser.log', output / 'browser.log', tail_bytes=1000000)
        result['crashes'] = watch.poll()
        result['passed'] = (result['passed'] and not result.get('leftovers')
                            and not result['crashes']['newReports']
                            and not result['crashes']['syslogEvents']
                            and result.get('shutdown', {}).get('method') == 'quit-request')
        save()
        server.shutdown()
        server.server_close()
    print(json.dumps({'passed': result['passed'], 'report': str(output / 'run.json')}))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
