"""Native half of test-modern-extension-startup.py --manager."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import threading
import time
import urllib.request
from native_crash_log import NativeCrashLog

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args):
    spec = importlib.util.spec_from_file_location('close_native', ROOT / 'tools/test-modern-close-native.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    bundle, manifest, before = helper.verified_bundle(args.bundle)
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for name, expected in inputs.items():
        if digest(ROOT / name) != expected:
            raise RuntimeError('Staged input changed: ' + name)
    source = bundle / 'source'
    executable = ROOT / 'ModernExtensionManagerTests'
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
        '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
        str(ROOT / 'tests/ModernExtensionManagerTests.cpp'), '-lbe', '-o', str(executable)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    report = {'scope': 'actual native extension installer/manager controls and file picker selecting XPI/folder packages, public SDK execution, catalog persistence and browser restarts',
              'bundle': str(bundle), 'inputs': inputs, 'compile_command': command, 'compile_exit': compiled.returncode,
              'compile_output': compiled.stdout + compiled.stderr, 'runs': [], 'passed': False}
    result_path = ROOT / 'result.json'
    def save():
        result_path.write_text(json.dumps(report, indent=2) + '\n')
    save()
    print(report['compile_output'], end='', flush=True)
    if compiled.returncode:
        return 1
    report['executable_sha256'] = digest(executable)
    for name in ('Summit', 'WebProcess', 'NetworkProcess'):
        subprocess.run([str(executable), '--check-executable-unused', str(bundle / name)], check=True, timeout=30)
    profile = ROOT / 'profile'
    profile.mkdir(mode=0o700)
    report['profile'] = str(profile)
    environment = dict(os.environ, WEBKIT_EXEC_PATH=str(bundle), LIBRARY_PATH=str(bundle / 'lib') + ':/boot/system/lib')
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    observations = ROOT / 'observations.json'
    observations.write_text(json.dumps({'reports': [], 'errors': []}))
    stop = threading.Event()

    def fetch():
        with urllib.request.urlopen(args.base_url + '/status?run=' + args.run_token, timeout=5) as response:
            return json.load(response)

    def publish_observations():
        while not stop.is_set():
            try:
                data = fetch()
            except Exception as error:
                data = {'reports': [], 'errors': [str(error)]}
            temporary = observations.with_suffix('.tmp')
            temporary.write_text(json.dumps(data))
            temporary.replace(observations)
            stop.wait(.05)

    def members(group):
        lines = subprocess.check_output(['ps', '-o', 'Id'], text=True).splitlines()
        if not lines or lines[0].strip() != 'Id':
            raise RuntimeError('Could not enumerate native teams')
        found = []
        for line in lines[1:]:
            pid = int(line.strip())
            try:
                if os.getpgid(pid) == group:
                    found.append(pid)
            except ProcessLookupError:
                pass
        return found

    publisher = threading.Thread(target=publish_observations, daemon=True)
    monitor = NativeCrashLog()
    publisher.start()
    try:
        for phase in ('install', 'remove', 'after-removal'):
            record = {'phase': phase, 'forced_cleanup': False}
            report['runs'].append(record)
            with (ROOT / (phase + '-browser.log')).open('w') as browser_log, (ROOT / (phase + '-harness.log')).open('w') as harness_log:
                browser = subprocess.Popen([str(bundle / 'Summit'), '--profile', str(profile)], env=environment,
                    stdout=browser_log, stderr=subprocess.STDOUT, start_new_session=True)
                record['pid'] = record['process_group'] = browser.pid
                record['command'] = [str(executable), str(browser.pid), str(bundle / 'Summit'), str(profile),
                                     str(ROOT / 'package'), str(observations), args.run_token, phase]
                save()
                try:
                    result = subprocess.run(record['command'], stdout=harness_log, stderr=subprocess.STDOUT, timeout=180)
                    record['harness_exit'] = result.returncode
                    record['browser_exit'] = browser.wait(timeout=15)
                except Exception as error:
                    record['error'] = str(error)
                finally:
                    if browser.poll() is None:
                        record['forced_cleanup'] = True
                        os.killpg(browser.pid, signal.SIGTERM)
                        try:
                            browser.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            os.killpg(browser.pid, signal.SIGKILL)
                            browser.wait(timeout=5)
                    record['browser_exit'] = browser.returncode
                    deadline = time.monotonic() + 10
                    while members(browser.pid) and time.monotonic() < deadline:
                        time.sleep(.05)
                    record['remaining_processes'] = members(browser.pid)
                    record['group_drained'] = not record['remaining_processes']
                    if record['remaining_processes']:
                        record['forced_cleanup'] = True
                        os.killpg(browser.pid, signal.SIGKILL)
            record['output'] = (ROOT / (phase + '-harness.log')).read_text()
            record['browser_output'] = (ROOT / (phase + '-browser.log')).read_text()
            marker = 'EXTENSION_MANAGER_RESULT PASS phase=' + phase + ' checks='
            record['passed'] = (record.get('harness_exit') == 0 and record['browser_exit'] == 0
                and record['group_drained'] and not record['forced_cleanup'] and marker in record['output'])
            save()
            print(record['output'], end='', flush=True)
            if not record['passed']:
                raise RuntimeError('Native extension manager phase failed: ' + phase)
        report['observations'] = fetch()
        expected = [{'id': 'summit-startup-fixture', 'boot': i, 'nonce': args.run_token,
                     'previousNonce': None if i == 1 else args.run_token, 'granted': True, 'optionalGranted': False}
                    for i in (1, 2, 3)]
        if report['observations'] != {'reports': expected, 'errors': []}:
            raise RuntimeError('Unexpected background execution or restored permissions')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        stop.set()
        publisher.join(timeout=6)
        report['native_crash_log'] = monitor.finish()
        report['bundle_unchanged'] = all(digest(Path(path)) == expected for path, expected in before.items())
        report['inputs_unchanged'] = all(digest(ROOT / name) == expected for name, expected in inputs.items())
        report['passed'] = (report['passed'] and report['native_crash_log']['passed']
                            and report['bundle_unchanged'] and report['inputs_unchanged'])
        save()
    return 0 if report['passed'] else 1
