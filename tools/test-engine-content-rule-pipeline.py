#!/usr/bin/env python3
"""Test the actual WebCore content-rule parser, compiler, caches and backend on Haiku."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit-extensions')
BUILD = ENGINE / 'WebKitBuild/Modern'
TEST = 'EngineContentRulePipelineTests.cpp'
DNR_TEST = 'EngineExtensionDNRRulesTests.cpp'
STORE_TEST = 'EngineContentRuleStoreTests.cpp'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def unchanged(snapshot):
    return all(digest(Path(path)) == expected for path, expected in snapshot.items())


def ninja_fields(predicate):
    selected = False
    fields = {}
    for line in (BUILD / 'build.ninja').read_text().splitlines():
        if line.startswith('build '):
            selected = predicate(line)
        elif selected:
            if not line.strip():
                return fields
            key, separator, value = line.strip().partition(' = ')
            if separator:
                fields[key] = value
    raise RuntimeError('Configured build command was not found')


def native(compile_only, run_only):
    output = ROOT / 'sources'
    report_path = output / 'results.json'
    source_manifest = json.loads((output / 'source-manifest.json').read_text())
    for name, record in source_manifest.get('helpers', {}).items():
        if digest(output / name) != record['sha256']:
            raise RuntimeError('Staged DNR input changed: ' + name)
    source = output / TEST
    if digest(source) != source_manifest['test_sha256']:
        raise RuntimeError('Staged test source changed')
    engine_manifest = ENGINE / '.summit-source-manifest.json'
    if json.loads(engine_manifest.read_text())['patch_sha256'] != source_manifest['engine_patch_sha256']:
        raise RuntimeError('Configured engine patch differs from the test source snapshot')
    config = (BUILD / 'cmakeconfig.h').read_text()
    if '#define ENABLE_WK_WEB_EXTENSIONS 1' not in config or '#define ENABLE_CONTENT_EXTENSIONS 1' not in config:
        raise RuntimeError('The real extension-enabled configuration is required')
    watched = [engine_manifest, BUILD / 'build.ninja', BUILD / 'cmakeconfig.h', ENGINE / 'Source/WebKit/WebKitPrefix.h']
    if run_only:
        report = json.loads(report_path.read_text())
        if not report.get('compiled') or not unchanged(report['native_inputs']) or not unchanged(report['headers']):
            raise RuntimeError('Compile inputs changed or the test did not compile')
        if digest(output / 'test.o') != report['object_sha256']:
            raise RuntimeError('Compiled test object changed')
    else:
        report = {'scope': 'actual WebCore rule-list JSON parser, compiler, URL caches, backend, action deserialization and in-memory ResourceRequest header mutation; no WebKit context, extension loader, IPC, persistent rule store or browser/network runtime',
                  'source_manifest': source_manifest, 'native_inputs': {str(path): digest(path) for path in watched},
                  'runtime_executed': False, 'passed': False, 'linked_webcore_archive': False, 'linked_webkit': False}
        if TEST == DNR_TEST:
            report['scope'] = 'native DNR translator through real WebCore parser, compiler and backend; no extension loader, persistent store, IPC, browser or network request'
        elif TEST == STORE_TEST:
            report['scope'] = 'actual WebKit persistent content-rule store and main-loop callbacks; no extension context, privileged IPC or browser/network request'
        fields = ninja_fields(lambda line: line.startswith('build Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o:'))
        raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
        flags = []
        for flag in raw:
            if flag == '-include':
                next(raw)
            elif not flag.startswith(('-O', '-fdiagnostics-color=', '-fmax-errors=')):
                flags.append(flag)
        command = ['c++', '-O1', *flags, '-include', str(watched[-1]), '-fdiagnostics-color=never', '-fmax-errors=5',
                   '-MMD', '-MF', str(output / 'test.d'), '-c', str(source), '-o', str(output / 'test.o')]
        result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['compile'] = {'command': command, 'exit': result.returncode, 'output': result.stdout}
        report['compiled'] = result.returncode == 0
        print(result.stdout, end='', flush=True)
        if not result.returncode:
            dependencies = (output / 'test.d').read_text().replace('\\\n', ' ').split(':', 1)[1]
            paths = {Path(name) if Path(name).is_absolute() else BUILD / name for name in shlex.split(dependencies)}
            report['headers'] = {str(path): digest(path) for path in sorted(paths)}
            report['object_sha256'] = digest(output / 'test.o')
        report_path.write_text(json.dumps(report, indent=2) + '\n')
        if result.returncode or compile_only:
            return result.returncode

    # Refuse to link an older archive while its source changes are still building.
    targets = ['WebKit', 'WebProcess', 'NetworkProcess'] if TEST == STORE_TEST else ['lib/libWebCore.a']
    ready = subprocess.run(['ninja', '-C', str(BUILD), '-n', *targets], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ready.returncode or 'ninja: no work to do.' not in ready.stdout:
        raise RuntimeError('Required engine targets are not fully rebuilt: ' + ready.stdout[-2000:])
    fields = ninja_fields(lambda line: ': CXX_SHARED_LIBRARY_LINKER__WebKit_' in line)
    libraries = shlex.split(fields['LINK_LIBRARIES'])
    if TEST == STORE_TEST:
        libraries = [str(BUILD / 'lib/libWebKit.so'), *[flag for flag in libraries if not flag.endswith('.a')]]
    paths = {Path(flag) if Path(flag).is_absolute() else BUILD / flag for flag in libraries if not flag.startswith('-')}
    report['libraries'] = {str(path): digest(path) for path in sorted(paths)}
    executable = output / 'run'
    command = ['c++', str(output / 'test.o'), '-Wl,--gc-sections', '-Wl,--disable-new-dtags', *libraries, '-o', str(executable)]
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report['link'] = {'command': command, 'exit': result.returncode, 'output': result.stdout}
    print(result.stdout, end='', flush=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        return result.returncode
    dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
    if TEST == STORE_TEST and 'libWebKit' not in dynamic:
        raise RuntimeError('Store test must link the actual WebKit library')
    if TEST != STORE_TEST and 'libWebKit' in dynamic:
        raise RuntimeError('Pipeline test unexpectedly links WebKit')
    report['linked_webcore_archive'] = TEST != STORE_TEST
    report['linked_webkit'] = TEST == STORE_TEST
    report['executable_sha256'] = digest(executable)
    report['dynamic_dependencies'] = dynamic
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    if TEST == STORE_TEST:
        environment['LIBRARY_PATH'] = ':'.join(map(str, (BUILD / 'lib', Path('/boot/home/summit-deps/icu78/lib'), Path('/boot/home/summit-deps/libzip-1.11.4/lib'), Path('/boot/system/lib'))))
    from native_crash_log import NativeCrashLog
    crash_log = NativeCrashLog()
    try:
        result = subprocess.run([str(executable)], env=environment, timeout=120, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['exit'], report['output'] = result.returncode, result.stdout
    except subprocess.TimeoutExpired:
        report['exit'], report['output'] = None, 'Native pipeline test timed out and was terminated; failed run'
    report['native_crash_log'] = crash_log.finish()
    report['runtime_executed'] = True
    report['inputs_unchanged'] = unchanged(report['native_inputs']) and unchanged(report['headers'])
    report['libraries_unchanged'] = unchanged(report['libraries'])
    report['passed'] = report['exit'] == 0 and report['native_crash_log']['passed'] and report['inputs_unchanged'] and report['libraries_unchanged']
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    print(report['output'], end='', flush=True)
    return 0 if report['passed'] else 1


def host(compile_only, resume, overlay=None):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    if resume:
        stage = resume.rstrip('/')
        if not stage.startswith('/boot/home/summit/content-rule-pipeline.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native stage')
        output = ROOT / '.vm' / Path(stage).name
    else:
        test = ROOT / 'tests' / TEST
        manifest = {'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
                    'test_source': str(test), 'test_sha256': digest(test)}
        files = {'sources/' + TEST: test.read_bytes()}
        if TEST == DNR_TEST:
            manifest['helpers'] = {}
            for stem in ('WebExtensionDeclarativeNetRequestRulesHaiku', 'WebExtensionDeclarativeNetRequestURLFilter'):
                for suffix in ('.h', '.cpp'):
                    name = stem + suffix
                    relative = Path('Source/WebKit/UIProcess/Extensions/haiku') / name
                    candidate = Path(overlay).resolve() / relative if overlay else None
                    path = candidate if candidate and candidate.is_file() else ROOT / '.cache/WebKit' / relative
                    files['sources/' + name] = path.read_bytes()
                    manifest['helpers'][name] = {'source': str(path), 'sha256': digest(path)}
        files['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
        for name in ('test-engine-content-rule-pipeline.py', 'native_crash_log.py'):
            files['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in files.items():
                info = tarfile.TarInfo(name)
                info.size = len(data)
                stream.addfile(info, io.BytesIO(data))
        stage = remote('mktemp -d /boot/home/summit/content-rule-pipeline.XXXXXXXX', check=True, text=True, capture_output=True).stdout.strip()
        if not stage.startswith('/boot/home/summit/content-rule-pipeline.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native stage')
        output = ROOT / '.vm' / Path(stage).name
        output.mkdir(exist_ok=False)
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native content-rule pipeline stage: ' + stage, flush=True)
    command = ['python3.10', stage + '/tools/test-engine-content-rule-pipeline.py', '--native']
    if TEST == DNR_TEST:
        command.append('--dnr')
    elif TEST == STORE_TEST:
        command.append('--store')
    if compile_only:
        command.append('--compile-only')
    if resume:
        command.append('--run-only')
    with (output / ('runtime.log' if resume else 'native.log')).open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode, 'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    (output / ('runtime-result.json' if resume else 'result.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output), 'compiled': report['native'].get('compiled', False), 'passed': report['native'].get('passed', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--resume', help='Resume a compiled native staging directory after WebCore has rebuilt')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--dnr', action='store_true', help='Run the native DNR translation pipeline fixture')
    mode.add_argument('--store', action='store_true', help='Run the persistent WebKit rule store fixture')
    parser.add_argument('--overlay', help='Candidate translator sources under Source/; requires --dnr')
    args = parser.parse_args()
    if args.overlay and not args.dnr:
        parser.error('--overlay requires --dnr')
    if args.dnr:
        TEST = DNR_TEST
    elif args.store:
        TEST = STORE_TEST
    if args.compile_only and (args.run_only or args.resume):
        parser.error('Compile-only and resume/run-only are mutually exclusive')
    raise SystemExit(native(args.compile_only, args.run_only) if args.native else host(args.compile_only, args.resume, args.overlay))
