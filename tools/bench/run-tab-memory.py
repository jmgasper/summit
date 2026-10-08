#!/usr/bin/env python3
"""Measure owned tabs through load, hide, show and close using local fixtures.

Reports resident area sums (shared mappings can be counted twice), system used
memory, CPU, per-process listarea dumps and page-state/pixel checks. This is a
lifetime diagnostic, not a claim about typical website memory consumption.
"""
import argparse
import http.server
import json
import pathlib
import statistics
import threading
import time
import urllib.parse

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--tabs', type=int, default=12)
    parser.add_argument('--kind', choices=['blank', 'dom', 'canvas', 'layers', 'mixed'], default='mixed')
    parser.add_argument('--settle', type=float, default=20)
    parser.add_argument('--close-settle', type=float, default=45)
    parser.add_argument('--samples', type=int, default=8)
    parser.add_argument('--port', type=int, default=8949)
    parser.add_argument('--window', default='4,1,1283,997')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--label', default='')
    args = parser.parse_args()
    if args.tabs < 1 or args.samples < 1 or args.settle < 0 or args.close_settle < 0:
        parser.error('tabs and samples must be positive; settle times must be nonnegative')
    run_id = time.strftime('tab-memory-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    output = guest.ROOT / '.vm/bench' / run_id
    output.mkdir(parents=True)
    remote = guest.GUEST_ROOT + '/' + run_id
    events = []
    event_lock = threading.Lock()
    fixture = (guest.BENCH / 'pages/tab-memory.html').read_bytes()

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if urllib.parse.urlsplit(self.path).path != '/tab':
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(fixture)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(fixture)

        def do_POST(self):
            size = int(self.headers.get('Content-Length', 0))
            if self.path != '/event' or not 0 < size < 65536:
                self.send_error(400)
                return
            event = json.loads(self.rfile.read(size))
            event['at'] = time.time()
            with event_lock:
                events.append(event)
            self.send_response(204)
            self.end_headers()

    ctl = guest.ensure_ctl()
    guest.require_volume()
    guest.wait_for_free_gui(ctl, args.bundle)
    server = http.server.ThreadingHTTPServer((guest.SERVER_BIND, args.port), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    watch = guest.CrashWatch()
    env = dict(item.split('=', 1) for item in args.env)
    env.setdefault('SUMMIT_ENABLE_INPUT_SYNTHESIS', '1')
    env.setdefault('SUMMIT_UI_FRAME_STATS', '1')
    report = {'id': run_id, 'bundle': args.bundle, 'machine': guest.HOST, 'env': env,
              'arguments': vars(args), 'phases': {}, 'startedAt': time.time()}
    group = team = None

    def save():
        with event_lock:
            report['events'] = list(events)
        (output / 'run.json').write_text(json.dumps(report, indent=2) + '\n')

    def command(*arguments):
        code, text, error = guest.ctl(ctl, team, *arguments, timeout_ms=10000)
        if code:
            raise guest.GuestError(f'{arguments}: {code}: {error or text}')

    def wait_event(index, after=0, visible=None, timeout=40):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with event_lock:
                found = [e for e in events if e['id'] == index and e['at'] > after
                         and (visible is None or e['visible'] == visible)]
            if found:
                if not all(e['passed'] for e in found):
                    raise RuntimeError(f'Page state/pixel check failed: {found}')
                return found[-1]
            if not guest.alive(team):
                raise RuntimeError('Owned browser exited')
            time.sleep(0.25)
        raise TimeoutError(f'No fixture event for tab {index}, visible={visible}')

    def snapshot(label):
        members = dict(guest.members(ctl, group))
        samples = [guest.sample(ctl, members, 1000) for _ in range(args.samples)]
        (output / (label + '-samples.json')).write_text(json.dumps(samples, indent=2) + '\n')
        for pid, image in members.items():
            areas = guest.ssh('listarea ' + str(pid), check=False).stdout
            (output / f'{label}-{guest.role(image)}-{pid}.areas').write_text(areas)
        own_memory = [sum(t.get('ramBytes', 0) for t in s['teams'] if t['team'] in members) / 2**20 for s in samples]
        own_cpu = [sum(t['cores'] for t in s['teams'] if t['team'] in members) for s in samples]
        data = {'members': members, 'residentAreaMiB': statistics.median(own_memory),
                'systemUsedMiB': statistics.median(s['usedBytes'] / 2**20 for s in samples),
                'ownCoresMean': statistics.mean(own_cpu),
                'busyCoresMean': statistics.mean(s['busyCores'] for s in samples)}
        report['phases'][label] = data
        save()
        print(label, json.dumps(data), flush=True)

    try:
        report['loadBefore'] = guest.load_report(ctl)
        guest.wake_display()
        group = guest.launch(args.bundle, remote + '/profile', 'about:blank', remote + '/browser.log', env)
        report['group'] = group
        save()
        team = guest.find_browser(ctl, group)
        report['team'] = team
        command('frame', *args.window.split(','))
        command('sidebar')
        time.sleep(3)
        snapshot('single-blank')
        kinds = ['blank', 'dom', 'canvas', 'layers'] if args.kind == 'mixed' else [args.kind]
        for index in range(args.tabs):
            url = f'http://{guest.HOST_ADDRESS}:{args.port}/tab?id={index}&kind={kinds[index % len(kinds)]}'
            command('navigate' if index == 0 else 'newtab', url)
            wait_event(index)
        snapshot('loaded')
        time.sleep(args.settle)
        snapshot('hidden-settled')
        state = guest.state(ctl, team)
        if len(state.get('tabs', [])) != args.tabs:
            raise RuntimeError('Owned window has an unexpected tab count')
        tabs = state['tabs']
        switches = []
        for tab in tabs[:-1] + tabs[-1:]:
            index = int(urllib.parse.parse_qs(urllib.parse.urlsplit(tab['url']).query)['id'][0])
            before = time.time()
            # Selecting the already selected tab produces no visibility event.
            if args.tabs == 1:
                before = 0
            else:
                command('selecttab', str(tab['id']))
            checked = wait_event(index, after=before, visible=True)
            current = guest.state(ctl, team)
            shown = next(t for t in current['tabs'] if t['id'] == tab['id'])
            switches.append({'id': index, 'checked': checked, 'shownAt': shown.get('shownAt'),
                             'firstFrameAfterShow': shown.get('firstFrameAfterShow')})
        report['switches'] = switches
        guest.screenshot(output / 'after-switching.png')
        snapshot('after-switching')
        for tab in tabs[:-1]:
            command('closetab', str(tab['id']))
        command('navigate', 'about:blank')
        snapshot('closed-immediate')
        time.sleep(args.close_settle)
        snapshot('closed-settled')
        report['finalState'] = guest.state(ctl, team)
        report['outcome'] = 'completed'
    except Exception as error:
        report['outcome'] = 'failed'
        report['error'] = repr(error)
        raise
    finally:
        if team is not None:
            report['shutdown'] = guest.terminate(ctl, team, group=group, grace=40)
        elif group is not None:
            report['shutdown'] = guest.terminate(ctl, group, group=group, grace=40)
        if group is not None:
            report['leftovers'] = guest.members(ctl, group)
            guest.fetch_file(remote + '/browser.log', output / 'browser.log', tail_bytes=4_000_000)
        report['crashes'] = watch.poll()
        report['finishedAt'] = time.time()
        server.shutdown()
        server.server_close()
        save()
        print('report:', output / 'run.json', flush=True)
    if report.get('leftovers') or report['crashes']['newReports'] or report['crashes']['syslogEvents']:
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
