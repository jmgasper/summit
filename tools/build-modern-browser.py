#!/usr/bin/env python3
"""Build a native preview or Summit browser, then freeze a completed modern WebKit build.

Run through build-modern-browser-in-vm.sh, which locks engine and ICU builds before bundling.
--compile-only uses isolated staged headers and sources, without accessing the native engine build.
"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = pathlib.Path('/boot/home/summit-webkit')
ENGINE = SOURCE / 'WebKitBuild/Modern'
ICU = pathlib.Path('/boot/home/summit-deps/icu78')
LIBZIP = pathlib.Path('/boot/home/summit-deps/libzip-1.11.4')


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            value.update(data)
    return value.hexdigest()


def run(arguments):
    return subprocess.run([str(argument) for argument in arguments], check=True,
                          capture_output=True, text=True).stdout


def dynamic(path, name):
    return re.findall(r'\((?:' + name + r')\).*?\[([^\]]*)\]', run(['readelf', '-d', path]))


def relocate(path, search_path, required=False):
    old = dynamic(path, 'RUNPATH|RPATH')
    if not old:
        if required:
            raise RuntimeError(f'{path.name} has no configured library search path')
        return
    if len(old) != 1:
        raise RuntimeError(f'{path.name} has ambiguous library search paths')
    script = path.parent / '.relocate.cmake'
    script.write_text(f'file(RPATH_CHANGE FILE [==[{path}]==] OLD_RPATH [==[{old[0]}]==] NEW_RPATH [==[{search_path}]==])\n')
    try:
        run(['cmake', '-P', script])
    finally:
        script.unlink(missing_ok=True)
    if dynamic(path, 'RUNPATH|RPATH') != [search_path]:
        raise RuntimeError(f'Failed to set the private library path for {path.name}')


def require_idle():
    # The host wrapper holds our two build locks. Native ps cannot expose a
    # process working directory, so refuse any direct make/ninja/cmake activity.
    active = [line for line in run(['ps']).splitlines()
              if re.search(r'(?:^|\s)(?:\S*/)?(?:ninja|cmake|make)(?:\s|$)', line)]
    if active:
        raise RuntimeError('An engine or ICU build is active in the VM: ' + active[0].strip())


def engine_inputs(inputs):
    require_idle()
    manifest = SOURCE / '.summit-source-manifest.json'
    if json.loads(manifest.read_text()).get('patch_sha256') != inputs['engine']['patch']['sha256']:
        raise RuntimeError('The native engine source patch differs from sources.lock.json; complete the engine build first')
    cache_path = ENGINE / 'CMakeCache.txt'
    cache = dict(re.findall(r'^([A-Za-z_][A-Za-z0-9_-]*):[^=\r\n]*=([^\r\n]*)$', cache_path.read_text(), re.MULTILINE))
    expected = {'PORT': 'Haiku', 'ENABLE_WEBKIT': 'ON', 'ENABLE_WEBKIT_LEGACY': 'OFF',
                'CMAKE_HOME_DIRECTORY': str(SOURCE), 'ICU_ROOT': str(ICU),
                'ENABLE_WK_WEB_EXTENSIONS': 'ON' if 'libzip' in inputs else 'OFF',
                'ENABLE_CONTENT_EXTENSIONS': 'ON' if 'libzip' in inputs else 'OFF'}
    mismatches = [f'{key}: expected {value!r}, found {cache.get(key)!r}'
                  for key, value in expected.items() if cache.get(key) != value]
    if mismatches:
        raise RuntimeError('Modern CMake configuration does not match the native WebKit/ICU build: ' + '; '.join(mismatches))
    for name, relative in inputs['public_headers'].items():
        if digest(SOURCE / 'Source/WebKit' / relative) != inputs['sha256']['include/WebKit/' + name]:
            raise RuntimeError(f'Public header {name} differs from the engine source')
    missing = [name for name in ['bin/WebProcess', 'bin/NetworkProcess', 'lib/libWebKit.so']
               if not (ENGINE / name).is_file()]
    if missing:
        raise RuntimeError('Modern engine is not fully linked; missing: ' + ', '.join(missing))
    # A matching source manifest alone does not prove these executables were rebuilt.
    dry_run = run(['ninja', '-C', ENGINE, '-n', 'WebProcess', 'NetworkProcess'])
    work = [line for line in dry_run.splitlines()
            if line and not line.startswith('ninja: Entering directory') and line != 'ninja: no work to do.']
    if work or 'ninja: no work to do.' not in dry_run:
        raise RuntimeError('Complete both modern process targets before freezing: ' + '\n'.join(work[:10]))
    paths = [manifest, cache_path, ENGINE / 'build.ninja', ENGINE / '.ninja_log']
    paths += [ENGINE / 'bin/WebProcess', ENGINE / 'bin/NetworkProcess']
    paths += [(ENGINE / 'lib' / name).resolve(strict=True)
              for name in ['libWebKit.so', 'libJavaScriptCore.so']]
    paths += [(ICU / 'lib' / name).resolve(strict=True)
              for name in ['libicudata.so', 'libicui18n.so', 'libicuuc.so']]
    if 'libzip' in inputs:
        lock = inputs['libzip']
        if lock['version'] != '1.11.4':
            raise RuntimeError('Update the private libzip prefix for the new locked version')
        for source, relative in [('lib/libzip.so.5.5', 'lib/libzip.so.5.5'),
                                 ('develop/headers/zip.h', 'include/zip.h')]:
            path = LIBZIP / relative
            if digest(path) != lock['files'][source]:
                raise RuntimeError('Private libzip differs from its locked input: ' + relative)
            paths.append(path)
    return expected, paths


def library(name, directory):
    source = (directory / name).resolve(strict=True)
    if source.parent != directory.resolve():
        raise RuntimeError(f'{name} resolves outside the required private library directory')
    sonames = dynamic(source, 'SONAME')
    if len(sonames) != 1 or pathlib.Path(sonames[0]).name != sonames[0]:
        raise RuntimeError(f'Invalid library identity in {source}')
    return name, sonames[0], source


def verify_hashes(hashes):
    for path, before in hashes.items():
        if digest(pathlib.Path(path)) != before:
            raise RuntimeError(f'Build input changed while creating the app: {path}')


def freeze(work, inputs, commands, before, configuration, build, executable_name, browser):
    require_idle()
    verify_hashes(before)
    bundle = pathlib.Path(tempfile.mkdtemp(prefix='bundle-', dir=build))
    try:
        (bundle / 'lib').mkdir()
        libraries = [library(name, ENGINE / 'lib') for name in ['libWebKit.so', 'libJavaScriptCore.so']]
        libraries += [library(name, ICU / 'lib') for name in ['libicudata.so', 'libicui18n.so', 'libicuuc.so']]
        if inputs['icu']['version'] != '78.3' or any('.so.78' not in soname for _, soname, _ in libraries[2:]):
            raise RuntimeError('The app requires the pinned private ICU 78.3 build')
        if 'libzip' in inputs:
            libraries.append(library('libzip.so', LIBZIP / 'lib'))
        files = {executable_name: work / executable_name, 'WebProcess': ENGINE / 'bin/WebProcess',
                 'NetworkProcess': ENGINE / 'bin/NetworkProcess'}
        files.update({'lib/' + source.name: source for _, _, source in libraries})
        before = {**before, str(work / executable_name): digest(work / executable_name)}
        for relative, source in files.items():
            destination = bundle / relative
            try:
                shutil.copy2(source, destination)
            except OSError as error:
                copied = destination.stat().st_size if destination.exists() else 0
                raise RuntimeError(f'Cannot copy {source} to {destination}: {error}; '
                                   f'{copied} of {source.stat().st_size} bytes copied, '
                                   f'{shutil.disk_usage(bundle).free} bytes free') from error
            if digest(destination) != before[str(source)]:
                raise RuntimeError(f'{source.name} changed while being copied')
            relocate(destination, '$ORIGIN' if relative.startswith('lib/') else '$ORIGIN/lib',
                     required=not relative.startswith('lib/') or source.name.startswith(('libWebKit', 'libJavaScriptCore')))
        for name, soname, source in libraries:
            for alias in {name, soname} - {source.name}:
                (bundle / 'lib' / alias).symlink_to(source.name)
        dependencies = {name: dynamic(bundle / name, 'NEEDED') for name in files}
        for name, needed in dependencies.items():
            for dependency in needed:
                if dependency.startswith(('libWebKit', 'libJavaScriptCore', 'libicu', 'libzip')) and not (bundle / 'lib' / dependency).is_file():
                    raise RuntimeError(f'{name} has an unbundled engine dependency: {dependency}')
        shutil.copy2(ROOT / 'LICENSE-Summit', bundle / 'LICENSE-Summit')
        assets = []
        if browser:
            (bundle / 'resources').mkdir()
            shutil.copy2(ROOT / 'resources/start.html', bundle / 'resources/start.html')
            assets.append('resources/start.html')
        for license_file in (SOURCE / 'Source').rglob('*'):
            if license_file.is_file() and license_file.name.upper().startswith(('LICENSE', 'COPYING')):
                destination = bundle / 'licenses/WebKit' / license_file.relative_to(SOURCE / 'Source')
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(license_file, destination)
        icu_license = pathlib.Path('/boot/home/summit-deps/icu-78.3/LICENSE')
        (bundle / 'licenses/ICU').mkdir(parents=True)
        shutil.copy2(icu_license, bundle / 'licenses/ICU/LICENSE')
        if 'libzip' in inputs:
            # The locked development header contains libzip's complete notice.
            # Haiku's binary package does not include a separate license file.
            (bundle / 'licenses/libzip').mkdir(parents=True)
            shutil.copy2(LIBZIP / 'include/zip.h', bundle / 'licenses/libzip/zip.h')
            if digest(bundle / 'licenses/libzip/zip.h') != before[str(LIBZIP / 'include/zip.h')]:
                raise RuntimeError('The libzip license header changed during copying')
            assets.append('licenses/libzip/zip.h')
        launcher = bundle / ('run-browser.sh' if browser else 'run-preview.sh')
        launcher.write_text(
            '#!/bin/sh\nset -eu\n'
            'SUMMIT_BUNDLE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)\n'
            'export WEBKIT_EXEC_PATH="$SUMMIT_BUNDLE"\n'
            'export LIBRARY_PATH="$SUMMIT_BUNDLE/lib:/boot/system/lib"\n'
            f'exec "$SUMMIT_BUNDLE/{executable_name}" "$@"\n')
        launcher.chmod(0o755)
        (bundle / 'README.txt').write_text(
            ('Summit browser with modern WebKit for Haiku/KunanyiOS.\n'
             'Run ./run-browser.sh [--profile PATH] [URL ...].\n'
             'Default settings are stored separately in the SummitModern profile.\n'
             if browser else
             'Summit modern WebKit native preview for Haiku/KunanyiOS.\n'
             'Run ./run-preview.sh [URL], or --smoke with tools/serve-fixtures.py on the host.\n') +
            'WebProcess and NetworkProcess are resolved beside the app executable.\n'
            'Private WebKit, JavaScriptCore and ICU libraries are in lib with relative runtime paths.\n'
            + ('The extension-enabled engine includes pinned libzip; its license notice is in licenses/libzip/zip.h.\n'
               if 'libzip' in inputs else '') +
            'The app source and build inputs are preserved in source.\n')
        shutil.copytree(ROOT, bundle / 'source')
        for name, expected in inputs['sha256'].items():
            if digest(bundle / 'source' / name) != expected:
                raise RuntimeError(f'Staged input changed while creating the bundle: {name}')
        require_idle()
        verify_hashes(before)
        manifest = {
            'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'kind': 'modern-native-browser' if browser else 'modern-native-preview',
            'inputs': inputs, 'configuration': configuration,
            'compiler': run(['c++', '--version']).splitlines()[0],
            'compile_commands': commands, 'original_sha256': before,
            'bundled_sha256': {name: digest(bundle / name) for name in [*files, launcher.name, *assets]},
            'needed': dependencies,
            'runtime_search_paths': {name: dynamic(bundle / name, 'RPATH|RUNPATH') for name in files},
            'symlinks': {str(path.relative_to(bundle)): os.readlink(path)
                         for path in (bundle / 'lib').iterdir() if path.is_symlink()},
        }
        (bundle / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        latest = build / 'latest-bundle.json'
        temporary = latest.with_suffix('.tmp')
        temporary.write_text(json.dumps({'bundle': str(bundle), **manifest}, indent=2) + '\n')
        temporary.replace(latest)
        return {'bundle': str(bundle), **manifest}
    except BaseException:
        shutil.rmtree(bundle)
        raise


def main():
    global SOURCE, ENGINE
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--compile-only', action='store_true')
    mode.add_argument('--bundle', action='store_true')
    parser.add_argument('--build-root', default='/boot/home/summit',
                        choices=('/boot/home/summit', '/SummitExtensions/summit'))
    arguments = parser.parse_args()
    inputs = json.loads((ROOT / 'inputs.json').read_text())
    variant = inputs.get('engine_variant', 'modern')
    if variant not in ('modern', 'modern-extensions') or ('libzip' in inputs) != (variant == 'modern-extensions'):
        raise RuntimeError('Unknown engine variant or inconsistent libzip input')
    SOURCE = pathlib.Path('/boot/home/summit-webkit' + ('-extensions' if variant == 'modern-extensions' else ''))
    engine_build_name = inputs.get('engine_build_name', 'Modern')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_-]{0,31}', engine_build_name):
        raise RuntimeError('Invalid engine build name')
    ENGINE = SOURCE / 'WebKitBuild' / engine_build_name
    target = inputs.get('target', 'preview')
    if target not in ('preview', 'browser'):
        raise RuntimeError('Unknown native app target: ' + str(target))
    browser = target == 'browser'
    executable_name = 'Summit' if browser else 'SummitModernPreview'
    build_root = pathlib.Path(arguments.build_root)
    if build_root.parts[1] == 'SummitExtensions' and not os.path.ismount('/SummitExtensions'):
        raise RuntimeError('The SummitExtensions volume must be mounted before building there')
    build = build_root / ('build-modern-' + target)
    if build.resolve() != build:
        raise RuntimeError('The native build directory must not use an indirect path')
    for name, expected in inputs['sha256'].items():
        if digest(ROOT / name) != expected:
            raise RuntimeError('Staged input hash mismatch: ' + name)
    if not arguments.compile_only:
        require_idle()
    configuration, original_paths = ({}, []) if arguments.compile_only else engine_inputs(inputs)
    build.mkdir(parents=True, exist_ok=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix='compile-', dir=build))
    flags = ['c++', '-std=c++23', '-O2', '-Wall', '-Wextra', '-Wno-multichar',
             '-DBUILDING_HAIKU__=1', '-I' + str(ROOT / 'include')]
    sources = ['tests/ModernBrowser.cpp']
    if browser:
        flags += ['-DSUMMIT_MODERN_WEBKIT=1', '-I' + str(ROOT / 'src'),
                  '-I' + str(ROOT / 'vendor'), '-I/boot/system/develop/headers/private/netservices',
                  # app/AppMisc.h, for the view token a synthesized mouse wheel
                  # message needs to reach the page (see BrowserWindow::SimulateScroll).
                  '-I/boot/system/develop/headers/private']
        sources = ['src/main.cpp', 'src/core/Address.cpp', 'src/core/Profile.cpp', 'src/core/ExtensionCatalog.cpp', 'src/core/ExtensionIdentity.cpp',
                   'src/ui/BrowserWindow.cpp', 'src/ui/Chrome.cpp', 'src/ui/ExtensionPermissionPrompt.cpp',
                   'src/ui/ExtensionController.cpp', 'src/ui/ExtensionInstaller.cpp', 'src/ui/ExtensionManager.cpp']
    objects = [work / pathlib.Path(source).with_suffix('.o') for source in sources]
    commands = [flags + ['-c', str(ROOT / source), '-o', str(output)]
                for source, output in zip(sources, objects)]
    if not arguments.compile_only:
        commands.append(['c++', *map(str, objects), '-L' + str(ENGINE / 'lib'),
                         '-lWebKit', '-lbe', '-lnetwork', '-lcrypto',
                         *(['-lbnetapi', '-ltranslation', '-ltracker'] if browser else []),
                         '-Wl,-rpath,' + ':'.join(map(str, [ENGINE / 'lib', ICU / 'lib']
                             + ([LIBZIP / 'lib'] if 'libzip' in inputs else []))),
                         '-o', str(work / executable_name)])
        if browser:
            commands += [['rc', '-I', str(ROOT / 'resources'),
                          '-o', str(work / 'Summit.rsrc'), str(ROOT / 'resources/Summit.rdef')],
                         ['xres', '-o', str(work / executable_name), str(work / 'Summit.rsrc')]]
    before = {str(path): digest(path) for path in original_paths}
    try:
        for output in objects:
            output.parent.mkdir(parents=True, exist_ok=True)
        for command in commands:
            run(command)
        verify_hashes(before)
        if arguments.compile_only:
            shutil.copytree(ROOT, work / 'source')
            report = {'kind': 'compile-only', 'target': target,
                      'objects': {str(output): digest(output) for output in objects}, 'inputs': inputs,
                      'compiler': run(['c++', '--version']).splitlines()[0], 'compile_commands': commands}
            (work / 'build-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
        else:
            report = freeze(work, inputs, commands, before, configuration, build, executable_name, browser)
        print(json.dumps(report, indent=2))
    except BaseException:
        shutil.rmtree(work)
        raise


if __name__ == '__main__':
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(f'Command failed ({error.returncode}): {error.cmd}\n{error.stdout or ""}{error.stderr or ""}')
    except (OSError, RuntimeError) as error:
        raise SystemExit(str(error))
