#!/usr/bin/env python3
"""Native focused test using an idle reference engine's generated headers/libs.

Only writes --output. --source supplies the actual amended engine source.
This does not rebuild or modify the reference engine.
"""
import argparse
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reference-build', required=True, type=Path)
parser.add_argument('--source', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
source = args.source.resolve() / 'Source/WebCore/platform/graphics/haiku'
build = args.reference_build.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
target = 'build Source/WebCore/CMakeFiles/WebCore.dir/platform/graphics/haiku/MediaDecoderSelectionHaiku.cpp.o:'
fields = {}
with (build / 'build.ninja').open() as f:
    for line in f:
        if line.startswith(target):
            for line in f:
                if not line.strip():
                    break
                key, _, value = line.strip().partition(' = ')
                fields[key] = value
            break
if not all(key in fields for key in ('DEFINES', 'INCLUDES', 'FLAGS')):
    raise SystemExit('Reference engine has no separate decoder compilation rule.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
# This standalone executable must use its own decoder definitions. It may
# share WTF/Media Kit with the reference engine, never the old decoder code.
flags += ['-I' + str(source), '-fno-profile-use', '-O1', '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden']
here = Path(__file__).resolve().parent
objects = []
for path in (source / 'MediaDecoderSelectionHaiku.cpp', here / 'track-decode.cpp'):
    obj = output / (path.stem + '.o')
    subprocess.run(['c++', *flags, '-c', str(path), '-o', str(obj)], cwd=build, check=True)
    objects.append(str(obj))
subprocess.run(['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-L' + str(build / 'lib'),
    '-lWebKit', '-lJavaScriptCore', '-lmedia', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(build / 'lib'),
    '-o', str(output / 'track-decode')], check=True)
subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-fPIC', '-shared',
    str(here / 'fault-addon.cpp'), '-lmedia', '-lbe', '-o', str(output / 'amduvd-fault')], check=True)
print('Built focused track test and TEST-ONLY fault add-on:', output)
