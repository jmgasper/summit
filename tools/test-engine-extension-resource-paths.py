#!/usr/bin/env python3
"""Execute the production resource resolver with frozen JSC/WTF, without WebCore."""
import hashlib
import json
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Modern'
OUTPUT = ROOT / 'build-extension-resource-path-tests'
FROZEN = ROOT / 'jsc-icu78-w8dm77f9/lib'
OBJECT = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


OUTPUT.mkdir(exist_ok=True)
dependencies = [FROZEN / name for name in ('libJavaScriptCore.so.18.7.4',
    'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')]
library_hashes = {str(path): digest(path) for path in dependencies}
if library_hashes[str(dependencies[0])] != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
    raise SystemExit('The expected pinned, frozen ICU 78 JavaScriptCore bundle is unavailable.')
watched = [BUILD / 'build.ninja', BUILD / 'cmakeconfig.h',
           ENGINE / '.summit-source-manifest.json', ENGINE / 'Source/WebKit/WebKitPrefix.h']
native_inputs = {str(path): digest(path) for path in watched}
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
    raise SystemExit('The configured native Modern WebKit compiler flags are missing.')
raw_flags = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
flags = []
for flag in raw_flags:
    if flag == '-include':
        next(raw_flags)
    elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
        flags.append(flag)
flags = ['-I' + str(OUTPUT), '-iquote', str(OUTPUT), *flags,
         '-include', str(ENGINE / 'Source/WebKit/WebKitPrefix.h'),
         '-fdiagnostics-color=never', '-fmax-errors=5']
source = ROOT / 'tests/EngineExtensionResourcePathTests.cpp'
header = OUTPUT / 'WebExtensionResourcePathsHaiku.h'
obj = OUTPUT / 'resource-tests.o'
subprocess.run(['c++', *flags, '-c', str(source), '-o', str(obj)], cwd=BUILD, check=True)
executable = OUTPUT / 'run'
subprocess.run(['c++', str(obj), '-Wl,--no-export-dynamic', '-Wl,--gc-sections',
                str(dependencies[0]), '-lbe', '-lnetwork', '-Wl,-rpath,' + str(FROZEN),
                '-o', str(executable)], check=True)
dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
if 'libWebCore' in dynamic or 'libWebKit' in dynamic:
    raise SystemExit('Resource resolver tests unexpectedly linked a WebCore/WebKit library.')
result = subprocess.run([str(executable)], timeout=30, text=True, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT)
print(result.stdout, end='')
report = {
    'scope': 'production native resource URL/canonical-path resolver, no manifest or browser runtime',
    'native_engine_patch_sha256': json.loads((ENGINE / '.summit-source-manifest.json').read_text())['patch_sha256'],
    'source_sha256': digest(source), 'header_sha256': digest(header),
    'object_sha256': digest(obj), 'executable_sha256': digest(executable),
    'frozen_libraries': library_hashes, 'native_inputs': native_inputs,
    'frozen_libraries_unchanged': all(digest(pathlib.Path(path)) == expected for path, expected in library_hashes.items()),
    'native_inputs_unchanged': all(digest(pathlib.Path(path)) == expected for path, expected in native_inputs.items()),
    'exit_code': result.returncode, 'output': result.stdout,
    'linked_webcore_or_webkit': False,
}
(OUTPUT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
if not report['frozen_libraries_unchanged'] or not report['native_inputs_unchanged']:
    raise SystemExit('A native build input or frozen dependency changed during testing.')
raise SystemExit(result.returncode)
