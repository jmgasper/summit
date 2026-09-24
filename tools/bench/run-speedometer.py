#!/usr/bin/env python3
"""Run Speedometer 3.1 in Summit inside the Haiku test VM and record the result.

Local mode (default) serves the pristine checkout in .cache/speedometer-3.1 via
serve-speedometer.py and receives the complete result JSON from the page.
--official points the browser at browserbench.org instead; nothing can be
injected there, so completion is detected from the tab URL (the benchmark moves
to "#summary") and the score has to be read from final.png and recorded with
--annotate.

Everything for one run lands in .vm/bench/<run-id>/ (run.json is the index).
Only the browser instance launched by this script is ever signalled.

Examples:
  python3 tools/bench/run-speedometer.py                       # official 10 iterations
  python3 tools/bench/run-speedometer.py --iterations 3        # quick check
  python3 tools/bench/run-speedometer.py --suites Editor-TipTap --haiku-profile # VM only
  python3 tools/bench/run-speedometer.py --wait-quiet 120      # poll up to 2 h for an idle VM first
  python3 tools/bench/run-speedometer.py --official
  python3 tools/bench/run-speedometer.py --annotate .vm/bench/<run-id> --score 4.21 --ci 0.13
"""
import argparse
import json
import pathlib
import signal
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

ROOT = guest.ROOT
OFFICIAL_URL = 'https://browserbench.org/Speedometer3.1/'
MACHINE_CONFIG = {
    'vm': {
        'vcpus': 12, 'memoryGiB': 20, 'display': 'QEMU std VGA 1280x800, app_server software rendering (no GPU)',
        'hypervisor': 'QEMU/KVM on a 32-core host', 'guest': 'Haiku R1/beta6 x86_64',
    },
    'workstation': {
        'cpu': 'AMD Ryzen Threadripper 1950X (16 cores / 32 threads)', 'memoryGiB': 64,
        'display': 'GeForce GTX 1070 (GP104) 1920x1080, Zink on NVK for OpenGL',
        'hypervisor': 'bare metal', 'guest': 'Haiku R1/beta6 hrev60097 x86_64',
    },
}


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


def wait_for_quiet(ctl, minutes, needed=3):
    """Poll until `needed` consecutive 5 s samples show no foreign load; returns the history."""
    deadline = time.time() + minutes * 60
    history, streak = [], 0
    while True:
        report = guest.load_report(ctl, (), 5000)
        history.append(report)
        streak = 0 if report['contended'] else streak + 1
        if streak >= needed:
            log(f'VM is quiet ({needed} consecutive idle samples)')
            return True, history
        if time.time() > deadline:
            log('no quiet window before the deadline; running contended')
            return False, history
        if report['contended']:
            top = ', '.join(f"{t['args'].split('/')[-1][:30]}={t['cores']:.1f}" for t in report['topForeign'][:3])
            log(f"contended: foreign={report['foreignCores']:.2f} cores compilers={report['compilerTeams']} [{top}]")
            time.sleep(55)


def suite_table(result):
    metrics = result['metrics']
    rows = []
    for name, metric in metrics.items():
        if '/' in name or name.startswith('Iteration-') or name in ('Geomean', 'Score', 'Total'):
            continue
        rows.append((name, metric['mean'], metric.get('delta') or 0.0, metric.get('percentDelta') or 0.0,
                     metric['min'], metric['max']))
    return rows


