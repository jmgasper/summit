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
    r'longest=(?P<longest>[\d.]+) ms over33=(?P<over33>\d+)'
    r'(?: queueMax=(?P<queueMax>[\d.]+) ms queueOver33=(?P<queueOver33>\d+))?')
FIXED_INLINE_LAYOUT_LINE = re.compile(
    r'Summit fixed inline layout: calls=(?P<calls>\d+) relayout=(?P<relayoutCalls>\d+) '
    r'renderers=(?P<renderers>\d+) eligible=(?P<eligible>\d+) reused=(?P<reused>\d+) '
    r'excluded=(?P<excludedFromNormalLayout>\d+) notBox=(?P<notBox>\d+) dirty=(?P<alreadyDirty>\d+) '
    r'dirtySelf=(?P<dirtySelf>\d+) dirtyNormalChild=(?P<dirtyNormalChild>\d+) '
    r'dirtyOutOfFlowChild=(?P<dirtyOutOfFlowChild>\d+) dirtySimplified=(?P<dirtySimplified>\d+) '
    r'dirtyOutOfFlowMovement=(?P<dirtyOutOfFlowMovement>\d+) '
    r'block=(?P<blockLevel>\d+) replaced=(?P<replaced>\d+) relative=(?P<relativeDimensions>\d+) '
    r'nonFixed=(?P<nonFixedWidth>\d+) percentPadding=(?P<percentagePadding>\d+)')
PAGE_UPDATE_LINE = re.compile(r'Summit page update: page=\S+ (?P<metrics>(?:[A-Za-z]+=[\d.]+ ?)+)')
PAGE_UPDATE_METRIC = re.compile(r'(?P<name>[A-Za-z]+)=(?P<value>[\d.]+)')
SCROLL_STEPS_LINE = re.compile(r'Summit scroll steps: document=\S+ (?P<metrics>(?:[A-Za-z]+=[\d.]+ ?)+)')
LAYOUT_PHASE_LINE = re.compile(r'Summit layout phases: context=\S+ (?P<metrics>(?:[A-Za-z]+=[\d.]+ ?)+)')
RENDERER_LAYOUT_LINE = re.compile(
    r'Summit renderer layout: renderer=\S+ type=(?P<rendererType>.+?) total=(?P<total>[\d.]+)')
MEDIA_LIFECYCLE_LINE = re.compile(
    r'Summit media lifecycle: player=\S+ operation=(?P<operation>[A-Za-z]+) '
    r'(?P<metrics>(?:[A-Za-z]+=[\d.]+ ?)+)')


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
    return [{name: float(value) for name, value in match.groupdict().items() if value is not None}
            for match in UI_FRAME_LINE.finditer(text)]


def parse_fixed_inline_layout_lines(text):
    return [{name: int(value) for name, value in match.groupdict().items()}
            for match in FIXED_INLINE_LAYOUT_LINE.finditer(text)]


def summarize_fixed_inline_layout(samples):
    if not samples:
        return {}
    total = {name: sum(sample[name] for sample in samples) for name in samples[0]}
    total['periods'] = len(samples)
    total['eligiblePercent'] = round(total['eligible'] * 100 / total['renderers'], 2) if total['renderers'] else 0
    return total


def parse_page_update_lines(text):
    return [
        {match.group('name'): float(match.group('value'))
         for match in PAGE_UPDATE_METRIC.finditer(line.group('metrics'))}
        for line in PAGE_UPDATE_LINE.finditer(text)
    ]


def summarize_page_updates(samples):
    if not samples:
        return {}
    phase_names = [name for name in samples[0] if name != 'total']
    phase_totals = {name: round(sum(sample.get(name, 0) for sample in samples), 1)
                    for name in phase_names}
    return {
        'slowUpdates': len(samples),
        'totalMs': round(sum(sample['total'] for sample in samples), 1),
        'worstMs': max(sample['total'] for sample in samples),
        'phaseTotalsMs': phase_totals,
        'topPhases': [
            {'phase': name, 'totalMs': total}
            for name, total in sorted(phase_totals.items(), key=lambda item: item[1], reverse=True)[:5]
        ],
    }


def parse_scroll_step_lines(text):
    return [
        {match.group('name'): float(match.group('value'))
         for match in PAGE_UPDATE_METRIC.finditer(line.group('metrics'))}
        for line in SCROLL_STEPS_LINE.finditer(text)
    ]


