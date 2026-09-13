#!/usr/bin/env python3
"""Build focused tests with the configured native WebCore compiler flags."""
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Release'
OBJECT = 'Source/WebCore/CMakeFiles/WebCore.dir/platform/network/curl/CookieUtil.cpp.o'
OUTPUT = ROOT / 'build-engine-cookies'


def run(command, *, timeout=None):
    try:
        result = subprocess.run(command, cwd=BUILD, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit('Native cookie test did not exit within 30 seconds.')
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
    raise SystemExit('Configure the pinned Haiku WebCore build before running these tests.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags.append('-fdiagnostics-color=never')
OUTPUT.mkdir(exist_ok=True)
run(['c++', *flags, '-c', str(ROOT / 'tests/EngineCookieTests.cpp'), '-o', str(OUTPUT / 'main.o')])
run(['c++', str(OUTPUT / 'main.o'), str(BUILD / OBJECT), '-L' + str(BUILD / 'lib'),
     '-lJavaScriptCore', '-licuuc', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
     '-o', str(OUTPUT / 'run')])
for arguments in ([], ['--native-loop'], ['--main-thread-exit']):
    print('Lifecycle:', ' '.join(arguments) or 'console', flush=True)
    run([str(OUTPUT / 'run'), *arguments], timeout=30)
print('All cookie checks and three process shutdown cases passed.', flush=True)
