#!/usr/bin/env python3
"""Compile native extension lifecycle sources in isolation; never link feature-mismatched objects."""
import argparse
import hashlib
import importlib.util
import io
import json
import pathlib
import re
import shlex
import subprocess
import tarfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
UNITS = ('WebExtensionController.cpp', 'WebExtensionContext.cpp',
         'WebExtensionDeclarativeNetRequestSQLiteStore.cpp',
         'WebExtensionMatchPatternProcessPool.cpp', 'WebExtensionContextProxy.cpp',
         'WebExtensionControllerProxy.cpp')
EXTRA_UNITS = ('haiku/WebExtensionContextBackgroundHaiku.cpp', 'haiku/WebExtensionURLSchemeHandlerHaiku.cpp', 'haiku/WebExtensionContextHaiku.cpp',
               'haiku/WebExtensionStateHaiku.cpp', 'Bindings/JSWebExtensionWrapper.cpp',
               'Bindings/JSWebExtensionMessageReply.cpp', 'Bindings/JSWebExtensionString.cpp',
               'Bindings/JSWebExtensionTabParameters.cpp',
               'API/WebExtensionAPINamespace.cpp', 'API/WebExtensionAPIRuntime.cpp',
               'API/WebExtensionAPIEvent.cpp', 'API/WebExtensionAPIPort.cpp')
GENERATED_UNITS = ('WebExtensionContextMessageReceiver.cpp', 'WebExtensionContextProxyMessageReceiver.cpp')
BINDING_UNITS = ('JSWebExtensionAPIEvent.cpp', 'JSWebExtensionAPIPort.cpp')
PAGE_UNITS = {'WebPage.cpp': 'WebProcess/WebPage/WebPage.cpp',
              'WebLocalFrameLoaderClient.cpp': 'WebProcess/WebCoreSupport/WebLocalFrameLoaderClient.cpp'}
NATIVE_PAGE_UNITS = {'WebPageProxy.cpp': 'UIProcess/WebPageProxy.cpp',
                     'WebView.cpp': 'UIProcess/haiku/WebView.cpp'}
UI_API_UNITS = {'WebExtensionDynamicScripts.cpp': 'UIProcess/Extensions/WebExtensionDynamicScripts.cpp',
                'WebExtensionUtilities.cpp': 'Shared/Extensions/WebExtensionUtilities.cpp',
                'WebExtensionContextAPIDeclarativeNetRequest.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIDeclarativeNetRequest.cpp',
                'WebExtensionRegisteredScriptParser.cpp': 'Shared/Extensions/WebExtensionRegisteredScriptParser.cpp',
                'WebExtensionContextAPIScripting.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIScripting.cpp',
                'WebExtensionContextAPIEvent.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIEvent.cpp',
                'WebExtensionContextAPIPort.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIPort.cpp',
                'WebExtensionContextAPIRuntime.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIRuntime.cpp'}
DEFAULT_ENGINE = '/boot/home/summit-webkit'


