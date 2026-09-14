#!/usr/bin/env python3
"""Compile context tests; execute only against an explicitly supplied frozen bundle."""
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
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def verify_symlinks(bundle, manifest):
    for relative, expected in manifest.get('symlinks', {}).items():
        name = pathlib.PurePosixPath(relative)
        if name.is_absolute() or '..' in name.parts:
            raise RuntimeError('Invalid symlink path in frozen bundle manifest')
        path = bundle / name
        if not path.is_symlink() or os.readlink(path) != expected:
            raise RuntimeError('Frozen bundle symlink mismatch: ' + relative)
        target = path.resolve(strict=True)
        if bundle not in target.parents:
            raise RuntimeError('Frozen bundle symlink leaves the bundle: ' + relative)


def verified_bundle(path):
    bundle = pathlib.Path(path).resolve(strict=True)
    manifest = json.loads((bundle / 'build-manifest.json').read_text())
    if not manifest.get('kind', '').startswith('modern-native-'):
        raise RuntimeError('Expected a frozen native modern WebKit bundle')
    files = manifest['bundled_sha256']
    if not {'WebProcess', 'NetworkProcess'}.issubset(files):
        raise RuntimeError('The bundle does not contain both modern process executables')
    if not any(name.startswith('lib/libWebKit.so') for name in files):
        raise RuntimeError('The bundle does not contain its private modern WebKit library')
    verify_symlinks(bundle, manifest)
    before = {}
    for relative, expected in files.items():
        name = pathlib.PurePosixPath(relative)
        if name.is_absolute() or '..' in name.parts:
            raise RuntimeError('Invalid path in frozen bundle manifest')
        source = bundle / name
        if digest(source) != expected:
            raise RuntimeError('Frozen bundle hash mismatch: ' + relative)
        before[str(source)] = expected
    include = bundle / 'source/include'
    header_hashes = manifest['inputs']['sha256']
    for name in ('WebKitView.h', 'WebKitContext.h', 'WKBase.h', 'WKBaseHaiku.h', 'WKDeclarationSpecifiers.h'):
        relative = 'include/WebKit/' + name
        path = include / 'WebKit' / name
        if relative not in header_hashes or digest(path) != header_hashes[relative]:
            raise RuntimeError('Frozen public context header mismatch: ' + name)
        before[str(path)] = header_hashes[relative]
    return bundle, include, manifest, before


def main():
    from native_crash_log import NativeCrashLog
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--compile-only', action='store_true')
    mode.add_argument('--bundle')
    parser.add_argument('--base-url')
    parser.add_argument('--run-token')
    args = parser.parse_args()
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    for relative, expected in inputs['sha256'].items():
        if digest(ROOT / relative) != expected:
            raise RuntimeError('Staged test input changed: ' + relative)
    include = ROOT / 'include'
    before = {}
    manifest = None
    bundle = None
    if args.bundle:
        if not args.base_url or not args.run_token:
            raise RuntimeError('Running requires the dedicated fixture URL and run token')
        bundle, include, manifest, before = verified_bundle(args.bundle)

    command = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-DBUILDING_HAIKU__=1',
               '-I' + str(include), str(ROOT / 'tests/ModernContextTests.cpp')]
    if args.compile_only:
        target = ROOT / 'ModernContextTests.o'
        command += ['-c', '-o', str(target)]
    else:
        target = ROOT / 'ModernContextTests'
        command += ['-L' + str(bundle / 'lib'), '-lWebKit', '-lbe', '-lnetwork',
                    '-Wl,-rpath,' + str(bundle / 'lib'), '-o', str(target)]
    compile_result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(compile_result.stdout, end='', flush=True)
    report = {'kind': 'compile-only' if args.compile_only else 'modern-context-runtime',
              'stage': str(ROOT), 'compile_command': command, 'compile_exit': compile_result.returncode,
              'inputs': inputs}
    if compile_result.returncode:
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        return 1
    report['artifact_sha256'] = digest(target)
    if args.compile_only:
        report['object'] = str(target)
        (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report), flush=True)
        return 0

    report['bundle'] = str(bundle)
    report['engine'] = manifest['inputs']['engine']
    profiles = ROOT / 'profiles'
    profiles.mkdir(mode=0o700)
    report['profile_root'] = str(profiles)
    environment = os.environ.copy()
    for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
        environment.pop(name, None)
    environment['WEBKIT_EXEC_PATH'] = str(bundle)
    environment['LIBRARY_PATH'] = str(bundle / 'lib') + ':/boot/system/lib'
    runtime = [str(target), args.base_url, args.run_token, str(profiles)]
    report['run_command'] = runtime
    print('Running context tests with frozen bundle: ' + str(bundle), flush=True)
    crash_log = NativeCrashLog()
    # A crashed orphan helper can retain the parent's stdout descriptor after
    # the harness has already failed. A file lets us observe the harness exit
    # without waiting for that orphan to close an inherited pipe.
    runtime_log = ROOT / 'runtime.log'
    with runtime_log.open('w') as log:
        try:
            result = subprocess.run(runtime, env=environment, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=850)
            report['runtime_exit'] = result.returncode
        except subprocess.TimeoutExpired:
            report['runtime_exit'] = None
            report['timeout'] = True
    output = runtime_log.read_text(errors='replace')
    print(output, end='', flush=True)
    report['native_crash_log'] = crash_log.finish()
    report['private_hint_exists'] = (profiles / 'private-must-not-exist').exists()
    report['profile_files'] = sorted(str(path.relative_to(profiles)) for path in profiles.rglob('*'))
    verify_symlinks(bundle, manifest)
    report['bundle_unchanged'] = all(digest(pathlib.Path(path)) == expected for path, expected in before.items())
    report['passed'] = (report['runtime_exit'] == 0 and not report['private_hint_exists']
                        and report['bundle_unchanged'] and report['native_crash_log']['passed']
                        and 'CONTEXT_RESULT PASS steps=12' in output)
    (ROOT / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        print('FAIL context test runner: ' + str(error), file=sys.stderr)
        sys.exit(1)
