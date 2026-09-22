#!/usr/bin/env python3
"""Capture a real-site wheel burst in Summit and, when available, frame pacing.

The page is given time to settle, then a paced burst of mouse wheel notches is
delivered through the browser's input path (summitctl scroll, which needs
SUMMIT_ENABLE_INPUT_SYNTHESIS=1). Before/after screenshots record visible
movement. Frame statistics are reported only when the engine or an opt-in
Summit view actually emits them.

Everything for one run lands in .vm/bench/<run-id>/.

Check the screenshots before treating a synthetic burst as real scrolling. An
older bundle accepted the notches without moving the page. The view-token fix
did move Reddit's /r/popular/ feed on 2026-09-23, but movement still needs
visual confirmation for each run.

Examples:
  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-scroll.py --bundle <path> https://www.reddit.com/
  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-scroll.py --env SUMMIT_DISABLE_GL_COMPOSITING=1 <url>
"""
import argparse
import json
import pathlib
import re
import signal
import statistics
import subprocess
import sys
import time
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

ROOT = guest.ROOT
# "Summit frames: 58.7/s over 1.0 s (painted=59 unchanged=0 dropped=0) update=1.10 ms
#  paint=4.20 ms flush=0.05 ms present=2.30 ms interval=17.00 ms (max 33.10)
#  longest frame=9.00 ms dirty=2.07 Mpx/frame [gl]"
FRAME_LINE = re.compile(
    r'Summit frames: (?P<fps>[\d.]+)/s over (?P<seconds>[\d.]+) s '
    r'\(painted=(?P<painted>\d+) unchanged=(?P<unchanged>\d+) dropped=(?P<dropped>\d+)\) '
    r'update=(?P<update>[\d.]+) ms paint=(?P<paint>[\d.]+) ms flush=(?P<flush>[\d.]+) ms '
    r'present=(?P<present>[\d.]+) ms '
    r'interval=(?P<interval>[\d.]+) ms \(max (?P<maxInterval>[\d.]+)\) '
    r'longest frame=(?P<longestFrame>[\d.]+) ms dirty=(?P<dirty>[\d.]+) Mpx/frame(?P<gl> \[gl\])?')
UI_FRAME_LINE = re.compile(
    r'Summit UI frames: (?P<fps>[\d.]+)/s frames=(?P<frames>\d+) '
    r'longest=(?P<longest>[\d.]+) ms over33=(?P<over33>\d+)')


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


def capture_visible(path):
    for _ in range(2):
        guest.wake_display()
        time.sleep(0.5)
        guest.screenshot(path)
        with Image.open(path) as screenshot:
            if any(high > 10 for _, high in screenshot.convert('RGB').getextrema()):
                return
    raise RuntimeError('Display stayed blank after waking it; scroll capture is invalid')


def parse_frame_lines(text):
    samples = []
    for match in FRAME_LINE.finditer(text):
        sample = {name: float(value) for name, value in match.groupdict().items()
                  if name != 'gl' and value is not None}
        sample['gl'] = bool(match.group('gl'))
        samples.append(sample)
    return samples


def parse_ui_frame_lines(text):
    return [{name: float(value) for name, value in match.groupdict().items()}
            for match in UI_FRAME_LINE.finditer(text)]


def summarize_ui(samples):
    if not samples:
        return {}
    frames = sum(sample['frames'] for sample in samples)
    seconds = sum(sample['frames'] / sample['fps'] for sample in samples if sample['fps'])
    return {
        'source': 'native-view', 'periods': len(samples), 'frames': int(frames),
        'seconds': round(seconds, 2), 'fps': round(frames / seconds, 2) if seconds else 0,
        'worstIntervalMs': max(sample['longest'] for sample in samples),
        'over33Ms': int(sum(sample['over33'] for sample in samples)),
    }


