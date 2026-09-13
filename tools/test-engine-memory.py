#!/usr/bin/env python3
"""Exercise production shared-memory code in an isolated native build."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Release'
OUTPUT = ROOT / 'build-memory-tests'
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
    raise SystemExit('Configure the pinned native engine before running memory tests.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags = ['-I' + str(OUTPUT), '-iquote', str(ENGINE / 'Source/WebCore'), '-I' + str(BUILD / 'WebCore/PrivateHeaders'),
         '-I' + str(BUILD / 'JavaScriptCore/Headers'), '-I' + str(BUILD / 'JavaScriptCore/PrivateHeaders'),
         '-I' + str(ENGINE / 'Source/WebCore/PAL'),
         '-I' + str(ENGINE / 'Source/WebCore/platform'), *flags,
         '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
         '-DWEBCORE_EXPORT=', '-fdiagnostics-color=never']
objects = []
for source in [OUTPUT / 'SharedMemory.cpp', OUTPUT / 'SharedMemoryHaiku.cpp', ROOT / 'tests/EngineSharedMemoryTests.cpp']:
    target = OUTPUT / (source.stem + '.o')
    subprocess.run(['c++', *flags, '-c', str(source), '-o', str(target)], cwd=BUILD, check=True)
    objects.append(str(target))
subprocess.run(['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-L' + str(BUILD / 'lib'),
                '-lJavaScriptCore', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
                '-o', str(OUTPUT / 'run')], check=True)
subprocess.run([str(OUTPUT / 'run')], check=True, timeout=30)
