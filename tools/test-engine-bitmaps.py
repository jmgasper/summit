#!/usr/bin/env python3
"""Draw with the production shared-bitmap adapter against the built native renderer."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Release'
OUTPUT = ROOT / 'build-bitmap-tests'
(OUTPUT / 'WebCore').mkdir(exist_ok=True)
for name in ('ShareableBitmap.h', 'Icon.h'):
    forwarding_header = OUTPUT / 'WebCore' / name
    if not forwarding_header.is_symlink():
        forwarding_header.symlink_to('../' + name)
OBJECT = 'Source/WebCore/CMakeFiles/WebCore.dir/platform/graphics/haiku/ShareableBitmapHaiku.cpp.o'
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
    raise SystemExit('Build the pinned native legacy renderer before testing shared bitmap drawing.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags = ['-I' + str(OUTPUT), '-iquote', str(OUTPUT), '-iquote', str(ENGINE / 'Source/WebCore/platform/graphics/haiku'),
         *flags, '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
         '-DWEBCORE_EXPORT=', '-fdiagnostics-color=never']
objects = []
for source in [OUTPUT / 'ShareableBitmap.cpp', OUTPUT / 'ShareableBitmapHaiku.cpp', OUTPUT / 'GraphicsContextHaiku.cpp', OUTPUT / 'IconHaiku.cpp', ROOT / 'tests/EngineBitmapTests.cpp']:
    target = OUTPUT / (source.stem + '.o')
    if subprocess.run(['c++', *flags, '-c', str(source), '-o', str(target)], cwd=BUILD).returncode:
        raise SystemExit('Native compilation failed: ' + source.name)
    objects.append(str(target))
subprocess.run(['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-L' + str(BUILD / 'lib'),
                '-lWebKitLegacy', '-lJavaScriptCore', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
                '-o', str(OUTPUT / 'run')], check=True)
subprocess.run([str(OUTPUT / 'run')], timeout=30, check=True)
