#!/usr/bin/env python3
"""Test the actual WebCore content-rule parser, compiler, caches and backend on Haiku."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import tarfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit-extensions')
BUILD = ENGINE / 'WebKitBuild/Modern'
TEST = 'EngineContentRulePipelineTests.cpp'
DNR_TEST = 'EngineExtensionDNRRulesTests.cpp'
STORE_TEST = 'EngineContentRuleStoreTests.cpp'
EXTENSION_RUNTIME_TEST = 'EngineExtensionRuntimeTests.cpp'
IPC_VALIDATION_TEST = 'EngineIPCValidationTests.cpp'
COOKIE_OBSERVER_TEST = 'EngineCookieObserverRuntimeTests.cpp'
PACKAGE_PREPARATION_TEST = 'EngineExtensionPackagePreparationTests.cpp'
PACKAGE_ACTIVATION_TEST = 'EngineExtensionPackageActivationTests.cpp'
HYPERLINK_TEST = 'EngineHyperlinkAuditingTests.cpp'
PRIVACY_TEST = 'EngineExtensionPrivacyTests.cpp'
PRIVACY_NETWORK_TEST = 'EngineExtensionPrivacyNetworkTests.cpp'
PROCESS_TESTS = (EXTENSION_RUNTIME_TEST, IPC_VALIDATION_TEST, COOKIE_OBSERVER_TEST, PACKAGE_PREPARATION_TEST, PACKAGE_ACTIVATION_TEST, HYPERLINK_TEST, PRIVACY_TEST, PRIVACY_NETWORK_TEST)
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
        elif TEST == PACKAGE_PREPARATION_TEST:
            report['scope'] = 'actual public BWebKitContext extension package preparation, background filesystem work, manifest metadata, cancellation, token ownership and cleanup; no extension execution or installation'
        elif TEST == PACKAGE_ACTIVATION_TEST:
            report['scope'] = 'actual public BWebKitContext preparation/loading/unloading, approved snapshots, saved grants, native BWebKitView extension pages, background execution, storage and cookies; context reload within one browser process, not browser restart or installer UI'
        elif TEST == HYPERLINK_TEST:
            report['scope'] = 'actual native page preferences and HTTP ping, navigation and beacon traffic across default, disabled, re-enabled and initially disabled pages; no extension privacy API runtime'
        elif TEST == PRIVACY_TEST:
            report['scope'] = 'actual extension privacy bindings, privileged IPC, callback/Promise replies, onChange, installation arbitration, saved state and effective regular/private page preferences; no HTTP/TLS network proof, browser restart or private-window UI integration'
        elif TEST == PRIVACY_NETWORK_TEST:
            report['scope'] = 'actual extension privacy setters and HTTPS requests across cached/fresh prefetch candidates, existing/new pages, pings and unaffected ordinary scripts, beacons and navigation; local TLS leaf is explicitly pinned, not general certificate-trust verification'
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
    if TEST in (PRIVACY_TEST, PRIVACY_NETWORK_TEST) and not (ENGINE / 'Source/WebKit/UIProcess/Extensions/haiku/WebExtensionContextAPIPrivacyHaiku.cpp').is_file():
        raise RuntimeError('The real privacy backend is not integrated into this engine; a compile preflight cannot be run as a privacy API test')
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
    if TEST in (EXTENSION_RUNTIME_TEST, COOKIE_OBSERVER_TEST, PACKAGE_ACTIVATION_TEST, HYPERLINK_TEST, PRIVACY_TEST, PRIVACY_NETWORK_TEST):
        report['runtime_helpers'] = {str(BUILD / 'bin' / name): digest(BUILD / 'bin' / name)
                                     for name in (('NetworkProcess',) if TEST == COOKIE_OBSERVER_TEST else ('WebProcess', 'NetworkProcess'))}
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
    if TEST in (EXTENSION_RUNTIME_TEST, COOKIE_OBSERVER_TEST, PACKAGE_ACTIVATION_TEST, HYPERLINK_TEST, PRIVACY_TEST, PRIVACY_NETWORK_TEST):
        environment['WEBKIT_EXEC_PATH'] = str(BUILD / 'bin')
    if TEST == PACKAGE_PREPARATION_TEST:
        environment['SUMMIT_EXTENSION_PREPARATION_ARCHIVE'] = str(output / 'preparation.xpi')
    if TEST == PACKAGE_ACTIVATION_TEST:
        environment['SUMMIT_EXTENSION_ACTIVATION_PACKAGE'] = str(output / 'activation-package')
    if TEST in (PRIVACY_TEST, PRIVACY_NETWORK_TEST):
        environment['SUMMIT_PRIVACY_PACKAGE'] = str(output / 'privacy-package')
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
    hyperlink_fixture = None
    privacy_network_fixture = None
    if TEST == HYPERLINK_TEST:
        sys.path.insert(0, str(output))
        from hyperlink_auditing_fixture import HyperlinkAuditingFixture
        hyperlink_fixture = HyperlinkAuditingFixture()
        environment['SUMMIT_HYPERLINK_BASE_URL'] = hyperlink_fixture.base
        environment['SUMMIT_HYPERLINK_NONCE'] = hyperlink_fixture.nonce
    if TEST == PRIVACY_NETWORK_TEST:
        sys.path.insert(0, str(output))
        from privacy_network_fixture import PrivacyNetworkFixture
        privacy_network_fixture = PrivacyNetworkFixture()
        environment['SUMMIT_PRIVACY_NETWORK_BASE'] = privacy_network_fixture.base
        environment['SUMMIT_PRIVACY_NETWORK_NONCE'] = privacy_network_fixture.nonce
        environment['SUMMIT_PRIVACY_NETWORK_CERTIFICATE'] = str(privacy_network_fixture.certificate)
    crash_log = NativeCrashLog()
    if TEST in PROCESS_TESTS:
        try:
            runtime = run_extension_fixture(executable, output, environment,
                timeout={EXTENSION_RUNTIME_TEST: 240, IPC_VALIDATION_TEST: 30, COOKIE_OBSERVER_TEST: 90, PACKAGE_PREPARATION_TEST: 120, PACKAGE_ACTIVATION_TEST: 180, HYPERLINK_TEST: 130, PRIVACY_TEST: 420, PRIVACY_NETWORK_TEST: 300}[TEST])
        finally:
            if hyperlink_fixture:
                report['hyperlink_network'] = hyperlink_fixture.finish()
            if privacy_network_fixture:
                report['privacy_network'] = privacy_network_fixture.finish()
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
    if TEST == HYPERLINK_TEST:
        rounds = [match.groups() for line in report['output'].splitlines()
                  if (match := re.fullmatch(r'HYPERLINK_ROUND ([0-3]) (enabled|disabled) ([a-f0-9]{32})', line))]
        report['hyperlink_rounds'] = rounds
        report['hyperlink_rounds_passed'] = rounds == [(str(number), 'enabled' if number in (0, 2) else 'disabled', hyperlink_fixture.nonce) for number in range(4)]
        report['passed'] = report['passed'] and report['hyperlink_rounds_passed'] and report['hyperlink_network']['passed']
    if TEST == PRIVACY_TEST:
        plans = re.findall(r'^PRIVACY_PLAN steps=(\d+) commands=(\d+)$', report['output'], re.M)
        results = re.findall(r'^PRIVACY_API_RESULT PASS steps=(\d+) commands=(\d+) checks=(\d+) failures=0$', report['output'], re.M)
        responses = [json.loads(line.removeprefix('PRIVACY_RESPONSE ')) for line in report['output'].splitlines()
                     if line.startswith('PRIVACY_RESPONSE ')]
        events = [json.loads(line.split('WebExtension test: ', 1)[1]) for line in report['output'].splitlines()
                  if 'WebExtension test: ' in line]
        messages = [json.loads(event['argumentJSON']) for event in events
                    if event.get('type') == 'message' and event.get('message') == 'summit-privacy-response']
        sequences = [value.get('sequence') for value in responses]
        report['privacy_responses'] = responses
        report['privacy_reports_passed'] = (len(plans) == len(results) == 1 and plans[0] == results[0][:2]
            and int(plans[0][0]) >= 60 and int(plans[0][1]) >= 40 and int(results[0][2]) > 100
            and len(responses) == int(plans[0][1]) and messages == responses
            and all(isinstance(value, int) for value in sequences) and sequences == sorted(set(sequences))
            and len({value.get('nonce') for value in responses}) == 1 and bool(responses[0].get('nonce'))
            and all(event.get('result') is not False and event.get('sourceURL', '').startswith('webkit-extension://') for event in events))
        report['passed'] = report['passed'] and report['privacy_reports_passed']
    if TEST == PRIVACY_NETWORK_TEST:
        rounds = re.findall(r'^PRIVACY_NETWORK_ROUND ([0-4]) (enabled|disabled) ([a-f0-9]{32})$', report['output'], re.M)
        results = re.findall(r'^PRIVACY_NETWORK_RESULT PASS rounds=5 api_commands=12 tls_pins=(\d+) failures=0$', report['output'], re.M)
        responses = [json.loads(line.removeprefix('PRIVACY_NETWORK_API ')) for line in report['output'].splitlines()
                     if line.startswith('PRIVACY_NETWORK_API ')]
        events = [json.loads(line.split('WebExtension test: ', 1)[1]) for line in report['output'].splitlines()
                  if 'WebExtension test: ' in line]
        messages = [json.loads(event['argumentJSON']) for event in events
                    if event.get('type') == 'message' and event.get('message') == 'summit-privacy-response']
        report['privacy_network_rounds'] = rounds
        report['privacy_network_responses'] = responses
        report['privacy_network_reports_passed'] = (
            rounds == [(str(number), 'enabled' if number in (0, 4) else 'disabled', privacy_network_fixture.nonce) for number in range(5)]
            and len(results) == 1 and int(results[0]) > 0
            and len(responses) == 12 and messages == responses
            and [value.get('sequence') for value in responses] == list(range(1, 13))
            and all(value.get('ok') is True and value.get('nonce') == privacy_network_fixture.nonce
                    and value.get('extension') == 'summit-privacy-network' for value in responses)
            and all(value.get('callbacks') == 1 and value.get('lastErrorCleared') is True for value in responses[1::2])
            and all(event.get('result') is not False and event.get('sourceURL', '').startswith('webkit-extension://') for event in events))
        report['passed'] = report['passed'] and report['privacy_network_reports_passed'] and report['privacy_network']['passed']
    if TEST == PACKAGE_ACTIVATION_TEST:
        pages = [match.groups() for line in report['output'].splitlines()
                 if (match := re.fullmatch(r'EXTENSION_PAGE SUMMIT ACTIVATION (\d+-\d+) ([123]) PASS (\d+)', line))]
        report['extension_page_reports'] = pages
        report['extension_page_reports_passed'] = (len(pages) == 3
            and len({page[0] for page in pages}) == 1
            and [(page[1], page[2]) for page in pages] == [('1', '7'), ('2', '6'), ('3', '4')])
        report['passed'] = report['passed'] and report['extension_page_reports_passed']
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
        if TEST == HYPERLINK_TEST:
            path = ROOT / 'tools/hyperlink_auditing_fixture.py'
            files['sources/' + path.name] = path.read_bytes()
            manifest['helpers'] = {path.name: {'source': str(path), 'sha256': digest(path)}}
        if TEST in (PRIVACY_TEST, PRIVACY_NETWORK_TEST):
            manifest['helpers'] = {}
            for name in ('manifest.json', 'background.html', 'background.js'):
                path = ROOT / 'tests/fixtures/extensions/privacy' / name
                relative = 'privacy-package/' + name
                files['sources/' + relative] = path.read_bytes()
                manifest['helpers'][relative] = {'source': str(path), 'sha256': digest(path)}
            if TEST == PRIVACY_NETWORK_TEST:
                path = ROOT / 'tools/privacy_network_fixture.py'
                files['sources/' + path.name] = path.read_bytes()
                manifest['helpers'][path.name] = {'source': str(path), 'sha256': digest(path)}
        if TEST == PACKAGE_ACTIVATION_TEST:
            manifest['helpers'] = {}
            for name in ('manifest.json', 'background.html', 'background.js', 'probe.html', 'probe.js'):
                path = ROOT / 'tests/fixtures/extensions/activation' / name
                relative = 'activation-package/' + name
                files['sources/' + relative] = path.read_bytes()
                manifest['helpers'][relative] = {'source': str(path), 'sha256': digest(path)}
        if TEST == PACKAGE_PREPARATION_TEST:
            package = io.BytesIO()
            manifest['package_sources'] = {}
            with zipfile.ZipFile(package, 'w') as archive:
                for name in ('manifest.json', 'background.html', 'background.js'):
                    path = ROOT / 'tests/fixtures/extensions/preparation' / name
                    archive.writestr(zipfile.ZipInfo(name), path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
                    manifest['package_sources'][name] = {'source': str(path), 'sha256': digest(path)}
            files['sources/preparation.xpi'] = package.getvalue()
            manifest['helpers'] = {'preparation.xpi': {'source': 'generated ZIP of package_sources',
                                   'sha256': hashlib.sha256(package.getvalue()).hexdigest()}}
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
    elif TEST == PACKAGE_PREPARATION_TEST:
        command.append('--extension-package-preparation')
    elif TEST == PACKAGE_ACTIVATION_TEST:
        command.append('--extension-package-activation')
    elif TEST == HYPERLINK_TEST:
        command.append('--hyperlink-auditing')
    elif TEST == PRIVACY_TEST:
        command.append('--extension-privacy')
    elif TEST == PRIVACY_NETWORK_TEST:
        command.append('--extension-privacy-network')
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
    mode.add_argument('--extension-package-preparation', action='store_true', help='Test public extension preparation, cancellation and owned package cleanup')
    mode.add_argument('--extension-package-activation', action='store_true', help='Test public extension loading, saved grants and native extension pages')
    mode.add_argument('--hyperlink-auditing', action='store_true', help='Test actual native hyperlink ping controls and unaffected navigation and beacons')
    mode.add_argument('--extension-privacy', action='store_true', help='Test actual privacy bindings, lifecycle, saved-state failures and regular/private page preferences')
    mode.add_argument('--extension-privacy-network', action='store_true', help='Test real privacy setters against unfiltered HTTPS prefetch, ping, beacon and navigation requests')
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
    elif args.extension_package_preparation:
        TEST = PACKAGE_PREPARATION_TEST
    elif args.extension_package_activation:
        TEST = PACKAGE_ACTIVATION_TEST
    elif args.hyperlink_auditing:
        TEST = HYPERLINK_TEST
    elif args.extension_privacy:
        TEST = PRIVACY_TEST
    elif args.extension_privacy_network:
        TEST = PRIVACY_NETWORK_TEST
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
