#!/usr/bin/env python3
"""Test production native font reconstruction and downloaded-face lifetime."""
import pathlib
import shlex
import subprocess
import concurrent.futures
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Release'
OUTPUT = ROOT / 'build-font-tests'
(OUTPUT / 'WebCore').mkdir(exist_ok=True)
# Make a deterministic two-face collection from the checked-in upstream fonts.
# TTC table offsets are absolute within the collection, including shared tables.
font_sources = [ENGINE / 'Tools/DumpRenderTree/fonts/FontWithFeatures.ttf',
                ENGINE / 'Tools/DumpRenderTree/fonts/WebKitWeightWatcher100.ttf']
collection = bytearray(struct.pack('>4sII2I', b'ttcf', 0x10000, 2, 0, 0))
for index, source in enumerate(font_sources):
    font = bytearray(source.read_bytes())
    face_offset = len(collection)
    struct.pack_into('>I', collection, 12 + index * 4, face_offset)
    table_count = struct.unpack_from('>H', font, 4)[0]
    for table_index in range(table_count):
        offset_position = 12 + table_index * 16 + 8
        table_offset = struct.unpack_from('>I', font, offset_position)[0]
        struct.pack_into('>I', font, offset_position, table_offset + face_offset)
    collection.extend(font)
    collection.extend(b'\0' * (-len(collection) % 4))
(OUTPUT / 'collection.ttc').write_bytes(collection)
for name in ('FontPlatformData.h', 'FontCustomPlatformData.h'):
    forwarding_header = OUTPUT / 'WebCore' / name
    if not forwarding_header.is_symlink():
        forwarding_header.symlink_to('../' + name)
OBJECT = 'Source/WebCore/CMakeFiles/WebCore.dir/platform/graphics/haiku/FontPlatformDataHaiku.cpp.o'
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
    raise SystemExit('Build the pinned native legacy renderer before testing native fonts.')
flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
flags = ['-I' + str(OUTPUT), '-iquote', str(OUTPUT), '-iquote', str(ENGINE / 'Source/WebCore/platform/graphics/haiku'),
         *flags, '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden',
         '-DWEBCORE_EXPORT=', '-fdiagnostics-color=never']
def compile_source(source):
    target = OUTPUT / (source.stem + '.o')
    if subprocess.run(['c++', *flags, '-c', str(source), '-o', str(target)], cwd=BUILD).returncode:
        raise SystemExit('Native compilation failed: ' + source.name)
    return str(target)

with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
    objects = list(executor.map(compile_source, [OUTPUT / 'FontPlatformData.cpp',
        OUTPUT / 'FontPlatformDataHaiku.cpp', OUTPUT / 'FontCustomPlatformData.cpp',
        OUTPUT / 'OpenTypeUtilities.cpp',
        ROOT / 'tests/EngineFontTests.cpp']))
subprocess.run(['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-L' + str(BUILD / 'lib'),
                '-lWebKitLegacy', '-lJavaScriptCore', '-lbe', '-lnetwork', '-Wl,-rpath,' + str(BUILD / 'lib'),
                '-o', str(OUTPUT / 'run')], check=True)
subprocess.run([str(OUTPUT / 'run')], timeout=30, check=True)
