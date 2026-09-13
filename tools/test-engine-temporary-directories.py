#!/usr/bin/env python3
"""Compare the native temporary-directory helper with a preserved baseline.

Stages immutable inputs, compiles the complete production FileSystem.cpp,
and links the test against the frozen ICU 78 JavaScriptCore bundle. This does
not change the active engine build or use its changing library outputs.
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
    for relative, expected in inputs.items():
        if digest(ROOT / relative) != expected:
            raise RuntimeError('Changed staged input: ' + relative)
    libraries = [frozen / name for name in ('libJavaScriptCore.so.18.7.4',
        'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')]
    hashes = {str(path): digest(path) for path in libraries}
    if hashes[str(libraries[0])] != 'e8a83d73453da614dc1c73d1fe875ce753122a092bbf10e16ca1accedfd0f6ba':
        raise RuntimeError('The pinned frozen ICU 78 JavaScriptCore bundle is unavailable')
    watched = [build / 'build.ninja', build / 'cmakeconfig.h', engine / '.summit-source-manifest.json']
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
    raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        else:
            flags.append(flag)
    flags += ['-include', str(engine / 'Source/WTF/wtf/WTFPrefix.h'),
              '-ffunction-sections', '-fdata-sections', '-fdiagnostics-color=never']
    report = {'kind': 'native-production-temporary-directories', 'inputs': inputs,
              'frozen_libraries': hashes, 'native_inputs': native_inputs, 'runs': {}}
    test_object = ROOT / 'tests.o'
    subprocess.run(['c++', *flags, '-c', str(ROOT / 'tests/EngineTemporaryDirectoryTests.cpp'),
                    '-o', str(test_object)], cwd=build, check=True)
    environment = {**os.environ, 'LIBRARY_PATH': str(frozen) + ':/boot/system/lib'}
    environment.pop('DISABLE_ASLR', None)
    for kind in ('baseline', 'fixed'):
        source = ROOT / ('FileSystem-' + kind + '.cpp')
        obj = ROOT / (kind + '.o')
        executable = ROOT / kind
        compile_command = ['c++', *flags, '-c', str(source), '-o', str(obj)]
        subprocess.run(compile_command, cwd=build, check=True)
        link_command = ['c++', str(obj), str(test_object), '-Wl,--no-export-dynamic',
                        '-Wl,--gc-sections', str(libraries[0]), '-lbe', '-lnetwork',
                        '-Wl,-rpath,' + str(frozen), '-o', str(executable)]
        subprocess.run(link_command, check=True)
        result = subprocess.run([str(executable)], env=environment, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (ROOT / (kind + '.log')).write_text(result.stdout)
        report['runs'][kind] = {'source_sha256': digest(source), 'object_sha256': digest(obj),
            'executable_sha256': digest(executable), 'compile_command': compile_command,
            'link_command': link_command, 'exit_code': result.returncode, 'output': result.stdout}
        print(kind + ':\n' + result.stdout, flush=True)
    report['libraries_unchanged'] = all(digest(pathlib.Path(path)) == value for path, value in hashes.items())
    report['native_inputs_unchanged'] = all(digest(pathlib.Path(path)) == value for path, value in native_inputs.items())
    report['passed'] = (report['runs']['fixed']['exit_code'] == 0
                        and report['libraries_unchanged'] and report['native_inputs_unchanged'])
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


def host():
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/temporary-directory-inputs.XXXXXXXX',
                   check=True, text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/temporary-directory-inputs.'):
        raise RuntimeError('Unexpected native staging path')
    output = pathlib.Path(tempfile.mkdtemp(prefix='temporary-directory-inputs.', dir=ROOT / '.vm'))
    files = {
        'tests/EngineTemporaryDirectoryTests.cpp': (ROOT / 'tests/EngineTemporaryDirectoryTests.cpp').read_bytes(),
        'tools/test-engine-temporary-directories.py': pathlib.Path(__file__).read_bytes(),
        'FileSystem-fixed.cpp': (ROOT / '.cache/WebKit/Source/WTF/wtf/FileSystem.cpp').read_bytes(),
        'FileSystem-baseline.cpp': remote('cat /boot/home/summit-webkit/Source/WTF/wtf/FileSystem.cpp',
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
    (output / 'inputs.json').write_bytes(files['inputs.json'])
    (output / 'stage.json').write_text(json.dumps({'stage': stage}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native stage: ' + stage + '\nHost evidence: ' + str(output), flush=True)
    with (output / 'native.log').open('w') as log:
        result = remote('python3.10 ' + shlex.quote(stage + '/tools/test-engine-temporary-directories.py')
                        + ' --native', text=True, stdout=log, stderr=subprocess.STDOUT)
    fetched = remote('cat ' + shlex.quote(stage + '/result.json'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if fetched.returncode == 0:
        (output / 'result.json').write_bytes(fetched.stdout)
    print((output / 'native.log').read_text(), end='')
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    sys.exit(native() if args.native else host())