def summarize_scroll_steps(samples):
    if not samples:
        return {}
    duration_names = [name for name in samples[0]
                      if name not in ('total', 'targets', 'documentTargets', 'elementTargets',
                                      'scrollTargets', 'scrollendTargets', 'maxEventDispatch')]
    duration_totals = {name: round(sum(sample.get(name, 0) for sample in samples), 1)
                       for name in duration_names}
    return {
        'slowRuns': len(samples),
        'totalMs': round(sum(sample['total'] for sample in samples), 1),
        'worstMs': max(sample['total'] for sample in samples),
        'targets': int(sum(sample['targets'] for sample in samples)),
        'maximumEventDispatchMs': max(sample['maxEventDispatch'] for sample in samples),
        'phaseTotalsMs': duration_totals,
    }


def parse_layout_phase_lines(text):
    return [
        {match.group('name'): float(match.group('value'))
         for match in PAGE_UPDATE_METRIC.finditer(line.group('metrics'))}
        for line in LAYOUT_PHASE_LINE.finditer(text)
    ]


def summarize_layout_phases(samples):
    if not samples:
        return {}
    phase_names = ('pre', 'renderTree', 'viewSize', 'post')
    return {
        'slowPasses': len(samples),
        'totalMs': round(sum(sample['total'] for sample in samples), 1),
        'worstMs': max(sample['total'] for sample in samples),
        'phaseTotalsMs': {
            name: round(sum(sample.get(name, 0) for sample in samples), 1)
            for name in phase_names
        },
    }


def parse_renderer_layout_lines(text):
    return [
        {'rendererType': match.group('rendererType'), 'total': float(match.group('total'))}
        for match in RENDERER_LAYOUT_LINE.finditer(text)
    ]


def summarize_renderer_layouts(samples):
    by_type = {}
    for sample in samples:
        entry = by_type.setdefault(sample['rendererType'], {'calls': 0, 'inclusiveMs': 0, 'worstMs': 0})
        entry['calls'] += 1
        entry['inclusiveMs'] = round(entry['inclusiveMs'] + sample['total'], 1)
        entry['worstMs'] = max(entry['worstMs'], sample['total'])
    return {
        'slowCalls': len(samples),
        'byType': dict(sorted(by_type.items(), key=lambda item: item[1]['inclusiveMs'], reverse=True)),
    }


def parse_media_lifecycle_lines(text):
    return [
        {'operation': line.group('operation'), **{
            match.group('name'): float(match.group('value'))
            for match in PAGE_UPDATE_METRIC.finditer(line.group('metrics'))
        }}
        for line in MEDIA_LIFECYCLE_LINE.finditer(text)
    ]


def summarize_media_lifecycle(samples):
    result = {}
    for operation in sorted({sample['operation'] for sample in samples}):
        operation_samples = [sample for sample in samples if sample['operation'] == operation]
        phase_names = sorted({name for sample in operation_samples for name in sample
                              if name not in ('operation', 'total') and not name.startswith('had')})
        result[operation] = {
            'slowCalls': len(operation_samples),
            'totalMs': round(sum(sample['total'] for sample in operation_samples), 1),
            'worstMs': max(sample['total'] for sample in operation_samples),
            'phaseTotalsMs': {
                name: round(sum(sample.get(name, 0) for sample in operation_samples), 1)
                for name in phase_names
            },
        }
    return result


