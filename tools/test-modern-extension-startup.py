#!/usr/bin/env python3
"""Seed approval through the public SDK, then verify two real browser startups."""
import argparse
import hashlib
import http.server
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import signal
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    from importlib.util import spec_from_file_location, module_from_spec
    spec = spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, manifest, before = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if digest(ROOT / name) != expected:
            raise RuntimeError('Staged input changed: ' + name)
    source = bundle / 'source'
    executable = ROOT / 'ExtensionStartupSeed'
    compile_command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
        '-I' + str(source / 'include'), '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
        str(ROOT / 'tests/ExtensionStartupSeed.cpp'), str(source / 'src/core/ExtensionCatalog.cpp'),
        '-L' + str(bundle / 'lib'), '-lWebKit', '-lbe', '-lnetwork',
        '-Wl,-rpath,' + str(bundle / 'lib'), '-o', str(executable)]
    report = {'bundle': str(bundle), 'inputs': inputs, 'compile_command': compile_command, 'runs': [], 'passed': False}
    result_path = ROOT / 'result.json'
    def save():
        result_path.write_text(json.dumps(report, indent=2) + '\n')
    save()
    compiled = subprocess.run(compile_command, capture_output=True, text=True)
    report['compile_exit'], report['compile_output'] = compiled.returncode, compiled.stdout + compiled.stderr
    print(report['compile_output'], end='', flush=True)
    save()
    if compiled.returncode:
        return 1
    report['executable_sha256'] = digest(executable)
    # The existing image guard checks actual loaded executable paths, not names.
    guard = ROOT / 'CheckUnused'
    subprocess.run(['c++', '-std=c++23', '-O2', '-Wno-multichar', '-I' + str(source / 'src'),
        '-I' + str(source / 'vendor'), str(ROOT / 'tests/ModernCloseTests.cpp'), '-lbe', '-o', str(guard)], check=True)
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(guard), '--check-executable-unused', str(bundle / name)], check=True)
    profile = ROOT / 'profile'
    profile.mkdir(mode=0o700)
    report['profile'] = str(profile)
    environment = dict(os.environ, WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    monitor = NativeCrashLog()
    status_url = args.base_url + '/status?run=' + args.run_token

    def observations():
        with urllib.request.urlopen(status_url, timeout=5) as response:
            return json.load(response)

    def group_members(group):
        # Haiku's killpg(group, 0) can succeed after every live team exits.
        # Use the kernel team list, as the engine extension runtime runner does.
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id':
            raise RuntimeError('Could not enumerate native live teams')
        members = []
        for line in lines[1:]:
            identifier = int(line.strip())
            try:
                if os.getpgid(identifier) == group:
                    members.append(identifier)
            except ProcessLookupError:
                pass
        return members

    def run_process(name, command, boot=None):
        record = {'name': name, 'command': command, 'forced_cleanup': False}
        report['runs'].append(record)
        with (ROOT / (name + '.log')).open('w') as log:
            process = subprocess.Popen(command, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            record['pid'] = record['process_group'] = process.pid
            save()
            try:
                if boot is not None:
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        data = observations()
                        if data['errors']:
                            raise RuntimeError('Unexpected extension report: ' + repr(data['errors']))
                        if len(data['reports']) >= boot:
                            break
                        if process.poll() is not None:
                            raise RuntimeError('Browser exited before extension startup report')
                        time.sleep(.05)
                    else:
                        raise RuntimeError('Extension startup report deadline')
                    observed = data['reports'][-1]
                    if observed != {'id': 'summit-startup-fixture', 'boot': boot, 'nonce': args.run_token,
                                    'previousNonce': args.run_token, 'granted': True}:
                        raise RuntimeError('Incorrect saved extension startup state: ' + repr(observed))
                    record['background_report'] = observed
                    subprocess.run([str(executable), '--close-browser', str(process.pid), str(bundle / 'Summit')],
                                   env=environment, check=True, timeout=10)
                record['exit'] = process.wait(timeout=60 if boot is None else 15)
            except Exception as error:
                record['error'] = str(error)
            finally:
                if process.poll() is None:
                    record['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                record['exit'] = process.returncode
                deadline = time.monotonic() + 10
                while group_members(process.pid) and time.monotonic() < deadline:
                    time.sleep(.05)
                record['remaining_processes'] = group_members(process.pid)
                record['group_drained'] = not record['remaining_processes']
                if not record['group_drained']:
                    record['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGKILL)
        record['output'] = (ROOT / (name + '.log')).read_text()
        record['passed'] = record['exit'] == 0 and record['group_drained'] and not record['forced_cleanup'] and 'error' not in record
        save()
        if not record['passed']:
            raise RuntimeError(name + ' failed')
        if boot is not None:
            expected = 'Summit extension summit-startup-changed: The installed package has changed and requires new approval.'
            if record['output'].count(expected) != 1 or 'Summit extension summit-startup-disabled:' in record['output']:
                raise RuntimeError('Startup did not reject the changed package and skip the disabled one')
        print(name + ' passed with clean exit and drained helper group', flush=True)

    try:
        run_process('seed', [str(executable), str(profile), str(ROOT / 'package'), args.run_token])
        initial = observations()
        expected = {'id': 'summit-startup-fixture', 'boot': 1, 'nonce': args.run_token, 'previousNonce': None, 'granted': True}
        if initial != {'reports': [expected], 'errors': []}:
            raise RuntimeError('Setup did not execute exactly one approved background: ' + repr(initial))
        catalog = profile / 'Extensions/catalog.json'
        report['seed_catalog_sha256'] = digest(catalog)
        for boot in (2, 3):
            run_process('browser-startup-' + str(boot - 1), [str(bundle / 'Summit'), '--profile', str(profile)], boot)
        report['catalog_unchanged'] = digest(catalog) == report['seed_catalog_sha256']
        if not report['catalog_unchanged']:
            raise RuntimeError('Automatic startup altered the installed catalog')
        report['observations'] = observations()
        if len(report['observations']['reports']) != 3 or report['observations']['errors']:
            raise RuntimeError('Unexpected extension executions')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(path)) == expected for path, expected in before.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = (report['passed'] and report['native_crash_log']['passed']
                            and report['bundle_unchanged'] and report['inputs_unchanged'])
        save()
    return 0 if report['passed'] else 1


class Reports(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        request = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(request.query)
        if query.get('run') != [self.server.token]:
            self.send_error(403)
            return
        with self.server.lock:
            if request.path == '/report':
                try:
                    payload = query['payload'][0]
                    if len(payload) > 4096:
                        raise ValueError('oversized report')
                    item = json.loads(payload)
                    if item.get('id') != 'summit-startup-fixture' or item.get('nonce') != self.server.token:
                        raise ValueError('unexpected extension identity or nonce')
                    self.server.reports.append(item)
                except (KeyError, ValueError, AttributeError) as error:
                    self.server.errors.append(str(error))
            elif request.path != '/status':
                self.send_error(404)
                return
            body = json.dumps({'reports': self.server.reports, 'errors': self.server.errors}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    token = secrets.token_hex(12)
    output = ROOT / '.vm' / ('modern-extension-startup-' + token)
    output.mkdir()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Reports)
    server.token, server.reports, server.errors, server.lock = token, [], [], threading.Lock()
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    base_url = 'http://10.0.2.2:' + str(server.server_port)
    try:
        names = ('tests/ExtensionStartupSeed.cpp', 'tests/ModernCloseTests.cpp', 'tools/test-modern-extension-startup.py',
                 'tools/test-modern-close-native.py', 'tools/native_crash_log.py')
        files = {name: (ROOT / name).read_bytes() for name in names}
        original = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        for path in sorted((ROOT / 'tests/fixtures/extensions/startup').iterdir()):
            original[str(path.relative_to(ROOT))] = digest(path)
            files['package/' + path.name] = path.read_text().replace('@NONCE@', token).replace(
                '@REPORT_URL@', base_url + '/report?run=' + token).encode()
        inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
        files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
        archive = io.BytesIO()
        with tarfile.open(fileobj=archive, mode='w:gz') as stream:
            for name, data in files.items():
                item = tarfile.TarInfo(name)
                item.size, item.mode = len(data), 0o600
                stream.addfile(item, io.BytesIO(data))
        stage = remote('mktemp -d /boot/home/summit/extension-startup-inputs.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
        if not stage.startswith('/boot/home/summit/extension-startup-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
            raise RuntimeError('Unexpected native staging directory')
        remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
        (output / 'stage.json').write_text(json.dumps({'stage': stage, 'source_sha256': original}, indent=2) + '\n')
        print(json.dumps({'stage': stage, 'output': str(output), 'base_url': base_url}), flush=True)
        command = ['python3.10', stage + '/tools/test-modern-extension-startup.py', '--native', '--bundle', args.bundle,
                   '--base-url', base_url, '--run-token', token]
        with (output / 'native.log').open('w') as log:
            result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
        print((output / 'native.log').read_text(), end='', flush=True)
        fetched = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True)
        evidence = json.loads(fetched.stdout) if fetched.returncode == 0 else {'passed': False, 'error': 'Missing native result'}
        unchanged = all(digest(ROOT / name) == expected for name, expected in original.items())
        with server.lock:
            observed = {'reports': list(server.reports), 'errors': list(server.errors)}
        report = {'native': evidence, 'source_sha256': original, 'sources_unchanged': unchanged,
                  'observations': observed, 'run': token,
                  'passed': result.returncode == 0 and evidence.get('passed') is True and unchanged
                            and observed == evidence.get('observations')}
        (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json')}), flush=True)
        return 0 if report['passed'] else 1
    finally:
        server.shutdown()
        worker.join(timeout=5)
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('--base-url', help=argparse.SUPPRESS)
    parser.add_argument('--run-token', help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    sys.exit(native(arguments) if arguments.native else host(arguments))
