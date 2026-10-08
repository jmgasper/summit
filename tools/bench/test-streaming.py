#!/usr/bin/env python3
"""Run native MSE playback checks against an isolated frozen browser bundle."""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys
import time
import urllib.request

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('.vm/streaming-test'))
    parser.add_argument('--port', type=int, default=8777)
    parser.add_argument('--first-case', type=int, default=0)
    parser.add_argument('--limit', type=int)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    bundle = args.bundle.rstrip('/')
    ctl = guest.ensure_ctl()
    guest.wait_for_free_gui(ctl, bundle)
    remote = guest.GUEST_ROOT + '/streaming-test-' + str(time.time_ns())
    sources = ['tests/ModernStreamingTests.cpp', 'tests/ModernExtensionManagerTests.cpp',
               'tests/ModernCloseTests.cpp', 'src/ui/Messages.h', 'vendor/nlohmann/json.hpp']
    digest = hashlib.sha256(b'g++-std=c++23-O1-lbe' + b''.join((guest.ROOT / path).read_bytes() for path in sources)).hexdigest()[:20]
    binary = guest.GUEST_ROOT + '/bin/streaming-check-' + digest
    guest.ssh('mkdir -p ' + shlex.quote(remote) + ' ' + shlex.quote(guest.GUEST_ROOT + '/bin'))
    if guest.ssh('test -x ' + shlex.quote(binary), check=False).returncode:
        archive = subprocess.run(['tar', '-cf', '-', '-C', str(guest.ROOT), *sources],
                                 stdout=subprocess.PIPE, check=True).stdout
        guest.ssh('tar -xf - -C ' + shlex.quote(remote), input_bytes=archive)
        command = ['g++', '-std=c++23', '-O1', '-Wno-multichar', '-I' + remote + '/vendor',
                   '-I' + remote + '/src', remote + '/tests/ModernStreamingTests.cpp',
                   '-lbe', '-o', remote + '/check']
        compilation = guest.ssh(shlex.join(command), timeout=300, check=False)
        (args.output / 'compile.log').write_text(compilation.stdout + compilation.stderr)
        if compilation.returncode:
            raise guest.GuestError('streaming test compilation failed; see compile.log')
        guest.ssh('mv ' + shlex.quote(remote + '/check') + ' ' + shlex.quote(binary))
    else:
        (args.output / 'compile.log').write_text('Reused driver ' + digest + '\n')
    guest.ssh('ln -s ' + shlex.quote(binary) + ' ' + shlex.quote(remote + '/check'))
    result = {'bundle': bundle, 'remote': remote, 'loadBefore': guest.load_report(ctl)}
    watch = guest.CrashWatch()
    server = subprocess.Popen([sys.executable, str(guest.BENCH / 'streaming-fixture.py'),
                               '--directory', str(args.output / 'media'), '--port', str(args.port)],
                              stdout=(args.output / 'server.log').open('w'), stderr=subprocess.STDOUT)
    team = driver = None
    try:
        deadline = time.monotonic() + 60
        while True:
            if server.poll() is not None:
                raise guest.GuestError('fixture server exited; see server.log')
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{args.port}/cases.json', timeout=1) as response:
                    cases = json.load(response)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise guest.GuestError('fixture server did not become ready')
                time.sleep(.25)
        end_case = min(len(cases), args.first_case + args.limit) if args.limit else len(cases)
        team = guest.launch(bundle, remote + '/profile', 'about:blank', remote + '/browser.log',
                            {'SUMMIT_ENABLE_INPUT_SYNTHESIS': '1', 'SUMMIT_MSE_TRACE': '1',
                             'SUMMIT_LIBRARY_PATH_PREFIX': '/boot/home/summit-mesa/prefix-20261002/lib',
                             '__EGL_VENDOR_LIBRARY_FILENAMES': '/boot/home/summit-mesa/prefix-20261002/data/glvnd/egl_vendor.d/50_mesa.json'})
        result['team'] = team
        launch = '''import subprocess,sys
remote,team,bundle,url,first,count=sys.argv[1:]
p=subprocess.Popen([remote+'/check',team,bundle+'/Summit',url,first,count],stdin=subprocess.DEVNULL,
 stdout=open(remote+'/native.log','wb'),stderr=subprocess.STDOUT,start_new_session=True)
print(p.pid)
'''
        launched = guest.ssh('python3.10 - ' + shlex.join([remote, str(team), bundle,
                            f'http://{guest.HOST_ADDRESS}:{args.port}', str(args.first_case), str(end_case)]), input_bytes=launch.encode())
        driver = int(launched.stdout.strip().splitlines()[-1])
        result['driver'] = driver
        (args.output / 'active.json').write_text(json.dumps(result, indent=2))
        deadline = time.monotonic() + 600
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
        server.terminate()
        server.wait(timeout=10)
        result['crashes'] = watch.poll()
        log = (args.output / 'native.log').read_text() if (args.output / 'native.log').exists() else ''
        result['passed'] = ('Streaming_RESULT PASS' in log and not result.get('members')
                            and not result.get('timedOut') and not result['crashes']['newReports'])
        if result['passed']:
            browser_log = (args.output / 'browser.log').read_text()
            loads = re.split(r'load player=0x[0-9a-f]+', browser_log)[1:]
            result['audioSeekChecks'] = []
            result['videoTimingChecks'] = []
            for index, segment in zip(range(args.first_case, end_case), loads):
                case = cases[index]
                if case.get('errorOnEnd') or case.get('detachOnEnd') or case.get('removeOnEnd'):
                    continue
                if case['video'] and any(codec in case['type'] for codec in ('av01', 'hvc1', 'hev1')):
                    generations = {}
                    for generation, pts in re.findall(r'video output codec=\S+ generation=(\d+) pts=(-?\d+)', segment):
                        generations.setdefault(generation, []).append(int(pts))
                    result['videoTimingChecks'].append({'case': index, 'frames': sum(map(len, generations.values())),
                        'passed': bool(generations) and all(len(values) > 1 and values == sorted(values)
                                                          and all(0 <= value < 10_000_000 for value in values)
                                                          for values in generations.values())})
                if case.get('mp4') and case['video']:
                    continue  # These fixtures have no audio track.
                starts = re.findall(r'audio first PCM codec=\S+ generation=\d+ pts=([\d.-]+)', segment)
                target = case.get('seek', (case.get('offset') or case.get('window', [0])[0] or case.get('start') or 0) + 2)
                actual = float(starts[-1]) if starts else None
                # Packet-timed audio must start at the exact requested PCM
                # sample. MP4's existing frame-granular path can start one
                # AC-3 frame before the requested time.
                tolerance = .04 if case.get('mp4') else .00005
                result['audioSeekChecks'].append({'case': index, 'expected': target, 'actual': actual,
                                                  'passed': actual is not None and abs(actual - target) < tolerance})
            result['passed'] = len(loads) == end_case - args.first_case and all(item['passed'] for item in result['audioSeekChecks'] + result['videoTimingChecks'])
        if result['passed'] and args.first_case <= 21 and end_case >= 24:
            browser_log = (args.output / 'browser.log').read_text()
            completed = {(int(decoded), int(queued)) for decoded, queued in re.findall(
                r'audio finished codec=Opus generation=\d+ decoded=(\d+) queued=(\d+)', browser_log)}
            # Four seconds of exact PCM, after the different packet sizes'
            # initial pre-skip and terminal discard padding. The 2.5 ms case
            # needs its wholly discarded first packet for decoder priming.
            result['opusPCM'] = {str(size): (size, 192000) in completed for size in (192120, 192240, 195840)}
            result['passed'] = all(result['opusPCM'].values())
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
