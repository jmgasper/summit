#!/usr/bin/env python3
"""Measure alternating wheel bursts within an already loaded page.

Each burst gets its own native-view frame counter. Reversing direction keeps
the test away from an infinite feed's loading boundary and exposes stalls
that occur while revisiting content. Logs are split by burst after capture so
low-volume engine traces can be matched to the frame statistics.

Example:
  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-scroll-cycles.py \
      --bundle /boot/home/summit/build-modern-browser/bundle-k7atp8la \
      --env SUMMIT_PAGE_UPDATE_TRACE=3 --env SUMMIT_COMPOSITOR_TIMING_TRACE=1 \
      https://www.reddit.com/r/popular/
"""
import argparse
import json
import pathlib
import signal
import time

from PIL import Image

import guest


def frame_summary(snapshot):
    elapsed = snapshot['elapsedMicros'] - snapshot['pendingGapMicros']
    summary = {
        'frames': snapshot['frames'],
        'fpsToLastFrame': round(snapshot['frames'] * 1_000_000 / elapsed, 2) if elapsed > 0 else 0,
        'completedWorstGapMs': round(snapshot['longestGapMicros'] / 1000, 1),
        'completedOver33': snapshot['longGaps'],
        'queueMaxMs': round(snapshot['queueMaxMicros'] / 1000, 1),
        'queueOver33': snapshot['queueOver33'],
    }
    if snapshot.get('splitGapAvailable'):
        summary['firstFrameDelayMs'] = round(snapshot['firstFrameDelayMicros'] / 1000, 1)
        summary['longestInterframeGapMs'] = round(snapshot['longestInterframeGapMicros'] / 1000, 1)
    return summary


def capture_visible(path):
    for _ in range(2):
        guest.wake_display()
        time.sleep(0.5)
        guest.screenshot(path)
        with Image.open(path) as screenshot:
            if any(high > 10 for _, high in screenshot.convert('RGB').getextrema()):
                return
    raise RuntimeError('Display stayed blank after waking it; capture is invalid')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('url')
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE)
    parser.add_argument('--settle', type=float, default=25)
    parser.add_argument('--notches', type=int, default=80)
    parser.add_argument('--interval-ms', type=int, default=25)
    parser.add_argument('--delta', type=float, default=3)
    parser.add_argument('--cycles', type=int, default=2, help='down/up pairs')
    parser.add_argument('--tail', type=float, default=0.25, help='seconds to include smooth-scroll animation tail')
    parser.add_argument('--after-settle', type=float, default=3, help='seconds to wait before a second final capture')
    parser.add_argument('--window', default='0,0,1279,1017')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    parser.add_argument('--label', default='')
    args = parser.parse_args()
    if args.notches < 1 or args.interval_ms < 1 or args.cycles < 1 or args.tail < 0 or args.after_settle < 0:
        parser.error('notches, interval, and cycles must be positive; tail and after-settle must be nonnegative')

    run_id = time.strftime('scroll-cycles-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    directory = guest.ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    remote = f'{guest.GUEST_ROOT}/runs/{run_id}'
    remote_log = f'{remote}/browser.log'
    environment = dict(item.split('=', 1) for item in args.env)
    environment.update(SUMMIT_UI_FRAME_STATS='1', SUMMIT_ENABLE_INPUT_SYNTHESIS='1')
    run = {
        'id': run_id, 'machine': guest.HOST, 'url': args.url, 'bundle': args.bundle,
        'notches': args.notches, 'intervalMs': args.interval_ms, 'delta': args.delta,
        'cycles': args.cycles, 'extraEnv': environment, 'bursts': [], 'outcome': 'not-started',
    }

    def save():
        (directory / 'run.json').write_text(json.dumps(run, indent=2) + '\n')

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    ctl = guest.ensure_ctl()
    guest.require_volume()
    guest.wait_for_free_gui(ctl, args.bundle)
    group = team = None
    try:
        group = guest.launch(args.bundle, f'{remote}/profile', 'summit:home', remote_log, environment)
        team = guest.find_browser(ctl, group)
        run['team'] = team
        code, out, err = guest.ctl(ctl, team, 'frame', *(int(v) for v in args.window.split(',')))
        if code:
            raise RuntimeError(err or out)
        guest.ctl(ctl, team, 'sidebar')
        time.sleep(3)
        code, out, err = guest.ctl(ctl, team, 'navigate', args.url, timeout_ms=15000)
        if code:
            raise RuntimeError(err or out)
        time.sleep(args.settle)
        run['stateBefore'] = guest.state(ctl, team)
        capture_visible(directory / 'before.png')
        time.sleep(1)
        guest.fetch_file(remote_log, directory / 'before.log')
        log_offset = (directory / 'before.log').stat().st_size
        run['outcome'] = 'running'
        save()

        for index in range(args.cycles * 2):
            delta = args.delta if index % 2 == 0 else -args.delta
            print(f'[{time.strftime("%H:%M:%S")}] burst {index + 1}: {args.notches} notches, delta {delta}', flush=True)
            code, out, err = guest.ctl(ctl, team, 'scroll', args.notches, args.interval_ms, delta,
                                       timeout_ms=15000)
            if code:
                raise RuntimeError(err or out)
            deadline = time.monotonic() + max(30, args.notches * args.interval_ms / 100)
            while True:
                state = guest.state(ctl, team)
                if not state.get('ok'):
                    raise RuntimeError(f'Cannot read scroll progress: {state}')
                if state['scrollRequested'] == args.notches and not state['scrollActive']:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('Wheel burst timed out')
                time.sleep(0.1)
            delivery = {name: state[name] for name in
                        ('scrollRequested', 'scrollSent', 'scrollStatus', 'scrollDurationMicros')}
            if delivery['scrollSent'] != args.notches or delivery['scrollStatus'] != 0:
                raise RuntimeError(f'Incomplete wheel burst: {delivery}')
            code, out, err = guest.ctl(ctl, team, 'framestats', timeout_ms=1000)
            if code:
                raise RuntimeError(err or out)
            active_snapshot = json.loads(out)
            time.sleep(args.tail)
            code, out, err = guest.ctl(ctl, team, 'framestats', timeout_ms=1000)
            if code:
                raise RuntimeError(err or out)
            tail_snapshot = json.loads(out)
            guest.fetch_file(remote_log, directory / 'browser.log')
            log = (directory / 'browser.log').read_bytes()
            burst_log = log[log_offset:]
            (directory / f'burst-{index + 1}.log').write_bytes(burst_log)
            log_offset = len(log)
            record = {'index': index + 1, 'delta': delta, 'delivery': delivery,
                      'active': frame_summary(active_snapshot),
                      'animationTail': frame_summary(tail_snapshot),
                      'activeFrameSnapshot': active_snapshot,
                      'animationTailFrameSnapshot': tail_snapshot,
                      'logBytes': len(burst_log)}
            run['bursts'].append(record)
            save()
            print(json.dumps(record), flush=True)

        run['stateAfter'] = guest.state(ctl, team)
        capture_visible(directory / 'after.png')
        time.sleep(args.after_settle)
        if args.after_settle:
            capture_visible(directory / 'settled.png')
        run['outcome'] = 'completed'
    except KeyboardInterrupt:
        run['outcome'] = 'interrupted'
    except Exception as error:
        run['outcome'] = 'failed'
        run['error'] = str(error)
        raise
    finally:
        if team is not None:
            run['termination'] = guest.terminate(ctl, team, group=group)
        save()
        print(f'Artifacts: {directory}', flush=True)


if __name__ == '__main__':
    main()
