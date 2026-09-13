#!/usr/bin/env python3
"""Compare real native pre-Run initialization against a preserved RunLoop baseline.

Uses copied production units and frozen JavaScriptCore/ICU libraries. No shared
engine configuration, source synchronization, or Ninja invocation is performed.
"""
import argparse
import hashlib
import io
import json
import os
import pathlib
import shlex
import subprocess
import sys
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native():
    engine = pathlib.Path('/boot/home/summit-webkit')
    build = engine / 'WebKitBuild/Modern'
    frozen = pathlib.Path('/boot/home/summit/jsc-icu78-w8dm77f9/lib')
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if digest(ROOT / name) != expected:
            raise RuntimeError('Changed staged input: ' + name)
    libraries = [frozen / name for name in ('libJavaScriptCore.so.18.7.4',
        'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')]
    library_hashes = {str(path): digest(path) for path in libraries}
    if library_hashes[str(libraries[0])] != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
        raise RuntimeError('Expected frozen JavaScriptCore/ICU 78 bundle is unavailable')
    prefix = engine / 'Source/WTF/wtf/WTFPrefix.h'
    watched = [build / 'build.ninja', build / 'cmakeconfig.h',
               engine / '.summit-source-manifest.json', prefix]
    native_inputs = {str(path): digest(path) for path in watched}
    fields = {}
    target = 'Source/WTF/wtf/CMakeFiles/WTF.dir/haiku/RunLoopHaiku.cpp.o'
    with (build / 'build.ninja').open() as stream:
        for line in stream:
            if line.startswith('build ' + target + ':'):
                for line in stream:
                    if not line.strip():
                        break
                    key, _, value = line.strip().partition(' = ')
                    fields[key] = value
                break
    if not all(name in fields for name in ('DEFINES', 'INCLUDES', 'FLAGS')):
        raise RuntimeError('Configured native RunLoop compiler flags are unavailable')
    raw = iter(shlex.split(' '.join(fields[name] for name in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        elif not flag.startswith('-fdiagnostics-color='):
            flags.append(flag)
    flags += ['-include', str(prefix), '-fdiagnostics-color=never']
    environment = {**os.environ, 'LIBRARY_PATH': str(frozen) + ':/boot/system/lib'}
    environment.pop('DISABLE_ASLR', None)
    report = {'kind': 'native-production-runloop-prerun', 'inputs': inputs,
              'frozen_libraries': library_hashes, 'native_inputs': native_inputs,
              'native_engine_patch_sha256': json.loads((engine / '.summit-source-manifest.json').read_text())['patch_sha256'],
              'compile_commands': [], 'link_commands': [], 'runs': {}}

    def compile_unit(source, output):
        command = ['c++', *flags, '-c', str(ROOT / source), '-o', str(ROOT / output)]
        report['compile_commands'].append(command)
        subprocess.run(command, cwd=build, check=True)

    def link(objects, output):
        command = ['c++', *[str(ROOT / name) for name in objects],
                   '-Wl,--no-export-dynamic', '-Wl,--gc-sections', str(libraries[0]),
                   '-lbe', '-lnetwork', '-Wl,-rpath,' + str(frozen), '-o', str(ROOT / output)]
        report['link_commands'].append(command)
        subprocess.run(command, check=True)

    def execute(name, executable, arguments=()):
        result = subprocess.run([str(ROOT / executable), *arguments], env=environment,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        (ROOT / (name + '.log')).write_text(result.stdout)
        report['runs'][name] = {'exit_code': result.returncode, 'output': result.stdout,
                               'executable_sha256': digest(ROOT / executable)}
        print(name + ':\n' + result.stdout, flush=True)
        return result.returncode

    compile_unit('tests/EngineRunLoopPreRunTests.cpp', 'prerun-tests.o')
    compile_unit('tests/EngineRunLoopTests.cpp', 'existing-tests.o')
    compile_unit('WTFProcess.cpp', 'WTFProcess.o')
    for kind in ('baseline', 'fixed'):
        compile_unit('RunLoop-' + kind + '.cpp', kind + '.o')
        link(['prerun-tests.o', kind + '.o', 'WTFProcess.o'], kind)
        execute(kind, kind)
    link(['existing-tests.o', 'fixed.o', 'WTFProcess.o'], 'existing')
    execute('console', 'existing')
    execute('native-after-run', 'existing', ['--native'])
    for index in range(10):
        execute('worker-exit-' + str(index), 'existing', ['--exit-with-worker'])

    report['artifact_sha256'] = {path.name: digest(path) for path in ROOT.glob('*.o')}
    report['libraries_unchanged'] = all(digest(pathlib.Path(path)) == expected
                                      for path, expected in library_hashes.items())
    report['native_inputs_unchanged'] = all(digest(pathlib.Path(path)) == expected
                                          for path, expected in native_inputs.items())
    baseline = report['runs']['baseline']
    report['baseline_reproduced'] = (baseline['exit_code'] == 1
        and 'FAIL pre-Run dispatch executes once on the application thread' in baseline['output']
        and 'FAIL pre-Run timer fires once through BApplication::Run' in baseline['output'])
    report['passed'] = (report['baseline_reproduced']
        and all(run['exit_code'] == 0 for name, run in report['runs'].items() if name != 'baseline')
        and report['libraries_unchanged'] and report['native_inputs_unchanged'])
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


def host(baseline):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/runloop-prerun-inputs.XXXXXXXX',
                   check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/runloop-prerun-inputs.'):
        raise RuntimeError('Unexpected native staging path')
    output = pathlib.Path(tempfile.mkdtemp(prefix='runloop-prerun-inputs.', dir=ROOT / '.vm'))
    files = {
        'tests/EngineRunLoopPreRunTests.cpp': (ROOT / 'tests/EngineRunLoopPreRunTests.cpp').read_bytes(),
        'tests/EngineRunLoopTests.cpp': (ROOT / 'tests/EngineRunLoopTests.cpp').read_bytes(),
        'tools/test-engine-runloop-prerun.py': pathlib.Path(__file__).read_bytes(),
        'RunLoop-fixed.cpp': (ROOT / '.cache/WebKit/Source/WTF/wtf/haiku/RunLoopHaiku.cpp').read_bytes(),
        'WTFProcess.cpp': (ROOT / '.cache/WebKit/Source/WTF/wtf/WTFProcess.cpp').read_bytes(),
        'RunLoop-baseline.cpp': pathlib.Path(baseline).read_bytes() if baseline else remote(
            'cat /boot/home/summit-webkit/Source/WTF/wtf/haiku/RunLoopHaiku.cpp',
            check=True, stdout=subprocess.PIPE).stdout,
    }
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files['inputs.json'] = (json.dumps(hashes, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as tar:
        for name, data in files.items():
            item = tarfile.TarInfo(name)
            item.size = len(data)
            tar.addfile(item, io.BytesIO(data))
            local = output / name
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(data)
    (output / 'stage.json').write_text(json.dumps({'stage': stage}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native stage: ' + stage + '\nHost evidence: ' + str(output), flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote('python3.10 ' + shlex.quote(stage + '/tools/test-engine-runloop-prerun.py')
                        + ' --native', text=True, stdout=log, stderr=subprocess.STDOUT)
    fetched = remote('cat ' + shlex.quote(stage + '/result.json'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if fetched.returncode == 0:
        (output / 'result.json').write_bytes(fetched.stdout)
    print((output / 'native.log').read_text(), end='')
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--baseline', help='Preserved production baseline; defaults to current native source')
    args = parser.parse_args()
    sys.exit(native() if args.native else host(args.baseline))