def summarize_ui(samples):
    if not samples:
        return {}
    frames = sum(sample['frames'] for sample in samples)
    seconds = sum(sample['frames'] / sample['fps'] for sample in samples if sample['fps'])
    summary = {
        'source': 'native-view', 'periods': len(samples), 'frames': int(frames),
        'seconds': round(seconds, 2), 'fps': round(frames / seconds, 2) if seconds else 0,
        'worstIntervalMs': max(sample['longest'] for sample in samples),
        'over33Ms': int(sum(sample['over33'] for sample in samples)),
    }
    queue_samples = [sample for sample in samples if 'queueMax' in sample]
    if queue_samples:
        summary['worstQueueDelayMs'] = max(sample['queueMax'] for sample in queue_samples)
        summary['queueOver33Ms'] = int(sum(sample['queueOver33'] for sample in queue_samples))
    return summary


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
        # Preserve idle samples before VNC capture, which can synchronously
        # hold the app_server and create a long native-view queue delay.
        guest.fetch_file(remote_log, directory / 'before.log')
        idle_text = (directory / 'before.log').read_text('utf-8', 'replace')
        idle_engine = parse_frame_lines(idle_text)
        idle_ui = parse_ui_frame_lines(idle_text)
        capture_visible(directory / 'before.png')
        # Let the screenshot's frame-stat window close, then use the resulting
        # log length as the scroll baseline so capture work is not attributed
        # to the wheel burst.
        time.sleep(1.1)
        guest.fetch_file(remote_log, directory / 'before.log')
        before = (directory / 'before.log').read_text('utf-8', 'replace')
        engine_before = parse_frame_lines(before)
        ui_before = parse_ui_frame_lines(before)
        fixed_inline_before = parse_fixed_inline_layout_lines(before)
        page_update_before = parse_page_update_lines(before)
        scroll_steps_before = parse_scroll_step_lines(before)
        layout_phases_before = parse_layout_phase_lines(before)
        renderer_layouts_before = parse_renderer_layout_lines(before)
        media_lifecycle_before = parse_media_lifecycle_lines(before)
        run['idle'] = summarize(idle_engine[-5:]) if idle_engine else summarize_ui(idle_ui[-5:])
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
        # Cut the measured log before taking the final VNC screenshot. Capture
        # can hold app_server long enough to resemble a page-update stall.
        guest.fetch_file(remote_log, directory / 'browser.log')
        text = (directory / 'browser.log').read_text('utf-8', 'replace')
        engine_samples = parse_frame_lines(text)[len(engine_before):]
        ui_samples = parse_ui_frame_lines(text)[len(ui_before):]
        fixed_inline_samples = parse_fixed_inline_layout_lines(text)[len(fixed_inline_before):]
        page_update_samples = parse_page_update_lines(text)[len(page_update_before):]
        scroll_step_samples = parse_scroll_step_lines(text)[len(scroll_steps_before):]
        layout_phase_samples = parse_layout_phase_lines(text)[len(layout_phases_before):]
        renderer_layout_samples = parse_renderer_layout_lines(text)[len(renderer_layouts_before):]
        media_lifecycle_samples = parse_media_lifecycle_lines(text)[len(media_lifecycle_before):]
        during = engine_samples or ui_samples
        run['frameStatsAvailable'] = bool(during)
        if during:
            run['frameSamples'] = during
            run['scroll'] = summarize(engine_samples) if engine_samples else summarize_ui(ui_samples)
        if fixed_inline_samples:
            run['fixedInlineLayoutSamples'] = fixed_inline_samples
            run['fixedInlineLayout'] = summarize_fixed_inline_layout(fixed_inline_samples)
        if page_update_samples:
            run['pageUpdateSamples'] = page_update_samples
            run['pageUpdates'] = summarize_page_updates(page_update_samples)
        if scroll_step_samples:
            run['scrollStepSamples'] = scroll_step_samples
            run['scrollSteps'] = summarize_scroll_steps(scroll_step_samples)
        if layout_phase_samples:
            run['layoutPhaseSamples'] = layout_phase_samples
            run['layoutPhases'] = summarize_layout_phases(layout_phase_samples)
        if renderer_layout_samples:
            run['rendererLayoutSamples'] = renderer_layout_samples
            run['rendererLayouts'] = summarize_renderer_layouts(renderer_layout_samples)
        if media_lifecycle_samples:
            run['mediaLifecycleSamples'] = media_lifecycle_samples
            run['mediaLifecycle'] = summarize_media_lifecycle(media_lifecycle_samples)
        capture_visible(directory / 'final.png')
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
                          'idle': run.get('idle'), 'scroll': run.get('scroll'),
                          'fixedInlineLayout': run.get('fixedInlineLayout'),
                          'pageUpdates': run.get('pageUpdates'),
                          'scrollSteps': run.get('scrollSteps'),
                          'layoutPhases': run.get('layoutPhases'),
                          'rendererLayouts': run.get('rendererLayouts'),
                          'mediaLifecycle': run.get('mediaLifecycle')}, indent=1))


if __name__ == '__main__':
    main()