def regenerate_ipc(probe, manifest, units):
    """Snapshot the configured receiver list and invoke the real generator in isolation."""
    source_root = probe.ENGINE / 'Source/WebKit'
    build = probe.BUILD
    generator = source_root / 'Scripts/generate-message-receiver.py'
    lines = (build / 'build.ninja').read_text().splitlines()
    command = None
    for index, line in enumerate(lines):
        if line.startswith('build ') and str(generator) in line:
            command = next(line.split(' = ', 1)[1] for line in lines[index + 1:index + 8]
                           if line.startswith('  COMMAND = '))
            break
    if not command:
        raise RuntimeError('Configured IPC generation rule was not found')
    words = shlex.split(command)
    watched = [generator]
    if 'generate-message-receiver.py' not in command:
        if words[0] != '/bin/sh' or not words[1].endswith('.sh'):
            raise RuntimeError('Unrecognized configured IPC command wrapper')
        wrapper = build / words[1]
        watched.append(wrapper)
        words = shlex.split(wrapper.read_text(), comments=True)
    index = next(index for index, word in enumerate(words) if word == str(generator))
    interpreter = pathlib.Path(words[index - 1])
    if not interpreter.is_file() or not interpreter.name.startswith('python'):
        raise RuntimeError('Configured IPC Python interpreter was not found')
    arguments = words[index + 1:words.index('--output-dir', index)]
    arguments.remove('--preserve-subdirs')
    if arguments.pop(0) != str(source_root):
        raise RuntimeError('Unexpected configured IPC source directory')
    if not arguments or any(not re.fullmatch(r'[A-Za-z0-9_/-]+', name) for name in arguments):
        raise RuntimeError('Invalid configured receiver list')

    inputs = probe.OUTPUT / 'ipc-inputs'
    inputs.mkdir(exist_ok=True)
    original_hashes = {}
    for receiver in arguments:
        relative = pathlib.Path(receiver + '.messages.in')
        target = inputs / relative
        if not target.exists():
            original = source_root / relative
            if not original.is_file():
                original = build / 'WebKit/DerivedSources' / relative.name
            data = original.read_bytes()
            original_hashes[str(original)] = hashlib.sha256(data).hexdigest()
            watched.append(original)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        name = str(target.relative_to(probe.OUTPUT))
        if name not in manifest['files']:
            manifest['files'][name] = {'source': str(target), 'sha256': probe.digest(target)}
    watched.extend(sorted((source_root / 'Scripts/webkit').glob('*.py')))
    watched.append(source_root / 'Scripts/webkit/opaque_ipc_types.tracking.in')
    snapshot = {str(path): probe.digest(path) for path in watched}
    command = [str(interpreter), str(generator), str(inputs), *arguments, '--output-dir', str(probe.OUTPUT)]
    subprocess.run(command, cwd=inputs, check=True)
    if any(probe.digest(pathlib.Path(path)) != expected for path, expected in snapshot.items()):
        raise RuntimeError('Native IPC inputs changed during generation')
    generated = {path.name: probe.digest(path) for path in probe.OUTPUT.glob('*Messages.h')}
    generated['MessageNames.h'] = probe.digest(probe.OUTPUT / 'MessageNames.h')
    for name in units:
        if name in GENERATED_UNITS:
            manifest['files'][name] = {'source': str(probe.OUTPUT / name),
                                       'sha256': probe.digest(probe.OUTPUT / name), 'generated_from_ipc': True}
    manifest['ipc_generation'] = {'receiver_count': len(arguments), 'command': command,
                                  'native_inputs': snapshot, 'copied_receivers': original_hashes,
                                  'generated_headers': generated}
    probe.EXTRA_WATCHED_INPUTS = tuple(watched)
    (probe.OUTPUT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def native(units=None, engine_root=DEFAULT_ENGINE, regenerate=False):
    spec = importlib.util.spec_from_file_location('extension_compile_probe',
        ROOT / 'tools/test-engine-extension-manifest-core.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    probe.OUTPUT = ROOT / 'sources'
    probe.SOURCES = UNITS + EXTRA_UNITS + GENERATED_UNITS + BINDING_UNITS + tuple(PAGE_UNITS) + tuple(NATIVE_PAGE_UNITS) + tuple(UI_API_UNITS)
    probe.ENGINE = pathlib.Path(engine_root)
    probe.BUILD = probe.ENGINE / 'WebKitBuild/Modern'
    manifest = json.loads((probe.OUTPUT / 'source-manifest.json').read_text())
    engine = json.loads((probe.ENGINE / '.summit-source-manifest.json').read_text())
    if engine['patch_sha256'] != manifest['host_engine_patch_sha256']:
        raise RuntimeError('Native configured source does not match the staged host engine patch')
    if regenerate:
        regenerate_ipc(probe, manifest, units or UNITS)
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


def host(overlay=None, units=None, engine_root=DEFAULT_ENGINE, regenerate=False, generated_bindings=None):
    engine = ROOT / '.cache/WebKit'
    files = {}
    def source(path):
        candidate = pathlib.Path(overlay).resolve() / path.relative_to(engine) if overlay else path
        return candidate if candidate.is_file() else path
    for directory in ('Source/WebKit/UIProcess/Extensions', 'Source/WebKit/Shared/Extensions',
                      'Source/WebKit/WebProcess/Extensions'):
        directory_path = engine / directory
        patterns = ('*.h', 'haiku/*.h', 'API/*.h', 'Bindings/*.h')
        paths = {path for pattern in patterns for path in directory_path.glob(pattern)}
        if overlay:
            candidate_directory = pathlib.Path(overlay).resolve() / directory
            for pattern in patterns:
                for candidate in candidate_directory.glob(pattern):
                    paths.add(directory_path / candidate.relative_to(candidate_directory))
        for path in sorted(paths):
            name = str(path.relative_to(directory_path))
            if name in files:
                raise RuntimeError('Ambiguous staged header: ' + name)
            files[name] = (path, source(path), source(path).read_bytes())
    for name in units or UNITS:
        if name in BINDING_UNITS:
            if not generated_bindings:
                raise RuntimeError('Generated binding units require --generated-bindings')
            continue
        if name in GENERATED_UNITS:
            if not regenerate:
                raise RuntimeError('Generated receiver units require --regenerate-ipc')
            continue
        directory = 'WebProcess' if name.endswith('Proxy.cpp') or name.startswith(('API/', 'Bindings/')) else 'UIProcess'
        relative = PAGE_UNITS.get(name, NATIVE_PAGE_UNITS.get(name, UI_API_UNITS.get(name, directory + '/Extensions/' + name)))
        path = engine / 'Source/WebKit' / relative
        files[name] = (path, source(path), source(path).read_bytes())
    if any(name in PAGE_UNITS for name in units or ()) or (overlay and
            (pathlib.Path(overlay).resolve() / 'Source/WebKit/WebProcess/WebPage/WebPage.h').is_file()):
        # Keep quoted sibling includes on the same copied WebPage header.
        for path in sorted((engine / 'Source/WebKit/WebProcess/WebPage').glob('*.h')):
            if path.name in files:
                raise RuntimeError('Ambiguous staged page header: ' + path.name)
            files[path.name] = (path, source(path), source(path).read_bytes())
    if any(name in NATIVE_PAGE_UNITS for name in units or ()) or (overlay and
            (pathlib.Path(overlay).resolve() / 'Source/WebKit/UIProcess/haiku/WebViewPrivate.h').is_file()):
        for relative in ('UIProcess/haiku/WebViewPrivate.h',):
            path = engine / 'Source/WebKit' / relative
            if path.name in files:
                raise RuntimeError('Ambiguous native page header: ' + path.name)
            files[path.name] = (path, source(path), source(path).read_bytes())
    if regenerate and overlay:
        for candidate in sorted((pathlib.Path(overlay).resolve() / 'Source/WebKit').rglob('*.messages.in')):
            relative = candidate.relative_to(pathlib.Path(overlay).resolve() / 'Source/WebKit')
            base = engine / 'Source/WebKit' / relative
            files['ipc-inputs/' + str(relative)] = (base, candidate, candidate.read_bytes())
    lock = json.loads((ROOT / 'engine/sources.lock.json').read_text())
    manifest = {'upstream_commit': lock['upstream']['commit'], 'host_engine_patch_sha256': lock['patch']['sha256'],
                'candidate_overlay': str(pathlib.Path(overlay).resolve()) if overlay else None,
                'files': {name: {'source': str(path), 'baseline_source': str(base.relative_to(ROOT)),
                                 'baseline_sha256': hashlib.sha256(base.read_bytes()).hexdigest() if base.is_file() else None,
                                 'sha256': hashlib.sha256(data).hexdigest()}
                          for name, (base, path, data) in files.items()}}
    content = {'sources/' + name: data for name, (_, _, data) in files.items()}
    if generated_bindings:
        generated_bindings = pathlib.Path(generated_bindings).resolve()
        generation = json.loads((generated_bindings / 'binding-generation.json').read_text())
        if not generation['passed'] or generation['engine_patch_sha256'] != lock['patch']['sha256']:
            raise RuntimeError('Binding generation did not pass against this engine patch')
        for path, expected in generation['inputs'].items():
            if hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() != expected:
                raise RuntimeError('Binding generation input changed: ' + path)
        for name, expected in generation['files'].items():
            if pathlib.Path(name).name != name:
                raise RuntimeError('Invalid generated binding filename')
            path = generated_bindings / name
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise RuntimeError('Generated binding changed: ' + name)
            if not name.endswith('.h') and name not in (units or ()):
                continue
            if 'sources/' + name in content:
                raise RuntimeError('Ambiguous generated binding header: ' + name)
            content['sources/' + name] = data
            manifest['files'][name] = {'source': str(path), 'sha256': expected, 'generated_binding': True}
        manifest['binding_generation'] = generation
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
        try:
            return subprocess.run(['bash', str(ROOT / 'tools/haiku.sh'), command], **kwargs)
        except subprocess.CalledProcessError as error:
            if error.stderr:
                print(error.stderr, flush=True)
            raise
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
    if regenerate:
        command.append('--regenerate-ipc')
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
    parser.add_argument('--unit', choices=UNITS + EXTRA_UNITS + GENERATED_UNITS + BINDING_UNITS + tuple(PAGE_UNITS) + tuple(NATIVE_PAGE_UNITS) + tuple(UI_API_UNITS), action='append', help='Compile only this unit; repeatable')
    parser.add_argument('--engine-root', default=DEFAULT_ENGINE, help='Configured native engine source tree')
    parser.add_argument('--regenerate-ipc', action='store_true', help='Generate matching IPC headers in the isolated stage')
    parser.add_argument('--generated-bindings', help='Verified output from generate-extension-bindings-candidate.py')
    args = parser.parse_args()
    raise SystemExit(native(args.unit, args.engine_root, args.regenerate_ipc) if args.native else host(args.overlay, args.unit, args.engine_root, args.regenerate_ipc, args.generated_bindings))
