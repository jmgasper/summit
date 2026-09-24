#!/usr/bin/env python3
"""Run the same local Speedometer 3.1 in Firefox on the Haiku workstation.

This is the comparison baseline for Summit. It serves the pristine checkout in
.cache/speedometer-3.1 with serve-speedometer.py, which injects a hook that
posts the complete result back here, so the score is read from JSON rather than
from a screenshot. Firefox runs with a throwaway profile of its own.

  SUMMIT_BENCH_HOST=workstation python3 tools/bench/run-firefox-speedometer.py
"""
import argparse
import json
import pathlib
import shlex
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import guest  # noqa: E402

ROOT = guest.ROOT
FIREFOX = '/boot/system/apps/Firefox/Firefox'


def log(message):
    print(time.strftime('[%H:%M:%S] ') + message, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--iterations', type=int, default=10)
    parser.add_argument('--suites', default='', help='comma-separated suite names (diagnostic)')
    parser.add_argument('--intersection-trace', action='store_true',
                        help='time CodeMirror IntersectionObserver callbacks (diagnostic)')
    parser.add_argument('--raf-phase-trace', action='store_true',
                        help='record rAF and timer phases per test (diagnostic only)')
    parser.add_argument('--source', type=pathlib.Path,
                        help='alternate local benchmark checkout (diagnostic only)')
    parser.add_argument('--port', type=int, default=8932)
    parser.add_argument('--timeout', type=int, default=3600)
    parser.add_argument('--settle', type=float, default=25, help='seconds to let Firefox start before navigating')
    parser.add_argument('--label', default='')
    parser.add_argument('--keep-open', action='store_true')
    args = parser.parse_args()

    run_id = time.strftime('firefox-speedometer-%Y%m%d-%H%M%S') + (f'-{args.label}' if args.label else '')
    directory = ROOT / '.vm/bench' / run_id
    directory.mkdir(parents=True)
    guest_dir = f'{guest.GUEST_ROOT}/runs/{run_id}'
    query = 'startAutomatically=true' + ('' if args.iterations == 10 else f'&iterationCount={args.iterations}')
    if args.suites:
        query += f'&suites={args.suites}'
    url = f'http://{guest.HOST_ADDRESS}:{args.port}/?{query}'
    run = {'id': run_id, 'browser': 'firefox', 'machine': guest.HOST, 'url': url,
           'iterations': args.iterations, 'suites': args.suites,
           'intersectionTrace': args.intersection_trace, 'rafPhaseTrace': args.raf_phase_trace,
           'source': str(args.source.resolve()) if args.source else None,
           'directory': str(directory),
           'startedAt': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'outcome': 'not-started'}

    def save():
        (directory / 'run.json').write_text(json.dumps(run, indent=1))

    signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt))
    server_command = [sys.executable, str(guest.BENCH / 'serve-speedometer.py'), '--port', str(args.port),
                      '--bind', guest.SERVER_BIND, '--out-dir', str(directory), '--cache-policy', 'official']
    if args.source:
        server_command.extend(['--source', str(args.source.resolve())])
    if args.intersection_trace:
        server_command.append('--intersection-trace')
    if args.raf_phase_trace:
        server_command.append('--raf-phase-trace')
    server = subprocess.Popen(
        server_command,
        stdout=(directory / 'server.log').open('w'), stderr=subprocess.STDOUT)
    time.sleep(1.0)
    if server.poll() is not None:
        sys.exit('server failed to start: ' + (directory / 'server.log').read_text())

    profile = f'{guest_dir}/profile'
    started = time.time()
    try:
        guest.ssh(f'mkdir -p {shlex.quote(profile)} {shlex.quote(guest_dir)}')
        run['version'] = guest.ssh(f'{FIREFOX} --version', check=False).stdout.strip()
        log('firefox: ' + run['version'])
        command = (f'{FIREFOX} --no-remote --profile {shlex.quote(profile)} '
                   f'{shlex.quote(url)} > {shlex.quote(guest_dir + "/firefox.log")} 2>&1 &')
        guest.ssh(command)
        run['outcome'] = 'running'
        save()
        log(f'launched Firefox on {url}')
        while True:
            time.sleep(15)
            if (directory / 'result.json').exists():
                run['outcome'] = 'completed'
                break
            if (directory / 'error.json').exists():
                run['outcome'] = 'benchmark-error'
                break
            if time.time() - started > args.timeout:
                run['outcome'] = 'timeout'
                break
        guest.screenshot(directory / 'final.png')
        if (directory / 'result.json').exists():
            result = json.loads((directory / 'result.json').read_text())
            run['result'] = result
            payload = result.get('payload', result)
            score = payload.get('score') or payload.get('metrics', {}).get('Score', {})
            run['score'] = score.get('mean')
            run['ci'] = (score.get('delta') or 0)
    except KeyboardInterrupt:
        run['outcome'] = 'interrupted'
    finally:
        if not args.keep_open:
            guest.ssh(r'ps | /bin/grep "[F]irefox" | awk "{print \$(NF-3)}" | xargs -r kill -9 2>/dev/null; true',
                      check=False)
        server.terminate()
        save()
        print(json.dumps({'id': run_id, 'outcome': run['outcome'],
                          'score': run.get('score'), 'ci': run.get('ci')}, indent=1))


if __name__ == '__main__':
    main()