def summarize(samples):
    if not samples:
        return {}
    weight = sum(sample['seconds'] for sample in samples)
    frames = sum(sample['painted'] + sample['unchanged'] + sample['dropped'] for sample in samples)
    painted = sum(sample['painted'] for sample in samples) or 1
    def mean(name, count):
        return round(sum(sample[name] * count(sample) for sample in samples) / max(1, sum(count(sample) for sample in samples)), 3)
    return {
        'periods': len(samples),
        'seconds': round(weight, 2),
        'fps': round(frames / weight, 2) if weight else 0,
        'paintedPerSecond': round(sum(sample['painted'] for sample in samples) / weight, 2) if weight else 0,
        'updateMs': mean('update', lambda s: s['painted'] + s['unchanged'] + s['dropped']),
        'paintMs': mean('paint', lambda s: s['painted']),
        'flushMs': mean('flush', lambda s: s['painted']),
        'presentMs': mean('present', lambda s: s['painted']),
        'intervalMs': mean('interval', lambda s: s['painted'] + s['unchanged'] + s['dropped']),
        'worstIntervalMs': round(max(sample['maxInterval'] for sample in samples), 2),
        'medianIntervalMs': round(statistics.median(sample['interval'] for sample in samples), 2),
        'longestFrameMs': round(max(sample['longestFrame'] for sample in samples), 2),
        'dirtyMegapixelsPerFrame': round(sum(sample['dirty'] * sample['painted'] for sample in samples) / painted, 3),
        'gl': any(sample['gl'] for sample in samples),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('url')
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--settle', type=float, default=25, help='seconds to let the page finish loading')
    parser.add_argument('--notches', type=int, default=600, help='wheel notches in the burst')
    parser.add_argument('--interval-ms', type=int, default=16, help='milliseconds between notches')
    parser.add_argument('--delta', type=float, default=3.0, help='wheel delta per notch')
    parser.add_argument('--stats-period', type=float, default=0, help='seconds per frame statistics line on an instrumented engine (default: off)')
    parser.add_argument('--ui-frame-stats', action='store_true', help='count coordinated frame messages delivered to the Summit view')
    parser.add_argument('--window', default='', metavar='L,T,R,B', help='browser window frame (default: full screen)')
    parser.add_argument('--keep-sidebar', action='store_true')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--label', default='')
    parser.add_argument('--keep-open', action='store_true')
    args = parser.parse_args()

    run_id = time.strftime('scroll-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    directory = ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    guest_dir = f'{guest.GUEST_ROOT}/runs/{run_id}'
    run = {'id': run_id, 'machine': guest.HOST, 'url': args.url, 'bundle': args.bundle,
           'notches': args.notches, 'intervalMs': args.interval_ms, 'delta': args.delta,
           'directory': str(directory), 'startedAt': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
           'outcome': 'not-started', 'events': []}

    def save():
        (directory / 'run.json').write_text(json.dumps(run, indent=1))

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    ctl = guest.ensure_ctl()
    guest.require_volume()
    guest.wait_for_free_gui(ctl, args.bundle, 0, log)
    run['guestFacts'] = guest.guest_facts(args.bundle)
    save()

    extra_env = dict(item.split('=', 1) for item in args.env)
    if args.stats_period > 0:
        extra_env.setdefault('SUMMIT_FRAME_STATS', str(args.stats_period))
    if args.ui_frame_stats:
        extra_env['SUMMIT_UI_FRAME_STATS'] = '1'
    extra_env['SUMMIT_ENABLE_INPUT_SYNTHESIS'] = '1'
    run['extraEnv'] = extra_env
    remote_log = f'{guest_dir}/browser.log'
    team = group = None
    try:
        group = guest.launch(args.bundle, f'{guest_dir}/profile', 'summit:home', remote_log, extra_env)
        run['group'] = group
        save()
        team = guest.find_browser(ctl, group)
        run['team'] = team
        if args.window:
            guest.ctl(ctl, team, 'frame', *(int(v) for v in args.window.split(',')))
        if not args.keep_sidebar:
            guest.ctl(ctl, team, 'sidebar')
        time.sleep(3)
        guest.wake_display()
        log(f'launched Summit team {team}; loading {args.url}')
        guest.ctl(ctl, team, 'navigate', args.url, timeout_ms=15000)
        remaining = args.settle
        while remaining > 0:
            interval = min(20, remaining)
            time.sleep(interval)
            remaining -= interval
            guest.wake_display()
        run['stateBeforeBurst'] = guest.state(ctl, team)
        capture_visible(directory / 'before.png')
        guest.fetch_file(remote_log, directory / 'before.log')
        before = (directory / 'before.log').read_text('utf-8', 'replace')
        engine_before = parse_frame_lines(before)
        ui_before = parse_ui_frame_lines(before)
        run['idle'] = summarize(engine_before[-5:]) if engine_before else summarize_ui(ui_before[-5:])
        log(f"idle: {run['idle'].get('fps', 0)} fps")

        seconds = args.notches * args.interval_ms / 1000
        log(f'scrolling: {args.notches} notches over about {seconds:.0f} s')
        burst_started = time.monotonic()
        code, out, err = guest.ctl(ctl, team, 'scroll', args.notches, args.interval_ms, args.delta,
                                   timeout_ms=15000)
        if code:
            run['outcome'] = 'scroll-refused'
            run['scrollError'] = err.strip() or out.strip()
            raise RuntimeError(run['scrollError'])
        deadline = burst_started + max(30, seconds * 10)
        while True:
            progress = guest.state(ctl, team)
            if not progress.get('ok'):
                raise RuntimeError('Cannot read scroll progress: ' + progress.get('error', 'unknown error'))
            if progress.get('scrollRequested') != args.notches:
                # Older browser bundles do not expose completion state.
                time.sleep(seconds + 3)
                run['completionVerified'] = False
                break
            if not progress['scrollActive']:
                run['completionVerified'] = True
                run['scrollDelivery'] = {key: progress[key] for key in
                    ('scrollRequested', 'scrollSent', 'scrollStatus', 'scrollDurationMicros')}
                if progress['scrollSent'] != args.notches or progress['scrollStatus'] != 0:
                    run['outcome'] = 'scroll-incomplete'
                    raise RuntimeError(f"Delivered {progress['scrollSent']} of {args.notches} wheel notches "
                                       f"(status {progress['scrollStatus']})")
                break
            if time.monotonic() >= deadline:
                run['outcome'] = 'scroll-timeout'
                raise RuntimeError('Wheel burst did not complete before the measurement deadline')
            time.sleep(0.2)
        run['burstSeconds'] = round(time.monotonic() - burst_started, 1)
        # Let the one-second frame counter flush its last partial window.
        time.sleep(1.1)
        capture_visible(directory / 'final.png')
        guest.fetch_file(remote_log, directory / 'browser.log')
        text = (directory / 'browser.log').read_text('utf-8', 'replace')
        engine_samples = parse_frame_lines(text)[len(engine_before):]
        ui_samples = parse_ui_frame_lines(text)[len(ui_before):]
        during = engine_samples or ui_samples
        run['frameStatsAvailable'] = bool(during)
        if during:
            run['frameSamples'] = during
            run['scroll'] = summarize(engine_samples) if engine_samples else summarize_ui(ui_samples)
        run['stateAfterBurst'] = guest.state(ctl, team)
        run['outcome'] = ('completed' if run['completionVerified'] else 'captured-unverified-delivery') \
            if during else 'captured-uninstrumented'
    except KeyboardInterrupt:
        run['outcome'] = 'interrupted'
    except Exception as error:
        run.setdefault('outcome', 'failed')
        run['error'] = str(error)
        raise
    finally:
        if team and not args.keep_open:
            run['shutdown'] = guest.terminate(ctl, team, group=group)
        save()
        print(json.dumps({'id': run_id, 'outcome': run['outcome'],
                          'idle': run.get('idle'), 'scroll': run.get('scroll')}, indent=1))


if __name__ == '__main__':
    main()
