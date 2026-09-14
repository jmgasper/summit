#!/usr/bin/env python3
"""Compile the external native harness; run only with a frozen full-browser bundle."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def inside(root, relative):
    name = pathlib.PurePosixPath(relative)
    if name.is_absolute() or '..' in name.parts:
        raise RuntimeError('Invalid frozen input path: ' + relative)
    path = root / name
    if root not in path.resolve(strict=True).parents:
        raise RuntimeError('Frozen input leaves its root: ' + relative)
    return path


def verified_bundle(path):
    bundle = pathlib.Path(path).resolve(strict=True)
    manifest = json.loads((bundle / 'build-manifest.json').read_text())
    if manifest.get('kind') != 'modern-native-browser':
        raise RuntimeError('Close tests require the full modern-native-browser bundle')
    files = manifest['bundled_sha256']
    if not {'Summit', 'WebProcess', 'NetworkProcess', 'run-browser.sh'}.issubset(files):
        raise RuntimeError('Frozen full-browser executables are missing')
    if not any(name.startswith('lib/libWebKit.so') for name in files):
        raise RuntimeError('Private modern WebKit library is missing')
    before = {str(bundle / 'build-manifest.json'): digest(bundle / 'build-manifest.json')}
    for relative, expected in files.items():
        source = inside(bundle, relative)
        if digest(source) != expected:
            raise RuntimeError('Frozen bundle hash mismatch: ' + relative)
        before[str(source)] = expected
    for relative, expected in manifest.get('symlinks', {}).items():
        source = inside(bundle, relative)
        if not source.is_symlink() or os.readlink(source) != expected:
            raise RuntimeError('Frozen symlink mismatch: ' + relative)
    for relative, expected in manifest['inputs']['sha256'].items():
        source = inside(bundle / 'source', relative)
        if digest(source) != expected:
            raise RuntimeError('Frozen browser source mismatch: ' + relative)
        before[str(source)] = expected
    for required in ('src/ui/Messages.h', 'src/ui/BrowserWindow.cpp', 'vendor/nlohmann/json.hpp'):
        if required not in manifest['inputs']['sha256']:
            raise RuntimeError('Frozen browser test dependency is missing: ' + required)
    return bundle, manifest, before


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--compile-only', action='store_true')
    mode.add_argument('--bundle')
    parser.add_argument('--base-url')
    parser.add_argument('--run-token')
    parser.add_argument('--downloads', action='store_true', help='Run the full-browser download harness')
    args = parser.parse_args()
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for relative, expected in inputs['sha256'].items():
        if digest(inside(ROOT, relative)) != expected:
            raise RuntimeError('Staged close input changed: ' + relative)
    before = {}
    bundle = manifest = None
    source = ROOT
    if args.bundle:
        if not args.base_url or not args.run_token:
            raise RuntimeError('Runtime requires dedicated fixture URL and run token')
        bundle, manifest, before = verified_bundle(args.bundle)
        source = bundle / 'source'
    harness = 'ModernBrowserDownloadTests' if args.downloads else 'ModernCloseTests'
    target = ROOT / (harness + ('.o' if args.compile_only else ''))
    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
               '-I' + str(source / 'src'), '-I' + str(source / 'vendor'),
               str(ROOT / 'tests' / (harness + '.cpp'))]
    command += ['-c'] if args.compile_only else ['-lbe']
    command += ['-o', str(target)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    report = {'kind': 'compile-only' if args.compile_only else 'modern-browser-downloads' if args.downloads else 'modern-close-runtime', 'stage': str(ROOT),
              'inputs': inputs, 'compile_command': command, 'compile_exit': result.returncode}
    report_path = ROOT / 'result.json'
    def save():
        report_path.write_text(json.dumps(report, indent=2) + '\n')
    save()
    if result.returncode:
        return 1
    report['artifact_sha256'] = digest(target)
    if args.compile_only:
        report['passed'] = True
        save()
        return 0

    report['bundle'] = str(bundle)
    report['engine'] = manifest['inputs']['engine']
    unused = subprocess.run([str(target), '--check-executable-unused', str(bundle / 'Summit')],
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    print(unused.stdout, end='', flush=True)
    report['unused_check_exit'] = unused.returncode
    if unused.returncode:
        report['passed'] = False
        save()
        return 1
    profile = ROOT / 'profile'
    profile.mkdir(mode=0o700)
    environment = os.environ.copy()
    environment['WEBKIT_EXEC_PATH'] = str(bundle)
    environment['LIBRARY_PATH'] = str(bundle / 'lib') + ':/boot/system/lib'
    browser_command = [str(bundle / 'Summit'), '--profile', str(profile)]
    report['browser_command'] = browser_command
    report['profile'] = str(profile)
    with (ROOT / 'browser.log').open('w') as browser_log:
        browser = subprocess.Popen(browser_command, env=environment, stdout=browser_log, stderr=subprocess.STDOUT)
        report['browser_team'] = browser.pid
        command = [str(target), str(browser.pid), str(bundle / 'Summit'), args.base_url, args.run_token, str(profile)]
        report['run_command'] = command
        save()
        try:
            result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=420)
            output = result.stdout
            report['runtime_exit'] = result.returncode
        except subprocess.TimeoutExpired as error:
            output = error.stdout or ''
            if isinstance(output, bytes):
                output = output.decode('utf-8', errors='replace')
            report['runtime_exit'] = None
            report['timeout'] = True
        finally:
            if browser.poll() is None:
                # The failed harness attempts queued native close first. Any
                # remaining process is solely this runner's direct child.
                report['forced_cleanup'] = True
                browser.terminate()
                try:
                    browser.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    browser.kill()
                    browser.wait(timeout=10)
            report['browser_exit'] = browser.wait()
    (ROOT / 'runtime.log').write_text(output)
    print(output, end='', flush=True)
    verified_bundle(str(bundle))
    report['bundle_unchanged'] = all(digest(pathlib.Path(path)) == expected for path, expected in before.items())
    report['passed'] = (report['runtime_exit'] == 0 and report['browser_exit'] == 0
                        and not report.get('forced_cleanup') and report['bundle_unchanged']
                        and ('DOWNLOAD_UI_RESULT PASS checks=' if args.downloads else 'CLOSE_RESULT PASS checks=') in output)
    save()
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print('FAIL close test runner: ' + str(error), file=sys.stderr)
        sys.exit(1)
