#!/usr/bin/env python3
"""Tab/navigation stress test for Summit in the Haiku test VM.

Drives one Summit instance (its own fresh profile, addressed by team id only)
through a seeded random sequence of: open tab, navigate, select tab, close tab,
back/forward and reload, over a mix of heavy real sites and local pages served
from the Speedometer checkout. While it runs it samples CPU, threads, areas and
resident area memory of every process in the browser's process group, and watches
for crashes (new *.report files on the guest Desktop, debugger lines in
/var/log/syslog, helper processes disappearing) and hangs (the window not
answering a scripting request).

Artifacts: .vm/bench/<run-id>/{run.json,actions.jsonl,samples.jsonl,browser.log,
final.png,*.report}.

  python3 tools/bench/stress-browser.py --duration 600
  python3 tools/bench/stress-browser.py --duration 3600 --max-tabs 6 --seed 7 --local-only
"""
import argparse
import json
import pathlib
import random
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

REAL_SITES = [
    'https://en.wikipedia.org/wiki/Haiku_(operating_system)',
    'https://en.wikipedia.org/wiki/WebKit',
    'https://news.ycombinator.com/',
    'https://www.bbc.com/news',
    'https://www.theguardian.com/international',
    'https://github.com/WebKit/WebKit',
    'https://developer.mozilla.org/en-US/docs/Web/JavaScript',
    'https://www.haiku-os.org/',
    'https://www.reddit.com/r/haikuOS/',
    'https://www.youtube.com/',
    'https://www.openstreetmap.org/#map=12/-42.8821/147.3272',
    'https://example.com/',
]
LOCAL_PAGES = [
    '/resources/todomvc/architecture-examples/react-complex/dist/index.html',
    '/resources/todomvc/architecture-examples/angular-complex/dist/index.html',
    '/resources/newssite/news-next/dist/index.html',
    '/resources/newssite/news-nuxt/dist/index.html',
    '/resources/editors/dist/codemirror.html',
    '/resources/charts/dist/chartjs.html',
    '/resources/charts/dist/observable-plot.html',
    '/resources/react-stockcharts/build/index.html?type=svg',
    '/__bench/pages/probe.html',
]


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


