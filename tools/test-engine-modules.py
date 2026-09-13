#!/usr/bin/env python3
"""Verify native module ownership using the production WebKit loader."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Modern')
OUTPUT = ROOT / 'build-module-tests'
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
    raise SystemExit('Configure modern WebKit and build JavaScriptCore before testing native modules.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
fixture = OUTPUT / 'module fixture 雪.so'
subprocess.run(['c++', '-shared', '-fPIC', str(ROOT / 'tests/EngineModuleFixture.cpp'), '-o', str(fixture)], check=True)
subprocess.run(['c++', *flags, '-I' + str(OUTPUT), '-fdiagnostics-color=never',
                str(OUTPUT / 'Module.cpp'), str(OUTPUT / 'ModuleHaiku.cpp'), str(ROOT / 'tests/EngineModuleTests.cpp'),
                '-L' + str(BUILD / 'lib'), '-lJavaScriptCore', '-lbe', '-lnetwork',
                '-Wl,-rpath,' + str(BUILD / 'lib'), '-o', str(OUTPUT / 'run')], cwd=BUILD, check=True)
subprocess.run([str(OUTPUT / 'run'), str(fixture)], timeout=20, check=True)
