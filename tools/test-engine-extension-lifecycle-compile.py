#!/usr/bin/env python3
"""Compile native extension lifecycle sources in isolation; never link feature-mismatched objects."""
import argparse
import hashlib
import importlib.util
import io
import json
import pathlib
import shlex
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
UNITS = ('WebExtensionController.cpp', 'WebExtensionContext.cpp',
         'WebExtensionMatchPatternProcessPool.cpp', 'WebExtensionContextProxy.cpp',
         'WebExtensionControllerProxy.cpp')
EXTRA_UNITS = ('haiku/WebExtensionURLSchemeHandlerHaiku.cpp', 'haiku/WebExtensionContextHaiku.cpp',
               'haiku/WebExtensionStateHaiku.cpp')
DEFAULT_ENGINE = '/boot/home/summit-webkit'


def native(units=None, engine_root=DEFAULT_ENGINE):
    spec = importlib.util.spec_from_file_location('extension_compile_probe',
        ROOT / 'tools/test-engine-extension-manifest-core.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    probe.OUTPUT = ROOT / 'sources'
    probe.SOURCES = UNITS + EXTRA_UNITS
    probe.ENGINE = pathlib.Path(engine_root)
    probe.BUILD = probe.ENGINE / 'WebKitBuild/Modern'
    manifest = json.loads((probe.OUTPUT / 'source-manifest.json').read_text())
    engine = json.loads((probe.ENGINE / '.summit-source-manifest.json').read_text())
    if engine['patch_sha256'] != manifest['host_engine_patch_sha256']:
        raise RuntimeError('Native configured source does not match the staged host engine patch')
    import sys
    sys.argv = [sys.argv[0], '--content-extensions']
    if '#define ENABLE_WK_WEB_EXTENSIONS 1' in (probe.BUILD / 'cmakeconfig.h').read_text():
        sys.argv += ['--configured-extensions']
    for unit in units or UNITS:
        sys.argv += ['--source', unit]
    failure = None
    try:
        probe.main()
    except SystemExit as error:
        failure = error
    result_path = probe.OUTPUT / 'results.json'
    if result_path.exists():
        report = json.loads(result_path.read_text())
        report['scope'] = 'extension lifecycle compile preflight only; no link or runtime assertions'
        report['runtime_assertions_executed'] = False
        report['runtime_link_requirement'] = 'Matching WebKit, WebCore and generated IPC with both extension features enabled'
        result_path.write_text(json.dumps(report, indent=2) + '\n')
    if failure:
        raise failure
    return 0


def host(overlay=None, units=None, engine_root=DEFAULT_ENGINE):
    engine = ROOT / '.cache/WebKit'
    files = {}
    def source(path):
        candidate = pathlib.Path(overlay).resolve() / path.relative_to(engine) if overlay else path
        return candidate if candidate.is_file() else path
    for directory in ('Source/WebKit/UIProcess/Extensions', 'Source/WebKit/Shared/Extensions',
                      'Source/WebKit/WebProcess/Extensions'):
        directory_path = engine / directory
        paths = set(directory_path.glob('*.h')) | set(directory_path.glob('haiku/*.h'))
        if overlay:
            candidate_directory = pathlib.Path(overlay).resolve() / directory
            for candidate in candidate_directory.glob('haiku/*.h'):
                paths.add(directory_path / candidate.relative_to(candidate_directory))
        for path in sorted(paths):
            name = str(path.relative_to(directory_path))
            if name in files:
                raise RuntimeError('Ambiguous staged header: ' + name)
            files[name] = (path, source(path), source(path).read_bytes())
    for name in units or UNITS:
        directory = 'WebProcess' if name.endswith('Proxy.cpp') else 'UIProcess'
        path = engine / 'Source/WebKit' / directory / 'Extensions' / name
        files[name] = (path, source(path), source(path).read_bytes())
    lock = json.loads((ROOT / 'engine/sources.lock.json').read_text())
    manifest = {'upstream_commit': lock['upstream']['commit'], 'host_engine_patch_sha256': lock['patch']['sha256'],
                'candidate_overlay': str(pathlib.Path(overlay).resolve()) if overlay else None,
                'files': {name: {'source': str(path), 'baseline_source': str(base.relative_to(ROOT)),
                                 'baseline_sha256': hashlib.sha256(base.read_bytes()).hexdigest() if base.is_file() else None,
                                 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, (base, path, data) in files.items()}}
    content = {'sources/' + name: data for name, (_, _, data) in files.items()}
    content['sources/source-manifest.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    for name in ('test-engine-extension-manifest-core.py', 'test-engine-extension-lifecycle-compile.py'):
        content['tools/' + name] = (ROOT / 'tools' / name).read_bytes()
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w:gz') as stream:
        for name, data in content.items():
            item = tarfile.TarInfo(name)
            item.mode, item.size = 0o600, len(data)
            stream.addfile(item, io.BytesIO(data))
    def remote(command, **kwargs):
        return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
    stage = remote('mktemp -d /boot/home/summit/extension-lifecycle-inputs.XXXXXXXX',
                   capture_output=True, text=True, check=True).stdout.strip()
    if not stage.startswith('/boot/home/summit/extension-lifecycle-inputs.') or not stage.rsplit('.', 1)[-1].isalnum():
        raise RuntimeError('Unexpected native staging directory')
    output = ROOT / '.vm' / stage.rsplit('/', 1)[-1]
    output.mkdir(exist_ok=False)
    remote('tar -xzf - -C ' + shlex.quote(stage), input=archive.getvalue(), check=True)
    print('Native extension compile stage: ' + stage, flush=True)
    command = ['python3.10', stage + '/tools/test-engine-extension-lifecycle-compile.py', '--native',
               '--engine-root', engine_root]
    for unit in units or ():
        command += ['--unit', unit]
    with (output / 'native.log').open('w') as log:
        result = remote(shlex.join(command), stdout=log, stderr=subprocess.STDOUT)
    print((output / 'native.log').read_text(), end='', flush=True)
    read = remote('cat ' + shlex.quote(stage + '/sources/results.json'), capture_output=True, text=True)
    report = {'stage': stage, 'exit': result.returncode,
              'native': json.loads(read.stdout) if read.returncode == 0 else {'error': 'No native compile report'}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output / 'result.json'), 'all_compiled': report['native'].get('all_compiled', False)}), flush=True)
    return result.returncode


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', action='store_true')
    parser.add_argument('--overlay', help='Candidate files under Source/; no production source changes')
    parser.add_argument('--unit', choices=UNITS + EXTRA_UNITS, action='append', help='Compile only this unit; repeatable')
    parser.add_argument('--engine-root', default=DEFAULT_ENGINE, help='Configured native engine source tree')
    args = parser.parse_args()
    raise SystemExit(native(args.unit, args.engine_root) if args.native else host(args.overlay, args.unit, args.engine_root))