class Stress:
    def __init__(self, args, directory):
        self.args = args
        self.directory = directory
        self.random = random.Random(args.seed)
        self.ctl = guest.ensure_ctl()
        self.actions = (directory / 'actions.jsonl').open('a', buffering=1)
        self.samples = (directory / 'samples.jsonl').open('a', buffering=1)
        self.team = self.group = None
        self.started = time.time()
        self.findings = []
        self.consecutive_hangs = 0
        local = [f'http://10.0.2.2:{args.port}{path}' for path in LOCAL_PAGES]
        self.urls = local if args.local_only else REAL_SITES + local
        if args.urls:
            self.urls = [line.strip() for line in pathlib.Path(args.urls).read_text().splitlines()
                         if line.strip() and not line.startswith('#')]

    def elapsed(self):
        return round(time.time() - self.started, 1)

    def finding(self, kind, **details):
        entry = dict(kind=kind, at=self.elapsed(), **details)
        self.findings.append(entry)
        log(f'FINDING {kind}: {json.dumps(details)[:300]}')
        return entry

    def state(self):
        current = guest.state(self.ctl, self.team, timeout_ms=self.args.hang_timeout * 1000)
        if current.get('ok'):
            self.consecutive_hangs = 0
            return current
        if current.get('exit') == 3:
            raise BrowserGone('Summit team no longer exists')
        self.consecutive_hangs += 1
        self.finding('ui-no-reply', consecutive=self.consecutive_hangs, error=current.get('error'))
        if self.consecutive_hangs >= 3:
            shot = self.directory / f'hang-{int(self.elapsed())}.png'
            guest.screenshot(shot)
            raise BrowserHung(f'window did not answer {self.consecutive_hangs} scripting requests in a row')
        return None

    def sample(self, label):
        processes = guest.members(self.ctl, self.group)
        data = guest.sample(self.ctl, [pid for pid, _ in processes], 1000)
        rows = []
        for pid, image in processes:
            row = next((t for t in data['teams'] if t['team'] == pid), None)
            if row:
                rows.append({'team': pid, 'role': guest.role(image), 'cores': row['cores'], 'threads': row['threads'],
                             'areas': row['areas'], 'ramMiB': round(row.get('ramBytes', 0) / 2**20, 1),
                             'virtualMiB': round(row.get('virtualBytes', 0) / 2**20, 1)})
        entry = {'at': self.elapsed(), 'label': label, 'systemUsedMiB': round(data['usedBytes'] / 2**20),
                 'busyCores': data['busyCores'], 'processes': rows}
        self.samples.write(json.dumps(entry) + '\n')
        return entry

    def wait_loaded(self, expect_url=None):
        """Poll until the selected tab stops loading; returns (state, seconds, timed_out)."""
        begin = time.time()
        current = None
        time.sleep(1.5)
        while time.time() - begin < self.args.nav_timeout:
            current = self.state()
            if current:
                selected = next((t for t in current['tabs'] if t['id'] == current['selected']), None)
                if selected and not selected['loading'] and selected['url'] not in ('', 'about:blank'):
                    return current, time.time() - begin, False
            time.sleep(2)
        return current, time.time() - begin, True

    def act(self, current):
        tabs = current['tabs']
        choices = ['navigate'] * 4 + ['select'] * 2 + ['history'] * 2 + ['reload']
        choices += ['newtab'] * (6 if len(tabs) < self.args.max_tabs else 0)
        choices += ['closetab'] * (5 if len(tabs) > 1 else 0)
        if len(tabs) >= self.args.max_tabs:
            choices += ['closetab'] * 4
        action = self.random.choice(choices)
        record = {'at': self.elapsed(), 'action': action, 'tabsBefore': len(tabs)}
        if action in ('navigate', 'newtab'):
            record['url'] = self.random.choice(self.urls)
            code, _, err = guest.ctl(self.ctl, self.team, action, record['url'])
        elif action == 'select':
            record['tab'] = self.random.choice(tabs)['id']
            code, _, err = guest.ctl(self.ctl, self.team, 'selecttab', record['tab'])
        elif action == 'closetab':
            record['tab'] = self.random.choice(tabs)['id']
            code, _, err = guest.ctl(self.ctl, self.team, 'closetab', record['tab'])
        elif action == 'history':
            record['direction'] = self.random.choice(['back', 'forward'])
            code, _, err = guest.ctl(self.ctl, self.team, record['direction'])
        else:
            code, _, err = guest.ctl(self.ctl, self.team, 'reload')
        record['sendExit'] = code
        if code:
            record['sendError'] = err.strip()
        after, seconds, timed_out = self.wait_loaded()
        record['seconds'] = round(seconds, 1)
        record['loadTimedOut'] = timed_out
        if after:
            selected = next((t for t in after['tabs'] if t['id'] == after['selected']), {})
            record.update(tabsAfter=len(after['tabs']), title=selected.get('title', '')[:80],
                          finalUrl=selected.get('url', '')[:160], loadError=selected.get('loadErrorText') or None,
                          replyMicros=after.get('replyMicros'))
        self.actions.write(json.dumps(record) + '\n')
        return record

    def run(self):
        args = self.args
        guest.require_volume()
        guest.wait_for_free_gui(self.ctl, args.bundle, args.wait_gui, log)
        guest_dir = f'{guest.GUEST_ROOT}/runs/{self.directory.name}'
        self.watch = guest.CrashWatch()
        guest.wake_display()
        start_url = f'http://10.0.2.2:{args.port}/about.html'
        self.group = guest.launch(args.bundle, f'{guest_dir}/profile', start_url, f'{guest_dir}/browser.log')
        self.team = guest.find_browser(self.ctl, self.group)
        log(f'launched Summit team {self.team}')
        guest.ctl(self.ctl, self.team, 'frame', *args.window.split(','))
        time.sleep(8)
        baseline = self.sample('start-idle')
        outcome = 'completed'
        counts = {}
        last_sample = last_watch = time.time()
        try:
            while time.time() - self.started < args.duration:
                current = self.state()
                if not current:
                    time.sleep(3)
                    continue
                record = self.act(current)
                counts[record['action']] = counts.get(record['action'], 0) + 1
                if record.get('loadTimedOut'):
                    counts['loadTimeouts'] = counts.get('loadTimeouts', 0) + 1
                if record.get('loadError'):
                    counts['loadErrors'] = counts.get('loadErrors', 0) + 1
                if time.time() - last_sample >= args.sample_interval:
                    last_sample = time.time()
                    entry = self.sample('running')
                    roles = [p['role'] for p in entry['processes']]
                    log(f"t+{entry['at']:.0f}s actions={sum(v for k, v in counts.items() if not k.startswith('load'))} "
                        + ' '.join(f"{p['role'][:3]}:{p['ramMiB']:.0f}M/{p['threads']}t" for p in entry['processes']))
                    if 'NetworkProcess' not in roles:
                        self.finding('network-process-missing', processes=roles)
                    guest.wake_display()
                if time.time() - last_watch >= 30:
                    last_watch = time.time()
                    crash = self.watch.poll()
                    fresh = [name for name in crash['newReports'] if name not in getattr(self, 'seen_reports', set())]
                    if fresh or crash['syslogEvents']:
                        self.seen_reports = getattr(self, 'seen_reports', set()) | set(fresh)
                        for name in fresh:
                            self.watch.fetch_report(name, self.directory / name)
                        if fresh:
                            self.finding('crash-report', reports=fresh, lastAction=record)
                    others = guest.foreign_browsers(self.ctl, self.args.bundle, self.group)
                    if others:
                        self.finding('foreign-summit-instance', teams=others)
        except BrowserGone as error:
            outcome = 'browser-exited'
            self.finding('browser-exited', error=str(error))
        except BrowserHung as error:
            outcome = 'browser-hung'
            self.finding('browser-hung', error=str(error))
        except KeyboardInterrupt:
            outcome = 'interrupted'

        summary = {'outcome': outcome, 'counts': counts, 'baseline': baseline}
        try:
            if outcome == 'completed':
                # Settle: one blank-ish tab, then compare resident memory with the start.
                current = self.state() or {'tabs': []}
                for tab in current['tabs'][1:]:
                    guest.ctl(self.ctl, self.team, 'closetab', tab['id'])
                    time.sleep(1)
                guest.ctl(self.ctl, self.team, 'navigate', start_url)
                time.sleep(args.settle)
                summary['settled'] = self.sample('end-idle')
            guest.screenshot(self.directory / 'final.png')
        except Exception as error:  # still collect logs below
            self.finding('settle-failed', error=str(error))
        crash = self.watch.poll()
        for name in crash['newReports']:
            if not (self.directory / name).exists():
                self.watch.fetch_report(name, self.directory / name)
        summary['crash'] = crash
        if crash['syslogEvents'] or crash['newReports'] or outcome != 'completed':
            (self.directory / 'syslog-tail.txt').write_text(self.watch.syslog_tail(300))
        summary['shutdown'] = guest.terminate(self.ctl, self.team, group=self.group)
        guest.fetch_file(f'{guest_dir}/browser.log', self.directory / 'browser.log', tail_bytes=8 * 1024 * 1024)
        guest.ssh(f'rm -rf {guest_dir}/profile', check=False, timeout=600)
        summary['findings'] = self.findings
        return summary


