#!/usr/bin/env python3
"""Verify installed script injection into existing documents, isolated worlds, and denied hosts."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import io
import json
import os
from pathlib import Path
import secrets
import shlex
import shutil
import signal
import subprocess
import tarfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = Path(__file__).name


def digest(path):
    with path.open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def native(args):
    from native_crash_log import NativeCrashLog
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, _, watched = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    if not all(digest(ROOT / name) == expected for name, expected in inputs.items()):
        raise RuntimeError('Staged input changed')
    executable = ROOT / 'ModernExtensionTabScriptTests'
    source = bundle / 'source'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
               '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
               str(ROOT / 'tests/ModernExtensionTabScriptTests.cpp'), '-lbe', '-o', str(executable)]
    temporary = ROOT / 'tmp'
    temporary.mkdir(mode=0o700)
    compiled = subprocess.run(command, capture_output=True, text=True, env=dict(os.environ, TMPDIR=str(temporary)))
    report = {'scope': __doc__, 'bundle': str(bundle), 'inputs': inputs,
              'compile_command': command, 'compile_exit': compiled.returncode,
              'compile_output': compiled.stdout + compiled.stderr, 'passed': False, 'runtime_verified': False}
    def save():
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    save()
    print(report['compile_output'], end='', flush=True)
    if compiled.returncode:
        return 1
    report['executable_sha256'] = digest(executable)
    if args.compile_only:
        report['bundle_unchanged'] = all(digest(Path(name)) == expected for name, expected in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = report['bundle_unchanged'] and report['inputs_unchanged']
        save()
        return 0 if report['passed'] else 1
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(executable), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    profile = ROOT / 'profile'
    profile.mkdir(mode=0o700)
    control = ROOT / 'control'
    control.mkdir(mode=0o700)
    shutil.copyfile(ROOT / 'package/expected.json', control / 'expected.json')
    token = secrets.token_hex(16)
    server_errors = []
    report_lock = threading.Lock()
    events = []
    observations = []
    loads = {}
    holds = set()
    release_parser = threading.Event()
    release_load = threading.Event()
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def respond(self, data, content_type='application/json'):
            self.send_response(200)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            try: self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError): pass
        def do_OPTIONS(self):
            self.send_response(204)
            self.send_header('Access-Control-Allow-Origin', '*')
            self.send_header('Access-Control-Allow-Headers', 'Content-Type')
            self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
            self.end_headers()
        def do_GET(self):
            if self.path == '/' + token + '/command':
                path = control / 'command.json'
                self.respond(path.read_bytes() if path.exists() else b'{}')
            elif self.path == '/' + token + '/state':
                with report_lock:
                    value = {'primaryReady': any(event.get('kind') == 'primary-ready' for event in events),
                             'events': list(events), 'observations': list(observations), 'loads': dict(loads), 'holds': sorted(holds)}
                self.respond(json.dumps(value).encode())
            elif self.path in ('/' + token + '/hold-parser.js', '/' + token + '/hold-load.svg'):
                parser = self.path.endswith('.js')
                with report_lock:
                    holds.add('parser' if parser else 'load')
                    if parser:
                        (control / 'parser-held.json').write_text('{"held":true}\n')
                released = release_parser if parser else release_load
                if not released.wait(90):
                    server_errors.append('Timed out releasing held page resource')
                if parser:
                    self.respond(b'document.documentElement.dataset.parserReleased="true";', 'text/javascript')
                else:
                    self.respond(b'<svg xmlns="http://www.w3.org/2000/svg" width="1" height="1"/>', 'image/svg+xml')
            elif self.path.startswith('/' + token + '/target/'):
                suffix = self.path.split('/target/', 1)[1]
                if suffix not in ('setup', 'empty', 'loading', 'a/main', 'a/child', 'b/main', 'b/child'):
                    self.send_error(404); return
                with report_lock:
                    loads[suffix] = loads.get(suffix, 0) + 1
                body = '<!doctype html><html data-label="' + suffix + '"><meta charset="utf-8"><title>Summit script injection</title>'
                body += '<h1>Extension script injection: ' + suffix + '</h1><p>Testing isolated JavaScript in existing documents.</p>'
                if suffix in ('a/main', 'b/main'):
                    prefix = suffix[0]
                    body += '<iframe src="child"></iframe><iframe id="blank" src="about:blank"></iframe>'
                    body += '<iframe srcdoc="&lt;html data-label=&quot;' + prefix + '/srcdoc&quot;&gt;&lt;p&gt;Inline frame&lt;/p&gt;"></iframe>'
                    body += '<script>const blank=document.getElementById("blank");function labelBlank(){blank.contentDocument.documentElement.dataset.label=' + json.dumps(prefix + '/blank') + ';}'
                    body += 'blank.addEventListener("load",labelBlank);labelBlank();</script>'
                observation_url = '/' + token + '/observe'
                body += '<script>var pageOnly="page"; const nonce=' + json.dumps(secrets.token_hex(12)) + ';'
                body += 'function observe(){fetch(' + json.dumps(observation_url) + ',{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({'
                body += 'label:document.documentElement.dataset.label,nonce,pageOnly,primaryType:typeof primaryOnly,otherType:typeof otherOnly,'
                body += 'injected:document.documentElement.dataset.injected||"",denied:document.documentElement.dataset.denied||"",pending:document.documentElement.dataset.pending||""})});}'
                body += 'new MutationObserver(observe).observe(document.documentElement,{attributes:true});observe();</script>'
                if suffix == 'loading':
                    body += '<script src="/' + token + '/hold-parser.js"></script><img src="/' + token + '/hold-load.svg">'
                body += '</html>'
                self.respond(body.encode(), 'text/html; charset=utf-8')
            else: self.send_error(404)
        def do_POST(self):
            prefix = '/' + token + '/report/'
            is_event = self.path == '/' + token + '/event'
            is_observation = self.path == '/' + token + '/observe'
            release = {'/' + token + '/release/parser': release_parser, '/' + token + '/release/load': release_load}.get(self.path)
            phase = self.path[len(prefix):] if self.path.startswith(prefix) else ''
            if not is_event and not is_observation and release is None and phase not in ('initial', 'timing', 'navigated', 'complete', 'failure'):
                self.send_error(404); return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 65536:
                    raise ValueError('Invalid report length')
                value = json.loads(self.rfile.read(length))
                if not isinstance(value, dict): raise ValueError('Report must be an object')
                with report_lock:
                    if release is not None:
                        release.set()
                    elif is_event or is_observation:
                        collection = events if is_event else observations
                        collection.append(value)
                        destination = control / ('events.json' if is_event else 'observations.json')
                        destination.write_text(json.dumps(collection) + '\n')
                    else:
                        if value.get('phase') != phase: raise ValueError('Incorrect phase')
                        destination = control / (phase + '.json')
                        if destination.exists(): raise ValueError('Duplicate phase report')
                        temporary = destination.with_suffix('.tmp')
                        temporary.write_text(json.dumps(value) + '\n')
                        temporary.replace(destination)
                self.respond(b'{}')
            except (ValueError, TypeError) as error:
                server_errors.append(str(error)); self.send_error(400)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    control_url = 'http://127.0.0.1:' + str(server.server_port) + '/' + token
    target_url = control_url + '/target'
    packages = []
    for name in ('package', 'other-package', 'denied-package'):
        package = ROOT / ('installed-' + name)
        shutil.copytree(ROOT / name, package)
        config = package / 'config.js'
        if config.read_text().count('__CONTROL_JSON__') != 1:
            raise RuntimeError('Missing unique control URL placeholder: ' + name)
        config.write_text(config.read_text().replace('__CONTROL_JSON__', json.dumps(control_url)))
        packages.append(package)
    package, other, denied = packages
    package_inputs = {str(path.relative_to(ROOT)): digest(path) for folder in packages
                      for path in folder.rglob('*') if path.is_file()}
    report.update(control_url=control_url, target_url=target_url, runtime_package_sha256=package_inputs)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    environment = dict(os.environ, WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for key in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR', 'SUMMIT_TRACE_EXTENSION_CONSOLE'):
        environment.pop(key, None)
    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id':
            raise RuntimeError('Cannot enumerate native teams')
        result = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group:
                    result.append(pid)
            except ProcessLookupError:
                pass
        return result
    monitor = NativeCrashLog()
    report['forced_cleanup'] = False
    try:
        with (ROOT / 'browser.log').open('w') as browser_log, (ROOT / 'harness.log').open('w') as harness_log:
            process = subprocess.Popen([str(bundle / 'Summit'), '--profile', str(profile), target_url + '/setup'], env=environment,
                stdout=browser_log, stderr=subprocess.STDOUT, start_new_session=True)
            report['pid'] = report['process_group'] = process.pid
            save()
            try:
                command = [str(executable), str(process.pid), str(bundle / 'Summit'), str(package), str(control), target_url, str(other), str(denied)]
                report['harness_command'] = command
                result = subprocess.run(command, stdout=harness_log, stderr=subprocess.STDOUT, timeout=300)
                report['harness_exit'] = result.returncode
                report['browser_exit'] = process.wait(timeout=15)
            finally:
                if process.poll() is None:
                    report['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait(timeout=5)
                report['browser_exit'] = process.returncode
                deadline = time.monotonic() + 10
                while members(process.pid) and time.monotonic() < deadline:
                    time.sleep(.05)
                report['remaining_processes'] = members(process.pid)
                report['group_drained'] = not report['remaining_processes']
                if report['remaining_processes']:
                    report['forced_cleanup'] = True
                    os.killpg(process.pid, signal.SIGKILL)
        report['output'] = (ROOT / 'harness.log').read_text()
        print(report['output'], end='', flush=True)
        report['passed'] = (report.get('harness_exit') == 0 and report.get('browser_exit') == 0
            and report['group_drained'] and not report['forced_cleanup'] and 'EXTENSION_TAB_SCRIPT_RESULT PASS checks=' in report['output'])
    except Exception as error:
        report['error'] = str(error)
    finally:
        report['output'] = (ROOT / 'harness.log').read_text() if (ROOT / 'harness.log').exists() else ''
        report['browser_output'] = (ROOT / 'browser.log').read_text() if (ROOT / 'browser.log').exists() else ''
        release_parser.set()
        release_load.set()
        server.shutdown()
        server.server_close()
        report['phases'] = {name: json.loads((control / (name + '.json')).read_text())
                            for name in ('initial', 'timing', 'navigated', 'complete', 'failure') if (control / (name + '.json')).exists()}
        report['server_errors'] = server_errors
        report['events'] = events
        report['page_observations'] = observations
        report['page_loads'] = loads
        other_events = [event for event in events if event.get('kind') == 'other-ready']
        denied_events = [event for event in events if event.get('kind') == 'denied-ready']
        report['extension_isolation_verified'] = (len(other_events) == 1
            and other_events[0].get('id') == 'summit-tab-script-other'
            and other_events[0].get('result') == [{'primary': 'undefined', 'page': 'undefined',
                'id': 'summit-tab-script-other', 'label': 'a/main'}])
        report['tabs_only_injection_denied'] = (len(denied_events) == 1
            and denied_events[0].get('id') == 'summit-tab-script-denied' and denied_events[0].get('rejected') is True
            and denied_events[0].get('metadata') == target_url + '/a/main'
            and all(not item.get('denied') for item in observations))
        report['page_observed_isolation'] = (any(item.get('label') == 'a/main' and item.get('injected') == 'primary' for item in observations)
            and all(item.get('pageOnly') == 'page' and item.get('primaryType') == 'undefined'
                and item.get('otherType') == 'undefined' for item in observations))
        report['fixture_failures'] = [event for event in events if event.get('kind', '').endswith('-failure')]
        report['runtime_package_unchanged'] = all(digest(ROOT / name) == expected for name, expected in package_inputs.items())
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(name)) == expected for name, expected in watched.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = (report['passed'] and report['native_crash_log']['passed'] and not server_errors and report['extension_isolation_verified']
                            and report['tabs_only_injection_denied'] and report['page_observed_isolation'] and not report['fixture_failures']
                            and report['bundle_unchanged'] and report['inputs_unchanged'] and report['runtime_package_unchanged'])
        report['runtime_verified'] = report['passed']
        save()
    return 0 if report['passed'] else 1


def host(args):
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    names = ('tests/ModernExtensionTabScriptTests.cpp', 'tests/ModernExtensionOverflowTests.cpp',
             'tests/ModernExtensionManagerTests.cpp', 'tests/ModernCloseTests.cpp',
             'tools/' + SCRIPT, 'tools/test-modern-close-native.py', 'tools/native_crash_log.py', 'tools/vm.py')
    files = {name: (ROOT / name).read_bytes() for name in names}
    original = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    for name, target in (('tab-script', 'package'), ('tab-script-other', 'other-package'), ('tab-script-denied', 'denied-package')):
        fixture = ROOT / 'tests/fixtures/extensions' / name
        for path in sorted(fixture.rglob('*')):
            if path.is_file():
                data = path.read_bytes()
                files[target + '/' + str(path.relative_to(fixture))] = data
                original[str(path.relative_to(ROOT))] = hashlib.sha256(data).hexdigest()
    inputs = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files['inputs.json'] = (json.dumps(inputs, indent=2) + '\n').encode()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in files.items():
            item = tarfile.TarInfo(name); item.size = len(data); item.mode = 0o600
            stream.addfile(item, io.BytesIO(data))
    stage = remote('mktemp -d /SummitExtensions/summit/extension-tab-script-browser.XXXXXXXX', capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/SummitExtensions/summit/extension-tab-script-browser.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Invalid native stage')
    output = ROOT / '.vm' / ('modern-extension-tab-script-' + secrets.token_hex(12))
    output.mkdir()
    (output / 'stage.json').write_text(json.dumps({'stage': stage, 'source_sha256': original}, indent=2) + '\n')
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print(json.dumps({'stage': stage, 'output': str(output)}), flush=True)
    command = ['python3.10', stage + '/tools/' + SCRIPT, '--native', '--bundle', args.bundle]
    if args.compile_only:
        command.append('--compile-only')
    else:
        subprocess.run(['python3', str(ROOT / 'tools/vm.py'), 'key', 'shift'], check=True)
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    data = remote('cat ' + shlex.quote(stage + '/result.json'), capture_output=True, text=True, check=True)
    report = {'native': json.loads(data.stdout), 'sources_unchanged': all(digest(ROOT / name) == expected for name, expected in original.items()),
              'source_sha256': original}
    report['passed'] = result.returncode == 0 and report['native']['passed'] and report['sources_unchanged']
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'result': str(output / 'result.json')}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True)
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--native', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    raise SystemExit(native(args) if args.native else host(args))
