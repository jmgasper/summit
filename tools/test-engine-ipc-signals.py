#!/usr/bin/env python3
"""Build copied production Haiku semaphore/event code and run native IPC tests.

Uses frozen framework headers and an existing private JavaScriptCore bundle.
Never synchronizes engine sources, reconfigures, or invokes a shared build.
"""
import argparse
import hashlib
import io
import json
import pathlib
import shlex
import subprocess
import sys
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def native_main():
    stage = ROOT
    frozen = pathlib.Path('/boot/home/summit/build-context-compile-agent')
    inputs = json.loads((stage / 'inputs.json').read_text())
    if not all(hashlib.sha256((stage / name).read_bytes()).hexdigest() == digest for name, digest in inputs.items()):
        raise RuntimeError('Staged IPC test input changed')
    (stage / 'WebCore').mkdir()
    (stage / 'WebCore/SharedMemory.h').symlink_to(stage / 'SharedMemory.h')
    flags = json.loads((frozen / 'flags.json').read_text())
    flags = [flag.replace(str(frozen), str(stage))
             if flag == '-I' + str(frozen) or flag == str(frozen) else flag for flag in flags]
    flags += ['-I/boot/home/summit-webkit/Source/WebCore/platform', '-DRELEASE_WITHOUT_OPTIMIZATIONS', '-DWEBCORE_EXPORT=', '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections']
    selection = pathlib.Path('/boot/home/summit/jsc-icu78-latest.json')
    library = pathlib.Path(json.loads(selection.read_text())['bundle']) / 'lib'
    libraries = {str(library / name): hashlib.sha256((library / name).read_bytes()).hexdigest()
                 for name in ('libJavaScriptCore.so.18.7.4', 'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')}
    selection_hash = hashlib.sha256(selection.read_bytes()).hexdigest()
    report = {'stage': str(stage), 'inputs': inputs, 'library': str(library), 'commands': [], 'compile_exits': [], 'libraries': libraries, 'selection_manifest_sha256': selection_hash}
    objects = []
    sources = ('IPCSemaphoreHaiku.cpp', 'IPCEventHaiku.cpp', 'SharedMemory.cpp', 'SharedMemoryHaiku.cpp',
               'Encoder.cpp', 'Decoder.cpp', 'ArgumentCodersUnix.cpp', 'WebKitPlatformGeneratedSerializers.cpp',
               'tests/EngineIPCSignalTests.cpp')
    for name in sources:
        target = stage / (pathlib.Path(name).stem + '.o')
        command = ['c++', *flags, '-MD', '-MF', str(target) + '.d', '-c', str(stage / name), '-o', str(target)]
        print('Compiling ' + name, flush=True)
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['commands'].append(command)
        report['compile_exits'].append(result.returncode)
        (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        if result.returncode:
            return result.returncode
        objects.append(str(target))
    command = ['c++', *objects, '-Wl,--no-export-dynamic', '-Wl,--gc-sections',
               str(library / 'libJavaScriptCore.so.18.7.4'), '-lbe', '-lnetwork',
               '-Wl,-rpath,' + str(library), '-o', str(stage / 'EngineIPCSignalTests')]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    report['link_command'] = command
    report['link_exit'] = result.returncode
    (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        return result.returncode
    headers = set()
    for target in objects:
        dependency_text = pathlib.Path(target + '.d').read_text().replace('\\\n', '')
        headers.update(shlex.split(dependency_text.split(':', 1)[1]))
    report['compile_inputs'] = {name: hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest() for name in sorted(headers)}
    (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    result = subprocess.run([str(stage / 'EngineIPCSignalTests')], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=60)
    print(result.stdout, end='', flush=True)
    (stage / 'runtime.log').write_text(result.stdout)
    report['runtime_exit'] = result.returncode
    report['compile_inputs_unchanged'] = all(hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest() == digest for name, digest in report['compile_inputs'].items())
    report['libraries_unchanged'] = all(hashlib.sha256(pathlib.Path(name).read_bytes()).hexdigest() == digest for name, digest in libraries.items())
    report['selection_manifest_unchanged'] = hashlib.sha256(selection.read_bytes()).hexdigest() == selection_hash
    report['passed'] = result.returncode == 0 and 'Native IPC signal checks:' in result.stdout
    report['inputs_unchanged'] = all(hashlib.sha256((stage / name).read_bytes()).hexdigest() == digest
                                     for name, digest in inputs.items())
    (stage / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] and report['inputs_unchanged'] and report['libraries_unchanged'] and report['selection_manifest_unchanged'] and report['compile_inputs_unchanged'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    args = parser.parse_args()
    if args.native:
        return native_main()
    source = ROOT / '.cache/WebKit/Source'
    ipc = source / 'WebKit/Platform/IPC'
    generated = ROOT / '.vm/ipc-signal-generated'
    generated.mkdir(exist_ok=True)
    # The full WebKit input list supplies this complete type through other
    # serialization domains. Supply its real header in the isolated input set.
    dependencies = generated / 'IPCSignalDependencies.serialization.in'
    dependencies.write_text('webkit_platform_header: <wtf/UUID.h>\n')
    subprocess.run([sys.executable, str(source / 'WebKit/Scripts/generate-serializers.py'),
                    '--output-dir', str(generated), 'cpp', str(ipc / 'IPCSemaphore.serialization.in'),
                    str(ipc / 'IPCEvent.serialization.in'), str(ipc / 'MessageFlags.serialization.in'),
                    str(source / 'WebKit/Shared/WTFArgumentCoders.serialization.in'), str(dependencies)], check=True)
    files = {name: ipc / name for name in ('IPCSemaphore.h', 'IPCEvent.h', 'IPCSemaphore.serialization.in',
                                          'IPCEvent.serialization.in', 'MessageFlags.serialization.in', 'Encoder.cpp', 'Decoder.cpp')}
    files.update({name: ipc / 'haiku' / name for name in ('IPCSemaphoreHaiku.cpp', 'IPCSemaphoreSharedHaiku.h', 'IPCEventHaiku.cpp')})
    files['generator/generate-serializers.py'] = source / 'WebKit/Scripts/generate-serializers.py'
    files['generator/webkit/opaque_ipc_types.py'] = source / 'WebKit/Scripts/webkit/opaque_ipc_types.py'
    files['IPCSignalDependencies.serialization.in'] = dependencies
    files['WTFArgumentCoders.serialization.in'] = source / 'WebKit/Shared/WTFArgumentCoders.serialization.in'
    files['ArgumentCodersUnix.cpp'] = ipc / 'unix/ArgumentCodersUnix.cpp'
    files.update({name: source / 'WebCore/platform' / name for name in ('SharedMemory.h', 'SharedMemory.cpp')})
    files['SharedMemoryHaiku.cpp'] = source / 'WebCore/platform/haiku/SharedMemoryHaiku.cpp'
    files['HaikuProcess.h'] = source / 'WebKit/UIProcess/Launcher/haiku/HaikuProcess.h'
    files['WebKitPlatformGeneratedSerializers.cpp'] = generated / 'WebKitPlatformGeneratedSerializers.cpp'
    files['tests/EngineIPCSignalTests.cpp'] = ROOT / 'tests/EngineIPCSignalTests.cpp'
    files['tools/test-engine-ipc-signals.py'] = pathlib.Path(__file__)
    contents = {name: path.read_bytes() for name, path in files.items()}
    contents['inputs.json'] = json.dumps({name: hashlib.sha256(data).hexdigest() for name, data in contents.items()}, indent=2).encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in contents.items():
            entry = tarfile.TarInfo(name)
            entry.size = len(data)
            entry.mode = 0o600
            stream.addfile(entry, io.BytesIO(data))

    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)

    stage = remote('mktemp -d /boot/home/summit/ipc-signal-inputs.XXXXXXXX', check=True,
                   text=True, stdout=subprocess.PIPE).stdout.strip()
    if not stage.startswith('/boot/home/summit/ipc-signal-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native stage')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    evidence = ROOT / '.vm' / pathlib.Path(stage).name
    evidence.mkdir(exist_ok=False)
    print('Native IPC stage: ' + stage, flush=True)
    with (evidence / 'native.log').open('w') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/test-engine-ipc-signals.py', '--native']),
                        stdout=log, stderr=subprocess.STDOUT)
    print((evidence / 'native.log').read_text(), end='', flush=True)
    report = remote('cat ' + shlex.quote(stage + '/result.json'), stdout=subprocess.PIPE)
    if report.returncode == 0:
        (evidence / 'result.json').write_bytes(report.stdout)
    print('Host evidence: ' + str(evidence), flush=True)
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
