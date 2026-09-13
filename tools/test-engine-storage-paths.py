#!/usr/bin/env python3
"""Build native storage-path tests without waiting for all of modern WebKit."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Modern')
OUTPUT = ROOT / 'build-storage-path-tests'
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
    raise SystemExit('Configure modern WebKit and build JavaScriptCore before testing storage paths.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
subprocess.run(['c++', *flags, '-I' + str(OUTPUT), '-fdiagnostics-color=never',
                str(OUTPUT / 'StoragePathsHaiku.cpp'), str(ROOT / 'tests/EngineStoragePathTests.cpp'),
                '-L' + str(BUILD / 'lib'), '-lJavaScriptCore', '-lbe', '-lnetwork',
                '-Wl,-rpath,' + str(BUILD / 'lib'), '-o', str(OUTPUT / 'run')], cwd=BUILD, check=True)
subprocess.run([str(OUTPUT / 'run')], timeout=20, check=True)