def print_summary(run):
    result = run.get('result')
    print()
    print(f"run        : {run['id']}  ({run['mode']}, {run['iterations']} iteration(s))")
    print(f"bundle     : {run['bundle']}")
    print(f"outcome    : {run['outcome']}   wall {run.get('wallSeconds', 0):.0f} s")
    load = run['load']
    print(f"contention : {'CONTENDED' if run['contended'] else 'uncontended'}  "
          f"(foreign CPU cores: before {load['before']['foreignCores']:.2f}, "
          f"during max {load['duringMaxForeignCores']:.2f} / median {load['duringMedianForeignCores']:.2f}, "
          f"compiler sightings {load['compilerSightings']}/{load['samples']})")
    if result:
        score = result['score']
        print(f"score      : {score['mean']:.3f} ± {score.get('delta') or 0:.3f} "
              f"({score.get('percentDelta') or 0:.1f}%)   displayed: {result['displayed']['score']} {result['displayed']['confidence']}")
        print(f"per-iter   : {', '.join(f'{v:.2f}' for v in score['values'])}")
        total = sum(mean for _, mean, *_ in suite_table(result))
        environment = result['environment']
        print(f"geomean    : {result['geomean']['mean']:.1f} ms   sum of suite means: {total:.0f} ms/iteration")
        print(f"page       : viewport {environment['innerWidth']}x{environment['innerHeight']} "
              f"crossOriginIsolated={environment['crossOriginIsolated']} "
              f"hardwareConcurrency={environment['hardwareConcurrency']}")
        print()
        print(f"{'suite':44} {'mean ms':>10} {'± ms':>9} {'±%':>6} {'min':>9} {'max':>9}")
        for name, mean, delta, percent, low, high in suite_table(result):
            print(f'{name:44} {mean:10.1f} {delta:9.1f} {percent:6.1f} {low:9.1f} {high:9.1f}')
    elif run['mode'] == 'official':
        official = run.get('official', {})
        print(f"score      : {official.get('score', 'read it from final.png, then --annotate')} ± {official.get('ci', '?')}")
    print(f"artifacts  : {run['directory']}")


