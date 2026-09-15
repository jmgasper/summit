#!/usr/bin/env python3
"""Test the actual WebCore content-rule parser, compiler, caches and backend on Haiku."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit-extensions')
BUILD = ENGINE / 'WebKitBuild/Modern'
TEST = 'EngineContentRulePipelineTests.cpp'
DNR_TEST = 'EngineExtensionDNRRulesTests.cpp'
STORE_TEST = 'EngineContentRuleStoreTests.cpp'
EXTENSION_RUNTIME_TEST = 'EngineExtensionRuntimeTests.cpp'
IPC_VALIDATION_TEST = 'EngineIPCValidationTests.cpp'
COOKIE_OBSERVER_TEST = 'EngineCookieObserverRuntimeTests.cpp'
PROCESS_TESTS = (EXTENSION_RUNTIME_TEST, IPC_VALIDATION_TEST, COOKIE_OBSERVER_TEST)
EXTENSION_STARTUP = 'cold'
EXTENSION_FIXTURE = 'storage'
TRACE_IPC = False


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def unchanged(snapshot):
    return all(digest(Path(path)) == expected for path, expected in snapshot.items())


def run_extension_fixture(executable, output, environment, timeout=240):
    # The native process launcher uses fork/exec and inherits this isolated
    # process group. File output cannot hang waiting for an orphan's pipe.
    runtime_log = output / 'runtime.log'
    result = {'timeout': False, 'forced_group_cleanup': False}
    with runtime_log.open('w') as log:
        process = subprocess.Popen([str(executable)], env=environment, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        result['process_group'] = process.pid

        def group_members():
            # On Haiku killpg(group, 0) can keep succeeding after every live
            # team has exited. Inspect the kernel's live team list instead.
            lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
            if not lines or lines[0].strip() != 'Id':
                raise RuntimeError('Could not enumerate native live teams')
            members = []
            for line in lines[1:]:
                identifier = int(line.strip())
                try:
                    if os.getpgid(identifier) == process.pid:
                        members.append(identifier)
                except ProcessLookupError:
                    pass
            return members

        def group_exists():
            return bool(group_members())

        try:
            result['exit'] = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            result['timeout'] = True
            result['exit'] = None
        # Reap only this fixture's group. A leftover child makes the run fail,
        # even if the parent reported success; no other VM process is targeted.
        deadline = time.monotonic() + 1
        while group_exists() and process.poll() is not None and time.monotonic() < deadline:
            time.sleep(0.1)
        if group_exists():
            result['forced_group_cleanup'] = True
            for action in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(process.pid, action)
                except ProcessLookupError:
                    break
                deadline = time.monotonic() + 5
                while group_exists() and time.monotonic() < deadline:
                    process.poll()
                    time.sleep(0.1)
                if not group_exists():
                    break
        result['remaining_processes'] = group_members()
        result['group_drained'] = not result['remaining_processes']
        if process.poll() is None:
            raise RuntimeError('Fixture process did not exit after its group was terminated')
    result['output'] = runtime_log.read_text(errors='replace')
    return result


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
    full_engine = TEST in (STORE_TEST, *PROCESS_TESTS)
    output = ROOT / 'sources'
    report_path = output / 'results.json'
    source_manifest = json.loads((output / 'source-manifest.json').read_text())
    for name, record in source_manifest.get('helpers', {}).items():
        if digest(output / name) != record['sha256']:
            raise RuntimeError('Staged fixture input changed: ' + name)
    if TEST == EXTENSION_RUNTIME_TEST and source_manifest.get('extension_fixture', 'storage') != EXTENSION_FIXTURE:
        raise RuntimeError('Selected extension fixture differs from its staged package')
    source = output / TEST
    if digest(source) != source_manifest['test_sha256']:
        raise RuntimeError('Staged test source changed')
    engine_manifest = ENGINE / '.summit-source-manifest.json'
    if json.loads(engine_manifest.read_text())['patch_sha256'] != source_manifest['engine_patch_sha256']:
        raise RuntimeError('Configured engine patch differs from the test source snapshot')
    config = (BUILD / 'cmakeconfig.h').read_text()
    if '#define ENABLE_WK_WEB_EXTENSIONS 1' not in config or '#define ENABLE_CONTENT_EXTENSIONS 1' not in config:
        raise RuntimeError('The real extension-enabled configuration is required')
    prefix_header = ENGINE / 'Source/WebKit/WebKitPrefix.h'
    watched = [engine_manifest, BUILD / 'build.ninja', BUILD / 'cmakeconfig.h', prefix_header]
    fixture_inputs = [output / name for name in source_manifest.get('helpers', {})]
    watched.extend([source, output / 'source-manifest.json', *fixture_inputs])
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
        elif TEST == EXTENSION_RUNTIME_TEST:
            report['scope'] = ('actual native extension package, controller, background document, '
                               + ('cookie API and event delivery' if EXTENSION_FIXTURE == 'cookies' else 'storage bindings')
                               + ', test-message IPC and context reload; not full extension compatibility or browser UI installation')
        elif TEST == IPC_VALIDATION_TEST:
            report['scope'] = 'actual WebKit Connection endpoints over native sockets, malformed dispatch flags, main-loop rejection and continued valid traffic; both endpoints in one fixture process'
        elif TEST == COOKIE_OBSERVER_TEST:
            report['scope'] = 'actual API cookie store, NetworkProcess, committed cookie receipts and typed observer delivery after rapid unregister/register; no extension JavaScript or HTTP request'
        fields = ninja_fields(lambda line: line.startswith('build Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o:'))
        raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
        flags = []
        for flag in raw:
            if flag == '-include':
                next(raw)
            elif not flag.startswith(('-O', '-fdiagnostics-color=', '-fmax-errors=')):
                flags.append(flag)
        command = ['c++', '-O1', *flags, '-include', str(prefix_header), '-fdiagnostics-color=never', '-fmax-errors=5',
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
    targets = ['WebKit', 'WebProcess', 'NetworkProcess'] if full_engine else ['lib/libWebCore.a']
    ready = subprocess.run(['ninja', '-C', str(BUILD), '-n', *targets], text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if ready.returncode or 'ninja: no work to do.' not in ready.stdout:
        raise RuntimeError('Required engine targets are not fully rebuilt: ' + ready.stdout[-2000:])
    fields = ninja_fields(lambda line: ': CXX_SHARED_LIBRARY_LINKER__WebKit_' in line)
    libraries = shlex.split(fields['LINK_LIBRARIES'])
    if full_engine:
        libraries = [str(BUILD / 'lib/libWebKit.so'), *[flag for flag in libraries if not flag.endswith('.a')]]
    paths = {Path(flag) if Path(flag).is_absolute() else BUILD / flag for flag in libraries if not flag.startswith('-')}
    report['libraries'] = {str(path): digest(path) for path in sorted(paths)}
    if TEST in (EXTENSION_RUNTIME_TEST, COOKIE_OBSERVER_TEST):
        report['runtime_helpers'] = {str(BUILD / 'bin' / name): digest(BUILD / 'bin' / name)
                                     for name in (('WebProcess', 'NetworkProcess') if TEST == EXTENSION_RUNTIME_TEST else ('NetworkProcess',))}
    executable = output / 'run'
    command = ['c++', str(output / 'test.o'), '-Wl,--gc-sections', '-Wl,--disable-new-dtags', *libraries, '-o', str(executable)]
    result = subprocess.run(command, cwd=BUILD, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report['link'] = {'command': command, 'exit': result.returncode, 'output': result.stdout}
    print(result.stdout, end='', flush=True)
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    if result.returncode:
        return result.returncode
    dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
    if full_engine and 'libWebKit' not in dynamic:
        raise RuntimeError('Full-engine fixture must link the actual WebKit library')
    if not full_engine and 'libWebKit' in dynamic:
        raise RuntimeError('Pipeline test unexpectedly links WebKit')
    report['linked_webcore_archive'] = not full_engine
    report['linked_webkit'] = full_engine
    report['executable_sha256'] = digest(executable)
    report['dynamic_dependencies'] = dynamic
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    if full_engine:
        environment['LIBRARY_PATH'] = ':'.join(map(str, (BUILD / 'lib', Path('/boot/home/summit-deps/icu78/lib'), Path('/boot/home/summit-deps/libzip-1.11.4/lib'), Path('/boot/system/lib'))))
    if TEST in (EXTENSION_RUNTIME_TEST, COOKIE_OBSERVER_TEST):
        environment['WEBKIT_EXEC_PATH'] = str(BUILD / 'bin')
    if TEST == EXTENSION_RUNTIME_TEST:
        environment['SUMMIT_EXTENSION_STARTUP'] = EXTENSION_STARTUP
        environment['SUMMIT_EXTENSION_FIXTURE'] = EXTENSION_FIXTURE
        environment.pop('SUMMIT_EXTENSION_PACKAGE_DIR', None)
        if EXTENSION_FIXTURE == 'cookies':
            environment['SUMMIT_EXTENSION_PACKAGE_DIR'] = str(output / 'extension-package')
        report['extension_startup'] = EXTENSION_STARTUP
        report['extension_fixture'] = EXTENSION_FIXTURE
        if TRACE_IPC:
            environment['SUMMIT_TRACE_IPC'] = '1'
        else:
            environment.pop('SUMMIT_TRACE_IPC', None)
        report['ipc_trace_requested'] = TRACE_IPC
    from native_crash_log import NativeCrashLog
    crash_log = NativeCrashLog()
    if TEST in PROCESS_TESTS:
        runtime = run_extension_fixture(executable, output, environment,
            timeout={EXTENSION_RUNTIME_TEST: 240, IPC_VALIDATION_TEST: 30, COOKIE_OBSERVER_TEST: 90}[TEST])
        report['exit'], report['output'] = runtime['exit'], runtime.pop('output')
        report['runtime_processes'] = runtime
    else:
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
    if TEST in PROCESS_TESTS:
        report['passed'] = (report['passed'] and runtime['group_drained']
                            and not runtime['forced_group_cleanup'] and not runtime['timeout'])
    if 'runtime_helpers' in report:
        report['runtime_helpers_unchanged'] = unchanged(report['runtime_helpers'])
        report['passed'] = report['passed'] and report['runtime_helpers_unchanged']
    if TEST == EXTENSION_RUNTIME_TEST:
        reports = []
        for line in report['output'].splitlines():
            if 'WebExtension test: ' in line:
                reports.append(json.loads(line.split('WebExtension test: ', 1)[1]))
        completions = [json.loads(item['argumentJSON']) for item in reports
                       if item.get('type') == 'message' and item.get('message') == 'summit-runtime-complete']
        finished = [item for item in reports if item.get('type') == 'finished' and item.get('result') is True]
        assertions = [item for item in reports if item.get('type') == 'assertion']
        reports_passed = (len(completions) == 2 and [item.get('round') for item in completions] == [1, 2]
                          and bool(completions[0].get('nonce')) and completions[0]['nonce'] == completions[1].get('nonce')
                          and len(finished) == 2 and len(assertions) == (28 if EXTENSION_FIXTURE == 'cookies' else 10)
                          and {item.get('message') for item in finished} == {'summit-runtime-round-1', 'summit-runtime-round-2'}
                          and all(item.get('result') is not False for item in reports)
                          and all(item.get('sourceURL', '').startswith('webkit-extension://') for item in reports))
        report['extension_reports'] = reports
        report['extension_reports_passed'] = reports_passed
        report['passed'] = (report['passed'] and reports_passed and report['runtime_helpers_unchanged']
                            and runtime['group_drained'] and not runtime['forced_group_cleanup'] and not runtime['timeout'])
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
        if TEST == EXTENSION_RUNTIME_TEST:
            manifest['extension_fixture'] = EXTENSION_FIXTURE
            if EXTENSION_FIXTURE == 'cookies':
                manifest['helpers'] = {}
                for name in ('manifest.json', 'background.html', 'background.js'):
                    path = ROOT / 'tests/fixtures/extensions/cookies' / name
                    relative = 'extension-package/' + name
                    files['sources/' + relative] = path.read_bytes()
                    manifest['helpers'][relative] = {'source': str(path), 'sha256': digest(path)}
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
    elif TEST == EXTENSION_RUNTIME_TEST:
        command.extend(['--extension-runtime', '--extension-startup', EXTENSION_STARTUP])
        if EXTENSION_FIXTURE != 'storage':
            command.extend(['--extension-fixture', EXTENSION_FIXTURE])
        if TRACE_IPC:
            command.append('--trace-ipc')
    elif TEST == IPC_VALIDATION_TEST:
        command.append('--ipc-validation')
    elif TEST == COOKIE_OBSERVER_TEST:
        command.append('--cookie-observers')
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
    mode.add_argument('--extension-runtime', action='store_true', help='Run an actual extension background/storage/message fixture')
    mode.add_argument('--ipc-validation', action='store_true', help='Test invalid dispatch flags through actual native WebKit connections')
    mode.add_argument('--cookie-observers', action='store_true', help='Test real network cookie observation across rapid unregister/register')
    parser.add_argument('--extension-startup', choices=('cold', 'deferred', 'network', 'page'), default='cold',
                        help='Diagnostic startup preparation for the extension runtime fixture (default: cold)')
    parser.add_argument('--extension-fixture', choices=('storage', 'cookies'), default='storage',
                        help='Extension API package to exercise (default: storage)')
    parser.add_argument('--trace-ipc', action='store_true', help='Enable metadata tracing in an instrumented engine build')
    parser.add_argument('--overlay', help='Candidate translator sources under Source/; requires --dnr')
    args = parser.parse_args()
    if args.overlay and not args.dnr:
        parser.error('--overlay requires --dnr')
    if args.dnr:
        TEST = DNR_TEST
    elif args.store:
        TEST = STORE_TEST
    elif args.extension_runtime:
        TEST = EXTENSION_RUNTIME_TEST
    elif args.ipc_validation:
        TEST = IPC_VALIDATION_TEST
    elif args.cookie_observers:
        TEST = COOKIE_OBSERVER_TEST
    EXTENSION_STARTUP = args.extension_startup
    EXTENSION_FIXTURE = args.extension_fixture
    TRACE_IPC = args.trace_ipc
    if TRACE_IPC and not args.extension_runtime:
        parser.error('IPC tracing requires --extension-runtime')
    if EXTENSION_STARTUP != 'cold' and not args.extension_runtime:
        parser.error('Extension startup preparation requires --extension-runtime')
    if EXTENSION_FIXTURE != 'storage' and not args.extension_runtime:
        parser.error('Extension fixture selection requires --extension-runtime')
    if args.compile_only and (args.run_only or args.resume):
        parser.error('Compile-only and resume/run-only are mutually exclusive')
    raise SystemExit(native(args.compile_only, args.run_only) if args.native else host(args.compile_only, args.resume, args.overlay))
