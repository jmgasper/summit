#!/usr/bin/env python3
"""Build the actual Haiku IPC socket monitor against the native WTF library."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Release')
OUTPUT = ROOT / 'build-socket-monitor-tests'
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
if not all(key in fields for key in ('DEFINES', 'INCLUDES', 'FLAGS')):
    raise SystemExit('Configure the pinned Haiku engine before running these tests.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
subprocess.run(['c++', *flags, '-fdiagnostics-color=never', '-I' + str(OUTPUT),
    str(ROOT / 'tests/EngineSocketMonitorTests.cpp'), str(OUTPUT / 'SocketMonitorHaiku.cpp'),
    '-L' + str(BUILD / 'lib'), '-lJavaScriptCore', '-lbe', '-lnetwork', '-licuuc',
    '-Wl,-rpath,' + str(BUILD / 'lib'), '-o', str(OUTPUT / 'run')], cwd=BUILD, check=True)
subprocess.run([str(OUTPUT / 'run')], timeout=20, check=True)
