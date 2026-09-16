#!/usr/bin/env python3
"""Verify CRX3 through the public SDK and actual Summit installation/startup UI.

Runs RSA/ECDSA identity and snapshot checks, published uBlock preparation only,
then native consent, cancellation, runtime IDs, popup resources, saved storage,
and startup rejection of corrupted or newly signed unapproved packages.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import signal
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).name


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, manifest, watched = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    if not all(digest(ROOT / name) == expected for name, expected in inputs.items()):
        raise RuntimeError('Staged inputs changed')
    report = {'scope': __doc__, 'bundle': str(bundle), 'engine': manifest['inputs']['engine'], 'inputs': inputs,
              'compiles': [], 'runs': [], 'passed': False, 'runtime_verified': False}
    def save():
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    temporary = ROOT / 'tmp'; temporary.mkdir(mode=0o700)
    environment = dict(os.environ, TMPDIR=str(temporary), WEBKIT_EXEC_PATH=str(bundle),
                       LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR', 'SUMMIT_TRACE_EXTENSION_CONSOLE'):
        environment.pop(name, None)
    source = bundle / 'source'
    sdk = ROOT / 'EngineExtensionCRXIntegrationTests'
    harness = ROOT / 'ModernExtensionCRXTests'
    for target in (sdk, harness):
        command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar', '-Wno-unused-function', '-fmax-errors=3',
            '-I' + str(source / 'include'), '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
            str(ROOT / 'tests' / (target.name + '.cpp'))]
        if target == sdk:
            command += ['-L' + str(bundle / 'lib'), '-lWebKit', '-lnetwork', '-Wl,-rpath,' + str(bundle / 'lib')]
        command += ['-lbe', '-o', str(target)]
        compiled = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
        row = {'command': command, 'exit': compiled.returncode, 'output': compiled.stdout + compiled.stderr}
        report['compiles'].append(row); save()
        print(row['output'], end='', flush=True)
        if compiled.returncode:
            return 1
        row['sha256'] = digest(target)
    if args.compile_only:
        report['bundle_unchanged'] = all(digest(Path(path)) == value for path, value in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / path) == value for path, value in inputs.items())
        report['passed'] = report['bundle_unchanged'] and report['inputs_unchanged']
        save()
        return 0 if report['passed'] else 1
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(harness), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id': raise RuntimeError('Cannot enumerate native teams')
        result = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group: result.append(pid)
            except ProcessLookupError: pass
        return result
    def run(name, command, harness_command=None):
        row = {'name': name, 'command': command, 'forced_cleanup': False}
        report['runs'].append(row)
        with Path('/SummitExtensions/summit/live-progress.log').open('a') as progress:
            progress.write('[Summit CRX tests] Starting ' + name + '\n')
        with (ROOT / (name + '.log')).open('w') as log:
            process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            row['pid'] = row['process_group'] = process.pid; save()
            try:
                if harness_command:
                    command = harness_command(process.pid)
                    row['harness_command'] = command
                    with (ROOT / (name + '-harness.log')).open('w') as output:
                        result = subprocess.run(command, env=environment, stdout=output, stderr=subprocess.STDOUT, timeout=180)
                    row['harness_exit'] = result.returncode
                    row['harness_output'] = (ROOT / (name + '-harness.log')).read_text(errors='replace')
                    row['marker_seen'] = 'CRX_BROWSER_RESULT PASS phase=' + name + ' checks=' in row['harness_output']
                row['exit'] = process.wait(timeout=15 if harness_command else 180)
            except Exception as error:
                row['error'] = str(error)
            finally:
                if process.poll() is None:
                    row['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL); process.wait(timeout=5)
                row['exit'] = process.returncode
                deadline = time.monotonic() + 10
                while members(process.pid) and time.monotonic() < deadline: time.sleep(.05)
                row['remaining_processes'] = members(process.pid)
                row['group_drained'] = not row['remaining_processes']
                if not row['group_drained']:
                    row['forced_cleanup'] = True; os.killpg(process.pid, signal.SIGKILL)
        row['output'] = (ROOT / (name + '.log')).read_text(errors='replace')
        if not harness_command: row['marker_seen'] = 'CRX_SDK_RESULT PASS checks=' in row['output']
        row['passed'] = row['exit'] == 0 and row.get('harness_exit', 0) == 0 and row['marker_seen'] and row['group_drained'] and not row['forced_cleanup'] and 'error' not in row
        save()
        print(json.dumps({'phase': name, 'passed': row['passed'], 'error': row.get('error')}), flush=True)
        with Path('/SummitExtensions/summit/live-progress.log').open('a') as progress:
            progress.write('[Summit CRX tests] ' + name + (' PASS' if row['passed'] else ' FAIL') + '\n')
        if not row['passed']: raise RuntimeError(name + ' failed')
        return row
    monitor = NativeCrashLog()
    control = ROOT / 'control'; control.mkdir(mode=0o700)
    fixture = ROOT / 'fixtures'
    profile = ROOT / 'browser-profile'
    try:
        if args.suite in ('sdk', 'all'):
            run('sdk', [str(sdk), str(ROOT / 'sdk-work'), str(fixture)])
        if args.suite in ('browser', 'all'):
            profile.mkdir(mode=0o700)
            report['profile'] = str(profile)
            def browser(phase):
                return run(phase, [str(bundle / 'Summit'), '--profile', str(profile), 'about:blank'],
                    lambda pid: [str(harness), str(pid), str(bundle / 'Summit'), str(fixture), str(profile), str(control), phase, 'watch' if args.watch else 'run'])
            browser('install')
            entry = json.loads((control / 'installed.json').read_text())
            name = entry['package']
            if Path(name).name != name or not name.startswith('pkg-'): raise RuntimeError('Invalid retained package name')
            installed = profile / 'Extensions/packages' / name / 'package'
            original = (fixture / 'rsa.crx').read_bytes()
            report['installed_extension'] = entry
            report['retained_crx_sha256'] = digest(installed)
            if not installed.is_file() or installed.read_bytes() != original: raise RuntimeError('Installer did not retain the exact signed CRX bytes')
            catalog = profile / 'Extensions/catalog.json'
            approved_catalog = digest(catalog)
            browser('startup')
            try:
                installed.write_bytes((fixture / 'corrupt.crx').read_bytes())
                row = browser('corrupt-startup')
                if row['output'].count('Summit extension ' + entry['identifier'] + ':') != 1:
                    raise RuntimeError('Corrupt startup did not report exactly one rejected installation')
                installed.write_bytes((fixture / 'updated.crx').read_bytes())
                row = browser('updated-startup')
                expected = 'Summit extension ' + entry['identifier'] + ': The installed package has changed and requires new approval.'
                if row['output'].count(expected) != 1: raise RuntimeError('Signed update did not require fresh approval')
            finally:
                installed.write_bytes(original)
            browser('restored')
            report['catalog_unchanged'] = digest(catalog) == approved_catalog
            report['retained_crx_unchanged'] = installed.read_bytes() == original
            if not report['catalog_unchanged'] or not report['retained_crx_unchanged']:
                raise RuntimeError('Startup altered the approved catalog or retained original package')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        report['screenshots'] = {path.name: digest(path) for path in control.glob('*.ppm')}
        report['native_crash_log'] = monitor.finish()
        helper.verified_bundle(args.bundle)
        report['bundle_unchanged'] = all(digest(Path(path)) == value for path, value in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / path) == value for path, value in inputs.items())
        report['passed'] = report['passed'] and report['native_crash_log']['passed'] and report['bundle_unchanged'] and report['inputs_unchanged']
        report['runtime_verified'] = report['passed']; save()
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    if not args.fixtures or not args.archive_fixtures:
        raise RuntimeError('Host mode requires frozen runtime and archive fixture directories')
    names = ('tests/EngineExtensionCRXIntegrationTests.cpp', 'tests/ModernExtensionCRXTests.cpp',
             'tests/ModernExtensionManagerTests.cpp', 'tests/ModernCloseTests.cpp',
             'tools/' + SCRIPT, 'tools/test-modern-close-native.py', 'tools/native_crash_log.py')
    files = {}; original = {}
    def add(name, path):
        data = path.read_bytes(); files[name] = data; original[str(path.resolve())] = hashlib.sha256(data).hexdigest()
    for name in names: add(name, ROOT / name)
    for path in sorted(args.fixtures.iterdir()):
        if path.is_file(): add('fixtures/' + path.name, path)
    for name in ('unsafe-parent.crx', 'published-ublock.crx'):
        add('fixtures/' + name, args.archive_fixtures / name)
    expected = json.loads(files['fixtures/expected.json'])
    if not all(hashlib.sha256(files['fixtures/' + name]).hexdigest() == value for name, value in expected['sha256'].items()):
        raise RuntimeError('Runtime fixture fingerprint changed')
    if hashlib.sha256(files['fixtures/published-ublock.crx']).hexdigest() != 'b6be71ed3e3e85eaad8f02710b9071d06428e141d942c43d5f65d4526e82dc3e':
        raise RuntimeError('Published uBlock package differs from its pinned bytes')
    files['inputs.json'] = (json.dumps({name: hashlib.sha256(data).hexdigest() for name, data in files.items()}, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as tar:
        for name, data in files.items():
            item = tarfile.TarInfo(name); item.size = len(data); item.mode = 0o600; tar.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /SummitExtensions/summit/extension-crx-runtime.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/SummitExtensions/summit/extension-crx-runtime.') or not stage.rsplit('.', 1)[-1].isalnum(): raise RuntimeError('Invalid native stage')
    output = ROOT / '.vm' / ('modern-extension-crx-' + secrets.token_hex(12)); output.mkdir()
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'sources': original}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print(json.dumps({'stage': stage, 'output': str(output)}), flush=True)
    command = ['python3.10', stage + '/tools/' + SCRIPT, '--native', '--bundle', args.bundle, '--suite', args.suite]
    if args.watch: command.append('--watch')
    if args.compile_only: command.append('--compile-only')
    else: subprocess.run(['python3', str(ROOT / 'tools/vm.py'), 'key', 'shift'], check=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    fetched = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True, check=True)
    report = {'native': json.loads(fetched.stdout), 'source_sha256': original,
              'sources_unchanged': all(digest(Path(name)) == value for name, value in original.items())}
    report['passed'] = result.returncode == 0 and report['native']['passed'] and report['sources_unchanged']
    for name, value in report['native'].get('screenshots', {}).items():
        if Path(name).name != name or not name.endswith('.ppm'): raise RuntimeError('Invalid screenshot name')
        data = remote('cat ' + shlex.quote(stage + '/control/' + name), capture_output=True, check=True).stdout
        if hashlib.sha256(data).hexdigest() != value: raise RuntimeError('Screenshot changed')
        (output / name).write_bytes(data)
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json'), 'error': report['native'].get('error')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--archive-fixtures', type=Path)
    parser.add_argument('--suite', choices=('sdk', 'browser', 'all'), default='all')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--watch', action='store_true')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise SystemExit(native(args) if args.native else host(args))
