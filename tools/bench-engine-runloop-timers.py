#!/usr/bin/env python3
"""Compare run loop timer throughput: engine library versus working-tree RunLoopHaiku.cpp.

Runs inside the guest after tools/test-engine-runloop-in-vm.sh has staged the sources.
"""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Release')
OUTPUT = ROOT / 'build-runloop-tests'
OBJECT = 'Source/WTF/wtf/CMakeFiles/WTF.dir/haiku/RunLoopHaiku.cpp.o'

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
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
link = ['-L' + str(BUILD / 'lib'), '-lJavaScriptCore', '-lbe', '-lnetwork', '-licuuc', '-Wl,-rpath,' + str(BUILD / 'lib')]
bench = OUTPUT / 'bench.o'
subprocess.run(['c++', *flags, '-c', str(ROOT / 'tests/EngineRunLoopTimerBench.cpp'), '-o', str(bench)], cwd=BUILD, check=True)
subprocess.run(['c++', str(bench), *link, '-o', str(OUTPUT / 'bench-library')], cwd=BUILD, check=True)
subprocess.run(['c++', str(bench), str(OUTPUT / 'RunLoopHaiku.o'), str(OUTPUT / 'WTFProcess.o'), *link, '-o', str(OUTPUT / 'bench-working-tree')], cwd=BUILD, check=True)
for name in ('bench-library', 'bench-working-tree'):
    print('==', name, flush=True)
    subprocess.run([str(OUTPUT / name)], timeout=600)
