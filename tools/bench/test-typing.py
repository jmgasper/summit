#!/usr/bin/env python3
"""Run native paste, caret, and frame-latency checks in an isolated WebKit view."""
import argparse
import json
import os
import pathlib
import shlex
import subprocess
import sys
import time

import guest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, help='frozen guest Summit bundle')
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('.vm/typing-test'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    bundle = args.bundle.rstrip('/')
    ctl = guest.ensure_ctl()
    remote = guest.GUEST_ROOT + '/typing-test-' + str(time.time_ns())
    sources = ['tests/ModernTypingTests.cpp', 'tests/ModernPDFTests.cpp',
               'tests/fixtures/typing.html', 'vendor/nlohmann/json.hpp']
    archive = subprocess.run(['tar', '-cf', '-', '-C', str(guest.ROOT), *sources],
                             stdout=subprocess.PIPE, check=True).stdout
    guest.ssh('mkdir -p ' + shlex.quote(remote) + ' && tar -xf - -C ' + shlex.quote(remote), input_bytes=archive)
    compile_command = ['g++', '-std=c++23', '-O1', '-I' + remote + '/vendor',
                       '-I' + bundle + '/source/include', remote + '/tests/ModernTypingTests.cpp',
                       '-L' + bundle + '/lib', '-lWebKit', '-lbe', '-lnetwork',
                       '-o', remote + '/check']
    compilation = guest.ssh(shlex.join(compile_command), timeout=300, check=False)
    (args.output / 'compile.log').write_text(compilation.stdout + compilation.stderr)
    if compilation.returncode:
        raise guest.GuestError('typing test compilation failed; see compile.log')
    watch = guest.CrashWatch()
    team = None
    result = {'bundle': bundle, 'remote': remote, 'loadBefore': guest.load_report(ctl)}
    launch = '''import os,subprocess,sys
remote,bundle=sys.argv[1:]
env=dict(os.environ,WEBKIT_EXEC_PATH=bundle,LIBRARY_PATH=bundle+'/lib:/boot/system/lib',SUMMIT_ENABLE_INPUT_SYNTHESIS='1')
mesa='/boot/home/summit-mesa/prefix-20261002'
if os.path.isdir(mesa):
 env['LIBRARY_PATH']=mesa+'/lib:'+env['LIBRARY_PATH']
 env['__EGL_VENDOR_LIBRARY_FILENAMES']=mesa+'/data/glvnd/egl_vendor.d/50_mesa.json'
p=subprocess.Popen([remote+'/check','file://'+remote+'/tests/fixtures/typing.html',bundle],env=env,stdin=subprocess.DEVNULL,stdout=open(remote+'/native.log','wb'),stderr=subprocess.STDOUT,start_new_session=True)
print(p.pid)
'''
    try:
        launched = guest.ssh('python3.10 - ' + shlex.join([remote, bundle]), input_bytes=launch.encode())
        team = int(launched.stdout.strip().splitlines()[-1])
        result['team'] = team
        (args.output / 'active.json').write_text(json.dumps(result, indent=2))
        deadline = time.monotonic() + 300
        while guest.alive(team) and time.monotonic() < deadline:
            time.sleep(1)
        if guest.alive(team):
            result['timedOut'] = True
    finally:
        if team is not None:
            if guest.alive(team):
                result['cleanup'] = guest.terminate(ctl, team)
            result['members'] = guest.members(ctl, team)
            guest.fetch_file(remote + '/native.log', args.output / 'native.log')
        result['crashes'] = watch.poll()
        log = (args.output / 'native.log').read_text() if (args.output / 'native.log').exists() else ''
        result['passed'] = ('Typing_RESULT PASS' in log and not result.get('members')
                            and not result.get('timedOut') and not result['crashes']['newReports'])
        (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
