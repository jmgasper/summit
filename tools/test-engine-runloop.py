#!/usr/bin/env python3
"""Verify the native event-loop implementation without changing a live engine build."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Release')
OUTPUT = ROOT / 'build-runloop-tests'
OBJECT = 'Source/WTF/wtf/CMakeFiles/WTF.dir/haiku/RunLoopHaiku.cpp.o'


def run(command, timeout=None):
    result = subprocess.run(command, cwd=BUILD, timeout=timeout)
    if result.returncode:
        raise SystemExit(result.returncode)


fields = {}
with (BUILD / 'build.ninja').open() as stream:
    for line in stream:
        if line.startswith('build ' + OBJECT + ':'):
            for line in stream:
                if not line.strip():
                    break
                key, _, value = line.strip().partition(' = ')
                fields[key] = value
            break
if not all(key in fields for key in ('DEFINES', 'INCLUDES', 'FLAGS')):
    raise SystemExit('Configure the pinned Haiku engine before running these tests.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags.append('-fdiagnostics-color=never')
OUTPUT.mkdir(exist_ok=True)
link = ['-L' + str(BUILD / 'lib'), '-lJavaScriptCore', '-lbe', '-lnetwork', '-licuuc',
        '-Wl,-rpath,' + str(BUILD / 'lib')]
run(['c++', *flags, '-c', str(ROOT / 'tests/EngineRunLoopTests.cpp'), '-o', str(OUTPUT / 'main.o')])
run(['c++', str(OUTPUT / 'main.o'), *link, '-o', str(OUTPUT / 'baseline')])
run(['c++', *flags, '-c', str(OUTPUT / 'RunLoopHaiku.cpp'), '-o', str(OUTPUT / 'RunLoopHaiku.o')])
run(['c++', *flags, '-c', str(OUTPUT / 'WTFProcess.cpp'), '-o', str(OUTPUT / 'WTFProcess.o')])
run(['c++', str(OUTPUT / 'main.o'), str(OUTPUT / 'RunLoopHaiku.o'), str(OUTPUT / 'WTFProcess.o'), *link, '-o', str(OUTPUT / 'run')])
for arguments in ([], ['--native']):
    print('Event-loop mode:', ' '.join(arguments) or 'console', flush=True)
    run([str(OUTPUT / 'run'), *arguments], timeout=15)
for iteration in range(10):
    result = subprocess.run([str(OUTPUT / 'run'), '--exit-with-worker'], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, timeout=15)
    if result.returncode or result.stdout != 'Explicit exit while worker thread finishes\n':
        raise SystemExit(f'Concurrent exit failed: {result.returncode}: {result.stdout}')
print('Ten explicit process exits with a finishing worker passed and flushed their output.', flush=True)
