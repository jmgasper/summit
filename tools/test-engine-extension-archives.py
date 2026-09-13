#!/usr/bin/env python3
"""Compile/run the production ZIP adapter with privately unpacked libzip."""
import hashlib
import json
import pathlib
import shlex
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Modern'
OUTPUT = ROOT / 'build-extension-archive-tests'
FROZEN = ROOT / 'jsc-icu78-w8dm77f9/lib'
ARCHIVE = ROOT / 'extension-archive-dependencies'
OBJECT = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'
PACKAGES = {'libzip-1.11.4-1-x86_64.hpkg': 'f22023b112ed362bce11fef7b3c36f995c9ec4c46e644ae889dabb916bd567ec',
    'libzip_devel-1.11.4-1-x86_64.hpkg': 'b90fefccc5b81c5947339a3367bf3d63943d550438f57acd8f0abd3b937e5b78'}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


OUTPUT.mkdir(exist_ok=True)
for name, expected in PACKAGES.items():
    if digest(ARCHIVE / name) != expected:
        raise SystemExit('Privately unpacked package provenance mismatch: ' + name)
headers = ARCHIVE / 'libzip-devel/develop/headers'
library = ARCHIVE / 'libzip/lib/libzip.so.5.5'
watched = [BUILD / 'build.ninja', BUILD / 'cmakeconfig.h', ENGINE / '.summit-source-manifest.json', ENGINE / 'Source/WebKit/WebKitPrefix.h']
native_inputs = {str(path): digest(path) for path in watched}
config = (BUILD / 'cmakeconfig.h').read_text()
if config.count('#define ENABLE_WK_WEB_EXTENSIONS 0') != 1:
    raise SystemExit('Expected the stable feature-disabled Modern configuration.')
(OUTPUT / 'cmakeconfig.h').write_text(config.replace('#define ENABLE_WK_WEB_EXTENSIONS 0', '#define ENABLE_WK_WEB_EXTENSIONS 1'))
dependencies = [FROZEN / name for name in ('libJavaScriptCore.so.18.7.4', 'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')]
dependencies += [library, headers / 'zip.h', headers / 'zipconf.h']
if digest(dependencies[0]) != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
    raise SystemExit('Frozen ICU78 JSC bundle changed.')
# Record the real system libraries used by the small private libzip package.
for name in ('libcrypto.so.3', 'libssl.so.3', 'libz.so.1', 'libroot.so'):
    path = pathlib.Path('/boot/system/lib') / name
    if path.exists():
        dependencies.append(path)
library_hashes = {str(path): digest(path) for path in dependencies}
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
raw_flags = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
flags = []
for flag in raw_flags:
    if flag == '-include':
        next(raw_flags)
    elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
        flags.append(flag)
flags = ['-I' + str(OUTPUT), '-I' + str(headers), '-iquote', str(OUTPUT), *flags,
    '-include', str(ENGINE / 'Source/WebKit/WebKitPrefix.h'), '-fdiagnostics-color=never', '-fmax-errors=5']
sources = [OUTPUT / 'WebExtensionArchiveHaiku.cpp', ROOT / 'tests/EngineExtensionArchiveTests.cpp']
source_hashes = {str(path): digest(path) for path in sources + [OUTPUT / 'WebExtensionArchiveHaiku.h', OUTPUT / 'WebExtensionResourcePathsHaiku.h']}
objects = []
for source in sources:
    obj = OUTPUT / (source.stem + '.o')
    print('COMPILE', source.name, flush=True)
    subprocess.run(['c++', *flags, '-c', str(source), '-o', str(obj)], cwd=BUILD, check=True)
    objects.append(obj)
executable = OUTPUT / 'run'
subprocess.run(['c++', *map(str, objects), '-Wl,--no-export-dynamic', '-Wl,--gc-sections', str(library),
    str(dependencies[0]), '-lbe', '-lnetwork', '-Wl,-rpath,' + str(library.parent),
    '-Wl,-rpath,' + str(FROZEN), '-o', str(executable)], check=True)
dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
if 'libWebCore' in dynamic or 'libWebKit' in dynamic:
    raise SystemExit('Archive tests unexpectedly depend on WebCore/WebKit.')
result = subprocess.run([str(executable), str(OUTPUT / 'fixtures')], timeout=60, text=True,
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
print(result.stdout, end='')
report = {'scope': 'production ZIP/XPI archive extraction and temporary-directory lifecycle; no WebExtension manifest/browser runtime',
    'native_engine_patch_sha256': json.loads((ENGINE / '.summit-source-manifest.json').read_text())['patch_sha256'],
    'repository_base_url': 'https://eu.hpkg.haiku-os.org/haikuports/r1beta6/x86_64/current',
    'packages': PACKAGES, 'source_sha256': source_hashes,
    'fixtures_sha256': {p.name: digest(p) for p in sorted((OUTPUT / 'fixtures').iterdir())},
    'object_sha256': {str(path): digest(path) for path in objects}, 'executable_sha256': digest(executable),
    'dependency_sha256': library_hashes, 'native_inputs': native_inputs,
    'dependencies_unchanged': all(digest(pathlib.Path(p)) == h for p, h in library_hashes.items()),
    'native_inputs_unchanged': all(digest(pathlib.Path(p)) == h for p, h in native_inputs.items()),
    'source_inputs_unchanged': all(digest(pathlib.Path(p)) == h for p, h in source_hashes.items()),
    'exit_code': result.returncode, 'output': result.stdout, 'linked_webcore_or_webkit': False}
(OUTPUT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
if not all(report[k] for k in ('dependencies_unchanged', 'native_inputs_unchanged', 'source_inputs_unchanged')):
    raise SystemExit('Probe input changed during testing.')
raise SystemExit(result.returncode)
