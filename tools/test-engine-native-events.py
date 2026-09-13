#!/usr/bin/env python3
"""Translate native Haiku messages through the actual WebKit and WebCore event classes."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Release'
OUTPUT = ROOT / 'build-native-event-tests'
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
    raise SystemExit('Build the pinned native legacy renderer before testing native event translation.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags = ['-I' + str(OUTPUT), '-iquote', str(OUTPUT), '-I' + str(ENGINE / 'Source/WebKit/Shared'),
         *flags, '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
         '-DWEBCORE_EXPORT=', '-fdiagnostics-color=never']
sources = [OUTPUT / name for name in ('WebEvent.cpp', 'WebKeyboardEvent.cpp', 'WebMouseEvent.cpp', 'WebWheelEvent.cpp',
           'WebEventConversion.cpp', 'NativeWebEventsHaiku.cpp', 'PlatformKeyboardEventHaiku.cpp')]
sources.append(ROOT / 'tests/EngineNativeEventTests.cpp')
objects = []
for source in sources:
    target = OUTPUT / (source.stem + '.o')
    if subprocess.run(['c++', *flags, '-c', str(source), '-o', str(target)], cwd=BUILD).returncode:
        raise SystemExit('Native compilation failed: ' + source.name)
    objects.append(str(target))
subprocess.run(['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-L' + str(BUILD / 'lib'),
                '-lWebKitLegacy', '-lJavaScriptCore', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
                '-o', str(OUTPUT / 'run')], check=True)
subprocess.run([str(OUTPUT / 'run')], timeout=30, check=True)
