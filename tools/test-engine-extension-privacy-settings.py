#!/usr/bin/env python3
"""Compile and run the real native privacy model/state writer with frozen WTF.

No WebCore/WebKit objects are linked. This does not validate the controller,
extension API, events, page preferences or network behavior. Run runtime tests
serially with other VM crash-log checks; --compile-only leaves the preview open.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path('/boot/home/summit-webkit-extensions')
PREFIX = '/boot/home/summit/extension-privacy-settings-tests.'
SCRIPT = 'test-engine-extension-privacy-settings.py'
HELPERS = {
    'UIProcess/Extensions/haiku': ('WebExtensionStateHaiku.h', 'WebExtensionStateHaiku.cpp'),
    'Shared/Extensions': ('WebExtensionPrivacySettingsHaiku.h', 'WebExtensionPrivacySettingsHaiku.cpp'),
}
UNITS = ('WebExtensionStateHaiku.cpp', 'WebExtensionPrivacySettingsHaiku.cpp', 'EngineExtensionPrivacySettingsTests.cpp')
LIBRARIES = ('libJavaScriptCore.so.18.7.4', 'libicudata.so.78.3', 'libicui18n.so.78.3', 'libicuuc.so.78.3')
SCOPE = 'native privacy scope/value model, saved-state codec and atomic state writer only; no controller, API, event, page or network runtime'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def verify_files(files):
    for path, expected in files.items():
        if digest(Path(path)) != expected:
            raise RuntimeError('Input changed: ' + path)


def native(compile_only=False, run_only=False):
    output = ROOT / 'sources'
    manifest = json.loads((output / 'source-manifest.json').read_text())
    staged = {str(ROOT / name): info['sha256'] for name, info in manifest['files'].items()}
    verify_files(staged)
    build = ENGINE / 'WebKitBuild/Modern'
    watched = [build / 'build.ninja', build / 'cmakeconfig.h', ENGINE / '.summit-source-manifest.json',
               ENGINE / 'Source/WebKit/WebKitPrefix.h']
    snapshot = {str(path): digest(path) for path in watched}
    if json.loads(watched[2].read_text())['patch_sha256'] != manifest['engine_patch_sha256']:
        raise RuntimeError('Staged and native engine patch identities differ')
    config = watched[1].read_text()
    for feature in ('WK_WEB_EXTENSIONS', 'CONTENT_EXTENSIONS'):
        if not re.search(r'^#define ENABLE_' + feature + r' 1$', config, re.M):
            raise RuntimeError('The native configured engine must enable ' + feature)
    libraries = manifest['frozen_libraries']
    verify_files(libraries)
    frozen = Path(manifest['frozen_bundle']) / 'lib'
    result_path = output / 'results.json'

    if run_only:
        report = json.loads(result_path.read_text())
        if not report.get('compile_passed') or report.get('source_manifest') != manifest:
            raise RuntimeError('Resume requires the same successfully compiled source snapshot')
        verify_files(report['native_inputs'])
        verify_files(report['artifacts'])
    else:
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
        if not all(key in fields for key in ('DEFINES', 'INCLUDES', 'FLAGS')):
            raise RuntimeError('The configured native compile command is missing')
        raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
        flags = []
        for flag in raw:
            if flag == '-include':
                next(raw)
            elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
                flags.append(flag)
        flags = ['-I' + str(output), '-iquote', str(output), *flags,
                 '-include', str(watched[3]), '-fdiagnostics-color=never', '-fmax-errors=5']
        report = {'scope': SCOPE, 'source_manifest': manifest, 'native_inputs': snapshot,
                  'frozen_libraries': libraries, 'compile_results': [], 'linked_webcore_or_webkit': False,
                  'runtime_executed': False, 'compile_passed': False, 'passed': False}
        objects = []
        for name in UNITS:
            obj = output / (Path(name).stem + '.o')
            command = ['c++', *flags, '-c', str(output / name), '-o', str(obj)]
            result = subprocess.run(command, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            print(result.stdout, end='', flush=True)
            print('COMPILE', name, result.returncode, flush=True)
            report['compile_results'].append({'source': name, 'command': command, 'exit': result.returncode, 'output': result.stdout})
            objects.append(obj)
        if any(unit['exit'] for unit in report['compile_results']):
            result_path.write_text(json.dumps(report, indent=2) + '\n')
            return 1
        executable = output / 'run'
        command = ['c++', *map(str, objects), str(frozen / LIBRARIES[0]), '-lbe', '-lnetwork',
                   '-Wl,--no-export-dynamic', '-Wl,--gc-sections', '-Wl,-rpath,' + str(frozen), '-o', str(executable)]
        link = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['link'] = {'command': command, 'exit': link.returncode, 'output': link.stdout}
        print(link.stdout, end='', flush=True)
        if link.returncode:
            result_path.write_text(json.dumps(report, indent=2) + '\n')
            return 1
        dynamic = subprocess.check_output(['readelf', '-d', str(executable)], text=True)
        if 'libWebKit' in dynamic or 'libWebCore' in dynamic:
            raise RuntimeError('Privacy model helper unexpectedly links WebCore or WebKit')
        report['dynamic_dependencies'] = dynamic
        report['artifacts'] = {str(path): digest(path) for path in [*objects, executable]}
        report['compile_passed'] = True
    verify_files(report['native_inputs'])
    verify_files(staged)
    verify_files(libraries)
    report['inputs_unchanged'] = True
    if compile_only:
        result_path.write_text(json.dumps(report, indent=2) + '\n')
        print('Native privacy model compiled and linked; runtime not executed.', flush=True)
        return 0

    from native_crash_log import NativeCrashLog
    environment = dict(os.environ, LIBRARY_PATH=str(frozen) + ':/boot/system/lib')
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'LD_LIBRARY_PATH', 'DISABLE_ASLR', 'WEBKIT_EXEC_PATH'):
        environment.pop(name, None)
    report['runtime_executed'] = True
    crash_log = NativeCrashLog()
    try:
        result = subprocess.run([str(output / 'run')], env=environment, timeout=60, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['exit'] = result.returncode
        report['output'] = result.stdout
    except subprocess.TimeoutExpired:
        report['exit'] = None
        report['output'] = 'Native privacy model test timed out and was terminated; failed run'
    report['native_crash_log'] = crash_log.finish()
    verify_files(report['native_inputs'])
    verify_files(report['artifacts'])
    verify_files(staged)
    verify_files(libraries)
    marker = re.findall(r'^PRIVACY_SETTINGS_RESULT PASS checks=(\d+) failures=0$', report['output'], re.M)
    report['checks'] = int(marker[0]) if len(marker) == 1 else 0
    report['passed'] = report['exit'] == 0 and report['checks'] == 71 and report['native_crash_log']['passed']
    result_path.write_text(json.dumps(report, indent=2) + '\n')
    print(report['output'], end='', flush=True)
    return 0 if report['passed'] else 1


def host(overlay, bundle_result, compile_only, resume):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    if resume:
        stage = resume
        if not re.fullmatch(re.escape(PREFIX) + r'[A-Za-z0-9]+', stage):
            raise RuntimeError('Invalid native privacy test stage')
        output = ROOT / '.vm' / Path(stage).name
        prior = json.loads((output / 'result.json').read_text())
        manifest = prior['native']['source_manifest']
        if prior['stage'] != stage or not prior['native'].get('compile_passed'):
            raise RuntimeError('No matching successful compile result')
    else:
        files = {}
        for directory, names in HELPERS.items():
            for name in names:
                relative = Path('Source/WebKit') / directory / name
                source = Path(overlay).resolve() / relative if overlay else ROOT / '.cache/WebKit' / relative
                if not source.exists():
                    source = ROOT / '.cache/WebKit' / relative
                files['sources/' + name] = source
        test = ROOT / 'tests/EngineExtensionPrivacySettingsTests.cpp'
        files['sources/' + test.name] = test
        for name in (SCRIPT, 'native_crash_log.py'):
            files['tools/' + name] = ROOT / 'tools' / name
        bundle_path = Path(bundle_result).resolve()
        bundle = json.loads(bundle_path.read_text())
        manifest = {'engine_patch_sha256': json.loads((ROOT / 'engine/sources.lock.json').read_text())['patch']['sha256'],
                    'files': {name: {'source': str(path), 'sha256': digest(path)} for name, path in files.items()},
                    'frozen_bundle': bundle['bundle'], 'bundle_result': str(bundle_path), 'bundle_result_sha256': digest(bundle_path),
                    'frozen_libraries': {str(Path(bundle['bundle']) / 'lib' / name): bundle['bundled_sha256']['lib/' + name] for name in LIBRARIES}}
        content = {name: path.read_bytes() for name, path in files.items()}
        content['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in content.items():
                item = tarfile.TarInfo(name)
                item.mode, item.size = 0o600, len(data)
                stream.addfile(item, io.BytesIO(data))
        stage = remote('mktemp -d ' + PREFIX + 'XXXXXXXX', check=True, text=True, capture_output=True).stdout.strip()
        if not re.fullmatch(re.escape(PREFIX) + r'[A-Za-z0-9]+', stage):
            raise RuntimeError('Unexpected native staging directory')
        output = ROOT / '.vm' / Path(stage).name
        output.mkdir(exist_ok=False)
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    host_inputs = {info['source']: info['sha256'] for info in manifest['files'].values()}
    host_inputs[manifest['bundle_result']] = manifest['bundle_result_sha256']
    verify_files(host_inputs)
    print('Native privacy settings stage: ' + stage, flush=True)
    command = ['python3.10', stage + '/tools/' + SCRIPT, '--native']
    if compile_only:
        command.append('--compile-only')
    if resume:
        command.append('--run-only')
    log_path = output / ('runtime.log' if resume else 'native.log')
    with log_path.open('x') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    print(log_path.read_text(), end='', flush=True)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    verify_files(host_inputs)
    report['host_inputs_unchanged'] = True
    path = output / ('runtime-result.json' if resume else 'result.json')
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(path), 'compile_passed': report['native'].get('compile_passed', False),
                      'passed': report['native'].get('passed', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--run-only', action='store_true')
    parser.add_argument('--overlay', help='Candidate source tree containing Source/')
    parser.add_argument('--bundle-result', default=str(ROOT / '.vm/modern-browser-ublock-console-bundle-result.json'))
    parser.add_argument('--resume', help='Run a previously compiled native stage after closing the preview')
    args = parser.parse_args()
    if args.compile_only and (args.run_only or args.resume):
        parser.error('--compile-only cannot be combined with a runtime resume')
    raise SystemExit(native(args.compile_only, args.run_only) if args.native else host(args.overlay, args.bundle_result, args.compile_only, args.resume))
