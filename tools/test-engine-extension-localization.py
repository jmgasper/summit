#!/usr/bin/env python3
"""Exercise the actual shared localizer with matching frozen WebKit/WTF/ICU dependencies.

The candidate's localizer and ICU locale implementation are compiled with hidden
symbols into the helper. Other real dependencies use the verified baseline bundle.
No extension/context objects cross the executable/library boundary. This tests
shared localization behavior, not the i18n JavaScript API or browser integration.
Compile first, then run the returned stage with exclusive use of the test VM.
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
SOURCE = Path('Source/WebKit/Shared/Extensions')
SCRIPT = Path(__file__).name


def digest(path):
    with path.open('rb') as stream:
        value = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def unchanged(snapshot):
    return all(digest(Path(name)) == expected for name, expected in snapshot.items())


def native(run):
    from native_crash_log import NativeCrashLog
    output = ROOT / 'sources'
    manifest = json.loads((output / 'manifest.json').read_text())
    engine = Path(manifest['engine'])
    build = engine / 'WebKitBuild/Modern'
    bundle = Path(manifest['bundle'])
    sources = {str(output / name): entry['sha256'] for name, entry in manifest['files'].items()}
    libraries = {str(bundle / name): expected for name, expected in manifest['libraries'].items()}
    if not unchanged(sources) or not unchanged(libraries):
        raise RuntimeError('Staged source or frozen dependency changed')
    result_path = output / ('runtime.json' if run else 'compile.json')
    if result_path.exists():
        raise RuntimeError('Refusing to overwrite an earlier result')
    if run:
        report = json.loads((output / 'compile.json').read_text())
        if not report.get('compiled') or not unchanged(report['native_inputs']) or not unchanged(report['objects']):
            raise RuntimeError('Native inputs or compiled objects changed')
        executable = output / 'run'
        if digest(executable) != report['executable_sha256']:
            raise RuntimeError('Compiled executable changed')
        environment = os.environ.copy()
        environment['LIBRARY_PATH'] = str(bundle / 'lib') + ':/boot/system/lib'
        for name in ('LD_PRELOAD', 'LD_PRELOAD_ADDONS', 'DISABLE_ASLR'):
            environment.pop(name, None)
        crash_log = NativeCrashLog()
        try:
            result = subprocess.run([str(executable)], env=environment, timeout=45, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            report['exit'], report['output'] = result.returncode, result.stdout
        except subprocess.TimeoutExpired as error:
            report['exit'] = None
            report['output'] = (error.stdout or b'').decode(errors='replace') if isinstance(error.stdout, bytes) else (error.stdout or '')
            report['timed_out'] = True
        report['native_crash_log'] = crash_log.finish()
        report['inputs_unchanged'] = unchanged(report['native_inputs'])
        report['sources_unchanged'] = unchanged(sources)
        report['libraries_unchanged'] = unchanged(libraries)
        report['passed'] = (report['exit'] == 0 and report['native_crash_log']['passed']
                            and report['inputs_unchanged'] and report['sources_unchanged'] and report['libraries_unchanged'])
        report['runtime_verified'] = report['passed']
        result_path.write_text(json.dumps(report, indent=2) + '\n')
        print(report['output'], end='', flush=True)
        return 0 if report['passed'] else 1

    watched = [build / 'build.ninja', build / 'cmakeconfig.h', engine / '.summit-source-manifest.json',
               engine / 'Source/WebKit/WebKitPrefix.h', engine / 'Source/WebKit/config.h',
               engine / 'Source/WebCore/platform/text/PlatformLocale.h',
               engine / SOURCE / 'WebExtensionUtilities.cpp']
    snapshot = {str(path): digest(path) for path in watched}
    configured = json.loads((engine / '.summit-source-manifest.json').read_text())
    if configured['patch_sha256'] != manifest['baseline_patch']:
        raise RuntimeError('Configured engine and frozen baseline patches differ')
    if '#define ENABLE_WK_WEB_EXTENSIONS 1' not in (build / 'cmakeconfig.h').read_text():
        raise RuntimeError('An extension-enabled native configuration is required')
    fields = {}
    target = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'
    with (build / 'build.ninja').open() as stream:
        for line in stream:
            if line.startswith('build ' + target + ':'):
                for line in stream:
                    if not line.strip():
                        break
                    key, _, value = line.strip().partition(' = ')
                    fields[key] = value
                break
    raw = iter(shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS'))))
    flags = []
    for flag in raw:
        if flag == '-include':
            next(raw)
        elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
            flags.append(flag)
    flags = ['-I' + str(output), '-iquote', str(output),
             '-idirafter', str(build / 'WebCore/PrivateHeaders/WebCore'),
             '-iquote', str(engine / 'Source/WebCore/platform/text'), *flags,
             '-include', str(engine / 'Source/WebKit/config.h'),
             '-ffunction-sections', '-fdata-sections', '-fvisibility=hidden', '-fvisibility-inlines-hidden',
             '-fdiagnostics-color=never', '-fmax-errors=5']
    report = {'scope': __doc__, 'source_manifest': manifest, 'native_inputs': snapshot,
              'compile_results': [], 'compiled': False, 'runtime_verified': False}
    objects = []
    for name in ('WebExtensionLocalization.cpp', 'LocaleICU.cpp', 'EngineExtensionLocalizationTests.cpp'):
        obj = output / (Path(name).stem + '.o')
        command = ['c++', *flags, '-c', str(output / name), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(result.stdout, end='', flush=True)
        report['compile_results'].append({'source': name, 'exit': result.returncode, 'output': result.stdout})
        objects.append(obj)
    if not any(unit['exit'] for unit in report['compile_results']):
        executable = output / 'run'
        # Archive-local symbols cannot interpose on definitions in the frozen
        # WebKit library. This also hides Locale's explicit export decoration.
        archive = output / 'libCandidate.a'
        subprocess.run(['ar', 'rcs', str(archive), *map(str, objects[:-1])], check=True)
        link = subprocess.run(['c++', str(objects[-1]), str(archive), str(bundle / 'lib/libWebKit.so'),
                               str(bundle / 'lib/libJavaScriptCore.so'), str(bundle / 'lib/libicuuc.so'),
                               str(bundle / 'lib/libicui18n.so'), '-lbe', '-lnetwork',
                               '-Wl,--exclude-libs,ALL',
                               '-Wl,--no-export-dynamic', '-Wl,--gc-sections',
                               '-Wl,-rpath,' + str(bundle / 'lib'), '-o', str(executable)],
                              text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        report['link'] = {'exit': link.returncode, 'output': link.stdout}
        print(link.stdout, end='', flush=True)
        if not link.returncode:
            symbols = subprocess.check_output(['nm', '-C', str(executable)], text=True)
            local = [line for line in symbols.splitlines() if 'WebExtensionLocalization::localizedStringForKey(' in line]
            if len(local) != 1 or ' t ' not in local[0] or not local[0].endswith(', bool)'):
                raise RuntimeError('The candidate lookup did not link as an executable-local definition')
            report['candidate_lookup_symbol'] = local[0]
            locale = [line for line in symbols.splitlines() if 'WebCore::Locale::create(' in line]
            if len(locale) != 1 or ' t ' not in locale[0]:
                raise RuntimeError('The candidate ICU locale factory did not link as an executable-local definition')
            report['candidate_locale_symbol'] = locale[0]
            report['objects'] = {str(path): digest(path) for path in objects}
            report['executable_sha256'] = digest(executable)
            report['compiled'] = unchanged(snapshot) and unchanged(sources) and unchanged(libraries)
    result_path.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['compiled'] else 1


def remote(command, **kwargs):
    return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)


def invoke(stage, run):
    if not re.fullmatch(r'/boot/home/summit/extension-localization-tests\.[A-Za-z0-9]+', stage):
        raise RuntimeError('Unexpected native stage: ' + stage)
    output = ROOT / '.vm' / Path(stage).name
    output.mkdir(exist_ok=True)
    mode = 'runtime' if run else 'compile'
    with (output / (mode + '.log')).open('x') as log:
        result = remote(shlex.join(['python3.10', stage + '/tools/' + SCRIPT, '--native-run' if run else '--native-compile']),
                        stdout=log, stderr=subprocess.STDOUT)
    print((output / (mode + '.log')).read_text(), end='', flush=True)
    read = remote('cat ' + shlex.quote(stage + '/sources/' + mode + '.json'), text=True, capture_output=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native report'}}
    manifest = report['native'].get('source_manifest', {})
    report['host_sources_unchanged'] = all(digest(Path(entry['source'])) == entry['sha256'] for entry in manifest.get('files', {}).values())
    report['passed'] = result.returncode == 0 and bool(manifest) and report['host_sources_unchanged']
    (output / (mode + '.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'stage': stage, 'result': str(output / (mode + '.json')), 'passed': report['passed']}), flush=True)
    return 0 if report['passed'] else 1


def host(args):
    if args.run_stage:
        return invoke(args.run_stage, True)
    if not args.bundle_report:
        raise RuntimeError('--bundle-report is required for compilation')
    baseline = json.loads(Path(args.bundle_report).read_text())
    files = {}
    for name in ('WebExtensionLocalization.cpp', 'WebExtensionLocalization.h'):
        source = Path(args.overlay).resolve() / SOURCE / name if args.overlay else ROOT / '.cache/WebKit' / SOURCE / name
        if not source.exists():
            source = ROOT / '.cache/WebKit' / SOURCE / name
        files[name] = source
    test = ROOT / 'tests/EngineExtensionLocalizationTests.cpp'
    files[test.name] = test
    locale = Path('Source/WebCore/platform/text/LocaleICU.cpp')
    candidate_locale = Path(args.overlay).resolve() / locale if args.overlay else ROOT / '.cache/WebKit' / locale
    files['LocaleICU.cpp'] = candidate_locale if candidate_locale.exists() else ROOT / '.cache/WebKit' / locale
    manifest = {'engine': args.engine_root, 'bundle': baseline['bundle'],
                'baseline_patch': baseline['inputs']['engine']['patch']['sha256'],
                'bundle_report_sha256': digest(Path(args.bundle_report)),
                'libraries': {name: value for name, value in baseline['bundled_sha256'].items() if name.startswith('lib/')},
                'files': {name: {'source': str(path), 'sha256': digest(path)} for name, path in files.items()}}
    contents = {'sources/' + name: path.read_bytes() for name, path in files.items()}
    contents['sources/manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name in (SCRIPT, 'native_crash_log.py'):
        contents['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in contents.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            stream.addfile(info, io.BytesIO(data))
    stage = remote('mktemp -d /boot/home/summit/extension-localization-tests.XXXXXXXX', check=True,
                   text=True, capture_output=True).stdout.strip()
    if not re.fullmatch(r'/boot/home/summit/extension-localization-tests\.[A-Za-z0-9]+', stage):
        raise RuntimeError('Unexpected native stage: ' + stage)
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native localization helper stage: ' + stage, flush=True)
    return invoke(stage, False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-compile', action='store_true')
    parser.add_argument('--native-run', action='store_true')
    parser.add_argument('--run-stage')
    parser.add_argument('--overlay')
    parser.add_argument('--bundle-report')
    parser.add_argument('--engine-root', default='/boot/home/summit-webkit-extensions')
    args = parser.parse_args()
    raise SystemExit(native(args.native_run) if args.native_run or args.native_compile else host(args))
