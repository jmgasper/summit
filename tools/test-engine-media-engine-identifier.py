#!/usr/bin/env python3
"""Run the generated native media engine IPC validator with frozen WTF; no WebKit object ABI is used."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import re
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit')
FROZEN = Path('/boot/home/summit/jsc-icu78-w8dm77f9/lib')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native():
    from native_crash_log import NativeCrashLog
    output = ROOT / 'sources'
    build = ENGINE / 'WebKitBuild/Modern'
    watched = [build / 'build.ninja', build / 'cmakeconfig.h', ENGINE / '.summit-source-manifest.json',
               ENGINE / 'Source/WebKit/WebKitPrefix.h']
    snapshot = {str(path): digest(path) for path in watched}
    manifest = json.loads((output / 'source-manifest.json').read_text())
    libraries = [FROZEN / name for name in ('libJavaScriptCore.so.18.7.4', 'libicudata.so.78.3',
                                           'libicui18n.so.78.3', 'libicuuc.so.78.3')]
    hashes = {str(path): digest(path) for path in libraries}
    if hashes[str(libraries[0])] != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
        raise RuntimeError('Frozen WTF/JavaScriptCore dependency changed')
    if json.loads(watched[2].read_text())['patch_sha256'] != manifest['engine_patch_sha256']:
        raise RuntimeError('The staged source and native configured engine patches differ')
    for name, expected in manifest['files'].items():
        if digest(output / name) != expected['sha256']:
            raise RuntimeError('Staged source changed: ' + name)
    config = watched[1].read_text()
    (output / 'cmakeconfig.h').write_text(config)
    fields = {}
    target = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'
    with watched[0].open() as stream:
        for line in stream:
            if line.startswith('build ' + target + ':'):
                for line in stream:
                    if not line.strip():
                        break
                    key, _, value = line.strip().partition(' = ')
                    fields[key] = value
                break
    raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
            flags.append(flag)
    flags = ['-I' + str(output), '-iquote', str(output), *flags,
             '-include', str(watched[3]), '-fdiagnostics-color=never', '-fmax-errors=5']
    report = {'scope': 'native generated enum validator only; no IPC transport, media playback or WebKit runtime',
              'source_manifest': manifest, 'native_inputs': snapshot, 'frozen_libraries': hashes,
              'compile_results': [], 'linked_webcore_or_webkit': False}
    objects = []
    for name in ('EngineMediaEngineIdentifierTests.cpp',):
        obj = output / (Path(name).stem + '.o')
        command = ['c++', *flags, '-c', str(output / name), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['compile_results'].append({'source': name, 'exit': result.returncode, 'output': result.stdout})
        objects.append(obj)
    if any(unit['exit'] for unit in report['compile_results']):
        (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
        return 1
    executable = output / 'run'
    subprocess.run(['c++', *map(str, objects), str(libraries[0]), '-lbe', '-lnetwork',
                    '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-Wl,-rpath,' + str(FROZEN),
                    '-o', str(executable)], check=True)
    dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
    if 'libWebKit' in dynamic or 'libWebCore' in dynamic:
        raise RuntimeError('Media validator unexpectedly links WebCore or WebKit')
    report['objects'] = {path.name: digest(path) for path in objects}
    report['executable_sha256'] = digest(executable)
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    crash_log = NativeCrashLog()
    try:
        result = subprocess.run([str(executable)], env=environment, timeout=60, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['exit'] = result.returncode
        report['output'] = result.stdout
    except subprocess.TimeoutExpired:
        report['exit'] = None
        report['output'] = 'Native media validator test timed out and was terminated; failed run'
    report['native_crash_log'] = crash_log.finish()
    report['inputs_unchanged'] = all(digest(Path(path)) == expected for path, expected in snapshot.items())
    report['libraries_unchanged'] = all(digest(Path(path)) == expected for path, expected in hashes.items())
    report['sources_unchanged'] = all(digest(output / name) == expected['sha256'] for name, expected in manifest['files'].items())
    report['passed'] = (report['exit'] == 0 and report['native_crash_log']['passed'] and report['inputs_unchanged']
                        and report['libraries_unchanged'] and report['sources_unchanged'])
    (output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report['output'], end='', flush=True)
    return 0 if report['passed'] else 1


def host(overlay):
    cache = ROOT / '.cache/WebKit'
    relative = Path('Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in')
    schema = Path(overlay).resolve() / relative if overlay else cache / relative
    generator = cache / 'Source/WebKit/Scripts/generate-serializers.py'
    enum_header = cache / 'Source/WebCore/platform/graphics/MediaPlayerEnums.h'
    with tempfile.TemporaryDirectory(prefix='summit-media-validator-') as temporary:
        subprocess.run(['python3', str(generator), 'cpp', '--split-by-directory', str(schema),
                        '--output-dir', temporary], check=True)
        generated = (Path(temporary) / 'GeneratedSerializersShared.cpp').read_text()
    match = re.search(r'template<> bool isValidEnum<WebCore::MediaPlayerMediaEngineIdentifier>\(uint8_t value\)\n\{.*?\n\}', generated, re.S)
    if not match:
        raise RuntimeError('Expected generated validator specialization was not found')
    validator = ('// Exact specialization extracted from the production generator output.\nnamespace WTF {\n'
                 + match.group() + '\n}\n').encode()
    files = {'MediaPlayerEnums.h': (enum_header, enum_header.read_bytes()),
             'GeneratedMediaEngineValidator.h': ('generated validator specialization', validator),
             'WebCoreArgumentCoders.serialization.in': (schema, schema.read_bytes()),
             'generate-serializers.py': (generator, generator.read_bytes())}
    test = ROOT / 'tests/EngineMediaEngineIdentifierTests.cpp'
    files[test.name] = (test, test.read_bytes())
    manifest = {'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
                'generation': {'generator': str(generator), 'input': str(schema),
                               'full_output_sha256': hashlib.sha256(generated.encode()).hexdigest(),
                               'extraction': 'Exact MediaPlayerMediaEngineIdentifier specialization, wrapped in namespace WTF'},
                'files': {name: {'source': str(path), 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, (path, data) in files.items()}}
    contents = {'sources/' + name: data for name, (_, data) in files.items()}
    contents['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name in ('test-engine-media-engine-identifier.py', 'native_crash_log.py'):
        contents['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in contents.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            stream.addfile(info, io.BytesIO(data))
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/media-engine-identifier-tests.XXXXXXXX', check=True,
                   text=True, capture_output=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/media-engine-identifier-tests.') or '/' in stage.removeprefix('/boot/home/summit/'):
        raise RuntimeError('Unexpected native stage: ' + stage)
    output = ROOT / '.vm' / Path(stage).name
    output.mkdir(exist_ok=False)
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native media engine validator stage: ' + stage, flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-engine-media-engine-identifier.py', '--native']),
                        stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output / 'result.json'), 'passed': report['native'].get('passed', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--overlay', help='Candidate source root containing Source/')
    args = parser.parse_args()
    raise SystemExit(native() if args.native else host(args.overlay))