class BrowserGone(Exception):
    pass


class BrowserHung(Exception):
    pass


def memory_trend(directory):
    """Per role: first/last/max resident MiB and a least-squares slope in MiB/minute."""
    series = {}
    for line in (directory / 'samples.jsonl').read_text().splitlines():
        entry = json.loads(line)
        by_role = {}
        for process in entry['processes']:
            by_role.setdefault(process['role'], []).append(process)
        for role, processes in by_role.items():
            series.setdefault(role, []).append((entry['at'] / 60, sum(p['ramMiB'] for p in processes),
                                                sum(p['threads'] for p in processes), len(processes)))
    trend = {}
    for role, points in series.items():
        n = len(points)
        mean_x = sum(p[0] for p in points) / n
        mean_y = sum(p[1] for p in points) / n
        denominator = sum((p[0] - mean_x) ** 2 for p in points) or 1
        slope = sum((p[0] - mean_x) * (p[1] - mean_y) for p in points) / denominator
        trend[role] = {'samples': n, 'firstMiB': points[0][1], 'lastMiB': points[-1][1],
                       'maxMiB': max(p[1] for p in points), 'slopeMiBPerMinute': round(slope, 2),
                       'firstThreads': points[0][2], 'lastThreads': points[-1][2],
                       'maxThreads': max(p[2] for p in points), 'maxProcesses': max(p[3] for p in points)}
    return trend


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--duration', type=int, default=600, help='seconds of actions (default 600)')
    parser.add_argument('--max-tabs', type=int, default=4)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--nav-timeout', type=int, default=45, help='seconds to wait for a load to finish')
    parser.add_argument('--hang-timeout', type=int, default=15, help='seconds a scripting reply may take')
    parser.add_argument('--sample-interval', type=int, default=20)
    parser.add_argument('--settle', type=int, default=45, help='idle seconds before the final memory sample')
    parser.add_argument('--local-only', action='store_true', help='skip internet sites')
    parser.add_argument('--urls', help='file with one URL per line to use instead of the built-in set')
    parser.add_argument('--port', type=int, default=8931)
    parser.add_argument('--window', default='4,1,1270,797')
    parser.add_argument('--wait-gui', type=float, default=0, metavar='MINUTES')
    parser.add_argument('--label', default='')
    args = parser.parse_args()

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    run_id = time.strftime('stress-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    directory = guest.ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    server = subprocess.Popen([sys.executable, str(guest.BENCH / 'serve-speedometer.py'), '--port', str(args.port),
                               '--out-dir', str(directory)], stdout=(directory / 'server.log').open('w'),
                              stderr=subprocess.STDOUT)
    time.sleep(1)
    stress = Stress(args, directory)
    load_before = guest.load_report(stress.ctl, (), 3000)
    try:
        summary = stress.run()
    finally:
        server.terminate()
    summary.update(id=run_id, bundle=args.bundle, seed=args.seed, duration=args.duration, urls=stress.urls,
                   loadBefore=load_before, loadAfter=guest.load_report(stress.ctl, (), 3000),
                   memoryTrend=memory_trend(directory))
    text = (directory / 'browser.log').read_text(errors='replace') if (directory / 'browser.log').exists() else ''
    summary['browserLog'] = {'bytes': len(text), 'hardLinkErrors': text.count('Failed to create hard link')}
    (directory / 'run.json').write_text(json.dumps(summary, indent=1))

    print()
    print(f"stress run : {run_id}   outcome: {summary['outcome']}   duration {args.duration}s seed {args.seed}")
    print(f"actions    : {summary['counts']}")
    print(f"{'process':16} {'first MiB':>10} {'last MiB':>10} {'max MiB':>10} {'MiB/min':>9} {'threads first/last/max':>24} {'max procs':>10}")
    for role, t in summary['memoryTrend'].items():
        print(f"{role:16} {t['firstMiB']:10.1f} {t['lastMiB']:10.1f} {t['maxMiB']:10.1f} {t['slopeMiBPerMinute']:9.2f} "
              f"{str(t['firstThreads']) + '/' + str(t['lastThreads']) + '/' + str(t['maxThreads']):>24} {t['maxProcesses']:10d}")
    print(f"crash reports: {summary['crash']['newReports'] or 'none'}; syslog debugger events: {len(summary['crash']['syslogEvents'])}")
    print(f"findings   : {len(summary['findings'])}")
    for finding in summary['findings'][:20]:
        print('  -', json.dumps(finding)[:240])
    print(f'artifacts  : {directory}')
    return 0 if summary['outcome'] == 'completed' and not summary['crash']['newReports'] else 1


if __name__ == '__main__':
    sys.exit(main())