def annotate(args):
    directory = pathlib.Path(args.annotate).resolve()
    run = json.loads((directory / 'run.json').read_text())
    run['official'] = {'score': args.score, 'ci': args.ci, 'source': 'read from final.png by the operator',
                       'note': args.note}
    (directory / 'run.json').write_text(json.dumps(run, indent=1))
    print_summary(run)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bundle', default=guest.DEFAULT_BUNDLE, help='guest path of the Summit bundle')
    parser.add_argument('--iterations', type=int, default=10, help='Speedometer iterationCount (official default 10)')
    parser.add_argument('--suites', default='', help='comma-separated suite names (non-standard; for diagnosis only)')
    parser.add_argument('--official', action='store_true', help='use browserbench.org instead of the local copy')
    parser.add_argument('--port', type=int, default=8931)
    parser.add_argument('--timeout', type=int, default=5400, help='seconds to wait for the result')
    parser.add_argument('--wait-quiet', type=float, default=0, metavar='MINUTES',
                        help='poll up to this long for a period without foreign CPU load before starting')
    parser.add_argument('--wait-gui', type=float, default=0, metavar='MINUTES',
                        help='wait this long for other Summit instances to exit (Summit is single-launch)')
    parser.add_argument('--cache-policy', choices=['official', 'revalidate', 'no-store'], default='official')
    parser.add_argument('--source', type=pathlib.Path,
                        help='alternate local benchmark checkout (diagnostic only)')
    parser.add_argument('--progress-beacons', action='store_true',
                        help='per-test progress beacons (diagnosis of hangs; marks the result instrumented)')
    parser.add_argument('--intersection-trace', action='store_true',
                        help='time CodeMirror IntersectionObserver callbacks (diagnostic; marks the result instrumented)')
    parser.add_argument('--intersection-defer-gap', action='store_true',
                        help='deliver CodeMirror gap callbacks in the next animation frame (diagnostic only)')
    parser.add_argument('--raf-phase-trace', action='store_true',
                        help='record rAF and timer phases per test (diagnostic only)')
    parser.add_argument('--haiku-profile', action='store_true',
                        help='run Summit under Haiku\'s inclusive sampling profiler and save profile.txt')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='extra browser environment')
    parser.add_argument('--window', default='4,1,1270,797' if guest.IS_VM else '4,1,1916,1076',
                        metavar='L,T,R,B',
                        help='browser window frame in screen coordinates (default fills the desktop)')
    parser.add_argument('--keep-sidebar', action='store_true', help='leave the Summit sidebar open (narrower viewport)')
    parser.add_argument('--label', default='')
    parser.add_argument('--keep-open', action='store_true', help='leave the browser running afterwards')
    parser.add_argument('--annotate', metavar='RUN_DIR', help='record a score read from an --official screenshot')
    parser.add_argument('--score', type=float)
    parser.add_argument('--ci', type=float)
    parser.add_argument('--note', default='')
    args = parser.parse_args()
    if args.annotate:
        return annotate(args)
    if args.official and args.intersection_trace:
        parser.error('--intersection-trace requires the local Speedometer copy')
    if args.intersection_defer_gap and not args.intersection_trace:
        parser.error('--intersection-defer-gap requires --intersection-trace')
    if args.official and args.raf_phase_trace:
        parser.error('--raf-phase-trace requires the local Speedometer copy')
    if args.official and args.source:
        parser.error('--source requires the local Speedometer copy')
    if args.haiku_profile and guest.HOST == 'workstation':
        parser.error('--haiku-profile is disabled on the workstation because Haiku profile shutdown can hang the machine')
    if args.haiku_profile and args.keep_open:
        parser.error('--haiku-profile cannot be combined with --keep-open because the profile is written at exit')

    run_id = time.strftime('speedometer-%Y%m%d-%H%M%S') + ('-official' if args.official else '') \
        + (f'-{args.label}' if args.label else '')
    directory = ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    guest_dir = f'{guest.GUEST_ROOT}/runs/{run_id}'
    query = 'startAutomatically=true'
    if args.iterations != 10:
        query += f'&iterationCount={args.iterations}'
    if args.suites:
        query += f'&suites={args.suites}'
    base = OFFICIAL_URL if args.official else f'http://{guest.HOST_ADDRESS}:{args.port}/'
    url = f'{base}?{query}'
    run = {'id': run_id, 'mode': 'official' if args.official else 'local', 'url': url, 'bundle': args.bundle,
           'iterations': args.iterations, 'suites': args.suites, 'directory': str(directory), 'machine': guest.HOST, 'vm': MACHINE_CONFIG[guest.HOST],
           'cachePolicy': None if args.official else args.cache_policy, 'progressBeacons': args.progress_beacons,
           'intersectionTrace': args.intersection_trace,
           'intersectionDeferGap': args.intersection_defer_gap,
           'rafPhaseTrace': args.raf_phase_trace,
           'source': str(args.source.resolve()) if args.source else None,
           'haikuProfile': args.haiku_profile,
           'startedAt': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'outcome': 'not-started', 'load': {}, 'events': []}

    def save():
        (directory / 'run.json').write_text(json.dumps(run, indent=1))

    def interrupted(signum, frame):  # make `kill <runner>` clean up the browser and server too
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    ctl = guest.ensure_ctl()
    guest.require_volume()
    guest.wait_for_free_gui(ctl, args.bundle, args.wait_gui, log)
    run['guestFacts'] = guest.guest_facts(args.bundle)
    quiet_history = []
    if args.wait_quiet:
        _, quiet_history = wait_for_quiet(ctl, args.wait_quiet)
    before = guest.load_report(ctl, (), 5000)
    run['load'] = {'waitHistory': quiet_history[-40:], 'before': before, 'during': []}
    log(f"load before: foreign={before['foreignCores']:.2f} cores, compilers={before['compilerTeams']}, "
        f"busy={before['busyCores']:.2f}/{before['cpuCount']}")

    server = None
    if not args.official:
        command = [sys.executable, str(guest.BENCH / 'serve-speedometer.py'), '--port', str(args.port),
                   '--bind', guest.SERVER_BIND,
                   '--out-dir', str(directory), '--cache-policy', args.cache_policy]
        if args.source:
            command.extend(['--source', str(args.source.resolve())])
        if args.progress_beacons:
            command.append('--progress-beacons')
        if args.intersection_trace:
            command.append('--intersection-trace')
        if args.intersection_defer_gap:
            command.append('--intersection-defer-gap')
        if args.raf_phase_trace:
            command.append('--raf-phase-trace')
        server = subprocess.Popen(command, stdout=(directory / 'server.log').open('w'), stderr=subprocess.STDOUT)
        time.sleep(1.0)
        if server.poll() is not None:
            sys.exit('server failed to start: ' + (directory / 'server.log').read_text())

    watch = guest.CrashWatch()
    guest.wake_display()  # a blanked screen would hide the window and skew painting costs
    team = group = None
    started = time.time()
    try:
        extra_env = dict(item.split('=', 1) for item in args.env)
        run['extraEnv'] = extra_env
        # Start on the built-in start page so the window can be sized before the
        # benchmark (which starts itself on load) is on screen. Speedometer asks for a
        # viewport of at least 850x650; the 1280x800 guest screen only just allows it.
        wrapper = ('profile', '-f', '-S', '-o', f'{guest_dir}/profile.txt') if args.haiku_profile else ()
        group = guest.launch(args.bundle, f'{guest_dir}/profile', 'summit:home', f'{guest_dir}/browser.log',
                             extra_env, wrapper)
        run['group'] = group
        run['outcome'] = 'starting'
        save()
        team = guest.find_browser(ctl, group)
        run['team'] = team
        left, top, right, bottom = (int(v) for v in args.window.split(','))
        guest.ctl(ctl, team, 'frame', left, top, right, bottom)
        if not args.keep_sidebar:
            guest.ctl(ctl, team, 'sidebar')
        time.sleep(3)
        started = time.time()
        guest.ctl(ctl, team, 'navigate', url)
        run['outcome'] = 'running'
        save()
        log(f'launched Summit team {team}: {url}')
        last_state_check = last_shot = 0.0
        unresponsive = 0
        summary_seen = None
        while True:
            time.sleep(15)
            elapsed = time.time() - started
            if not args.official and (directory / 'result.json').exists():
                run['outcome'] = 'completed'
                break
            if not args.official and (directory / 'error.json').exists():
                run['outcome'] = 'benchmark-error'
                break
            if elapsed > args.timeout:
                run['outcome'] = 'timeout'
                break
            if time.time() - last_shot >= 60:
                last_shot = time.time()
                try:
                    guest.wake_display()
                    guest.screenshot(directory / 'progress.png')
                except Exception as error:  # a missed progress frame is not fatal
                    run['events'].append({'at': elapsed, 'screenshotError': str(error)})
                processes = guest.members(ctl, group)
                load = guest.load_report(ctl, [pid for pid, _ in processes], 2000)
                load['elapsed'] = round(elapsed)
                run['load']['during'].append(load)
                roles = sorted(guest.role(name) for _, name in processes)
                if 'Summit' not in roles:
                    run['outcome'] = 'browser-exited'
                    break
                if 'WebProcess' not in roles or 'NetworkProcess' not in roles:
                    run['events'].append({'at': round(elapsed), 'missingHelper': roles})
                    run['outcome'] = 'helper-process-died'
                    break
                crash = watch.poll()
                if crash['newReports'] or crash['syslogEvents']:
                    run['events'].append({'at': round(elapsed), 'crash': crash})
                    run['outcome'] = 'crash-detected'
                    break
                save()
            if time.time() - last_state_check >= 60:
                # Answered by the UI process, not by the WebProcess running the benchmark.
                last_state_check = time.time()
                current = guest.state(ctl, team, timeout_ms=15000)
                problem = guest.contamination(current, base) if current.get('ok') else None
                if problem:
                    run['events'].append({'at': round(elapsed), 'contamination': problem})
                    run['outcome'] = 'contaminated'
                    break
                if not current.get('ok'):
                    unresponsive += 1
                    run['events'].append({'at': round(elapsed), 'stateError': current})
                    if unresponsive >= 3:
                        run['outcome'] = 'ui-unresponsive'
                        break
                else:
                    unresponsive = 0
                    tabs = current.get('tabs') or []
                    shown = next((t for t in tabs if t['id'] == current.get('selected')), tabs[0] if tabs else {})
                    if round(elapsed) % 300 < 60:
                        log(f"t+{elapsed:.0f}s url={shown.get('url', '?')[-40:]} loading={shown.get('loading')}")
                    # loadOutcome alone is not enough: it also reads "process-exited" after a
                    # harmless COOP process swap. The status line is only set for a dead page.
                    if (shown.get('loadOutcome') == 'process-exited'
                            and 'page process exited' in (current.get('status') or '')):
                        # The UI shows "The page process exited. Reload to try again."
                        run['events'].append({'at': round(elapsed), 'webProcessExited': shown,
                                              'status': current.get('status')})
                        run['outcome'] = 'web-process-exited'
                        break
                    if shown.get('loadError'):
                        run['events'].append({'at': round(elapsed), 'loadError': shown})
                        run['outcome'] = 'load-error'
                        break
                    if shown.get('url', '').endswith('#summary'):
                        if args.official:
                            run['outcome'] = 'completed'
                            break
                        summary_seen = summary_seen or time.time()
                        if time.time() - summary_seen > 90:  # page is done but never uploaded its JSON
                            run['outcome'] = 'completed-result-upload-failed'
                            break
        run['wallSeconds'] = time.time() - started
        log(f"finished waiting: {run['outcome']} after {run['wallSeconds']:.0f} s")
        time.sleep(4)  # let the gauge animation settle before the final frame
        try:
            guest.screenshot(directory / 'final.png')
        except Exception as error:
            run['events'].append({'finalScreenshotError': str(error)})
        if args.official and run['outcome'] == 'completed':
            guest.ctl(ctl, team, 'navigate', url + '#details')
            time.sleep(5)
            guest.screenshot(directory / 'details.png')
        run['finalState'] = guest.state(ctl, team, timeout_ms=15000)
        problem = guest.contamination(run['finalState'], base) if run['finalState'].get('ok') else None
        others = guest.foreign_browsers(ctl, args.bundle, group)
        if problem or others:
            run['events'].append({'contamination': problem, 'foreignBrowsers': others})
            run['outcome'] = 'contaminated'
    finally:
        run['crash'] = watch.poll()
        for name in run['crash']['newReports']:
            watch.fetch_report(name, directory / name)
        if run['outcome'] != 'completed':
            (directory / 'syslog-tail.txt').write_text(watch.syslog_tail(200))
        if team is not None:
            run['processesAtEnd'] = guest.members(ctl, group)
            if not args.keep_open:
                run['shutdown'] = guest.terminate(ctl, team, group=group)
            try:
                guest.fetch_file(f'{guest_dir}/browser.log', directory / 'browser.log', tail_bytes=4 * 1024 * 1024)
            except Exception as error:
                run['events'].append({'browserLogError': str(error)})
            if args.haiku_profile:
                try:
                    guest.fetch_file(f'{guest_dir}/profile.txt', directory / 'profile.txt')
                except Exception as error:
                    run['events'].append({'profileFetchError': str(error)})
            if not args.keep_open:
                guest.ssh(f'rm -rf {guest_dir}/profile', check=False, timeout=300)
        if server:
            server.terminate()
        after = guest.load_report(ctl, (), 5000)
        during = run['load'].get('during', [])
        foreign = [sample['foreignCores'] for sample in during] or [0.0]
        run['load'].update({
            'after': after, 'samples': len(during),
            'duringMaxForeignCores': max(foreign), 'duringMedianForeignCores': statistics.median(foreign),
            'compilerSightings': sum(1 for sample in during if sample['compilerTeams']),
        })
        run['contended'] = bool(before['contended'] or after['contended']
                                or any(sample['contended'] for sample in during))
        if (directory / 'result.json').exists():
            run['result'] = json.loads((directory / 'result.json').read_text())['payload']
        if (directory / 'error.json').exists():
            run['benchmarkError'] = json.loads((directory / 'error.json').read_text())['payload']
        if (directory / 'browser.log').exists():
            text = (directory / 'browser.log').read_text(errors='replace')
            run['browserLog'] = {'bytes': len(text), 'lines': text.count('\n'),
                                 'hardLinkErrors': text.count('Failed to create hard link')}
        save()
        print_summary(run)
    return 0 if run['outcome'] == 'completed' else 1


if __name__ == '__main__':
    sys.exit(main())
