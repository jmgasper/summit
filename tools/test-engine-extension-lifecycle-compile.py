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
EXTRA_UNITS = ('API/WebExtensionAPICookies.cpp', 'Bindings/JSWebExtensionCookieParameters.cpp', 'API/WebExtensionAPIAction.cpp', 'API/WebExtensionAPIWindows.cpp', 'API/WebExtensionAPIWindowsEvent.cpp',
               'Bindings/JSWebExtensionWindowParameters.cpp', 'API/WebExtensionAPITabs.cpp', 'API/WebExtensionAPIPermissions.cpp', 'API/WebExtensionAPICommands.cpp',
               'API/WebExtensionAPIWebRequest.cpp', 'API/WebExtensionAPIWebRequestEvent.cpp', 'haiku/WebExtensionContextBackgroundHaiku.cpp', 'haiku/WebExtensionURLSchemeHandlerHaiku.cpp', 'haiku/WebExtensionContextHaiku.cpp',
               'haiku/WebExtensionContextTestHaiku.cpp',
               'haiku/WebExtensionStateHaiku.cpp', 'Bindings/JSWebExtensionWrapper.cpp',
               'Bindings/JSWebExtensionMessageReply.cpp', 'Bindings/JSWebExtensionString.cpp',
               'Bindings/JSWebExtensionTabParameters.cpp',
               'Bindings/JSWebExtensionStorageValues.cpp',
               'Bindings/JSWebExtensionPromise.cpp',
               'API/WebExtensionAPINamespace.cpp', 'API/WebExtensionAPIWebPageNamespace.cpp',
               'API/WebExtensionAPIStorage.cpp', 'API/WebExtensionAPIStorageArea.cpp',
               'API/WebExtensionAPIWebNavigation.cpp', 'API/WebExtensionAPIWebNavigationEvent.cpp',
               'API/WebExtensionAPITest.cpp',
               'API/WebExtensionAPIRuntime.cpp',
               'API/WebExtensionAPIEvent.cpp', 'API/WebExtensionAPIPort.cpp')
SERIALIZER_UNITS = ('GeneratedSerializersSharedWebCoreArgumentCodersNetwork.cpp',)
GENERATED_UNITS = ('NetworkProcessProxyMessageReceiver.cpp', 'WebExtensionContextMessageReceiver.cpp', 'WebExtensionContextProxyMessageReceiver.cpp', 'WebCookieManagerMessageReceiver.cpp')
BINDING_UNITS = ('JSWebExtensionAPICookies.cpp', 'JSWebExtensionAPIAction.cpp', 'JSWebExtensionAPIWindows.cpp', 'JSWebExtensionAPIWindowsEvent.cpp', 'JSWebExtensionAPITabs.cpp', 'JSWebExtensionAPIPermissions.cpp', 'JSWebExtensionAPICommands.cpp',
                 'JSWebExtensionAPIWebRequest.cpp', 'JSWebExtensionAPIWebRequestEvent.cpp', 'JSWebExtensionAPIWebNavigation.cpp', 'JSWebExtensionAPIWebNavigationEvent.cpp',
                 'JSWebExtensionAPIEvent.cpp', 'JSWebExtensionAPIPort.cpp',
                 'JSWebExtensionAPIStorage.cpp', 'JSWebExtensionAPIStorageArea.cpp',
                 'JSWebExtensionAPINamespace.cpp', 'JSWebExtensionAPIWebPageNamespace.cpp')
PAGE_UNITS = {'WebLoaderStrategy.cpp': 'WebProcess/Network/WebLoaderStrategy.cpp',
              'WebPage.cpp': 'WebProcess/WebPage/WebPage.cpp',
              'WebLocalFrameLoaderClient.cpp': 'WebProcess/WebCoreSupport/WebLocalFrameLoaderClient.cpp'}
NATIVE_PAGE_UNITS = {'ExtensionPermissionPromptHaiku.cpp': 'UIProcess/haiku/ExtensionPermissionPromptHaiku.cpp',
                     'BrowserTabRegistryHaiku.cpp': 'UIProcess/haiku/BrowserTabRegistryHaiku.cpp',
                     'WebViewContextHaiku.cpp': 'UIProcess/haiku/WebViewContextHaiku.cpp',
                     'WebKitContext.cpp': 'UIProcess/API/haiku/WebKitContext.cpp',
                     'WebPageProxy.cpp': 'UIProcess/WebPageProxy.cpp',
                     'WebView.cpp': 'UIProcess/haiku/WebView.cpp'}
WEB_CORE_UNITS = {name: 'contentextensions/' + name for name in ('URLFilterParser.cpp', 'DFABytecodeInterpreter.cpp', 'ContentExtension.cpp',
                  'ContentExtensionURLConditions.cpp', 'ContentExtensionRule.cpp', 'ContentExtensionParser.cpp',
                  'ContentExtensionCompiler.cpp', 'ContentExtensionsBackend.cpp')}
UI_API_UNITS = {'CookieStorageCurl.cpp': 'NetworkProcess/Cookies/curl/CookieStorageCurl.cpp',
                'WebCookieManager.cpp': 'NetworkProcess/Cookies/WebCookieManager.cpp',
                'NetworkProcessProxy.cpp': 'UIProcess/Network/NetworkProcessProxy.cpp',
                'NetworkStorageSessionCurl.cpp': 'NetworkProcess/curl/NetworkStorageSessionCurl.cpp',
                'WebExtensionContextAPICookiesHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextAPICookiesHaiku.cpp',
                'WebExtensionCookieHostAccess.cpp': 'UIProcess/Extensions/haiku/WebExtensionCookieHostAccess.cpp',
                'WebExtensionCookieWriteParser.cpp': 'Shared/Extensions/WebExtensionCookieWriteParser.cpp',
                'WebExtensionCookieQueryParser.cpp': 'Shared/Extensions/WebExtensionCookieQueryParser.cpp',
                'WebExtensionCookieStoreIdentifier.cpp': 'Shared/Extensions/WebExtensionCookieStoreIdentifier.cpp',
                'WebCookieManagerCurl.cpp': 'NetworkProcess/Cookies/curl/WebCookieManagerCurl.cpp',
                'APIHTTPCookieStore.cpp': 'UIProcess/API/APIHTTPCookieStore.cpp',
                'WebsiteDataStore.cpp': 'UIProcess/WebsiteData/WebsiteDataStore.cpp',
                'WebExtensionActionDetails.cpp': 'Shared/Extensions/WebExtensionActionDetails.cpp',
                'WebExtensionContextAPIActionHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextAPIActionHaiku.cpp', 'WebExtensionContextWindowsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextWindowsHaiku.cpp', 'WebExtensionWindowQueryParser.cpp': 'Shared/Extensions/WebExtensionWindowQueryParser.cpp',
                'WebExtensionContextAPIWindowsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextAPIWindowsHaiku.cpp', 'WebExtensionTabQueryParser.cpp': 'Shared/Extensions/WebExtensionTabQueryParser.cpp',
                'WebExtensionContextAPITabsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextAPITabsHaiku.cpp',
                'WebExtensionPermissionState.cpp': 'Shared/Extensions/WebExtensionPermissionState.cpp',
                'WebExtensionContextAPIPermissionsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextAPIPermissionsHaiku.cpp',
                'WebExtensionCommandSettings.cpp': 'Shared/Extensions/WebExtensionCommandSettings.cpp',
                'WebExtensionCommand.cpp': 'UIProcess/Extensions/WebExtensionCommand.cpp',
                'WebExtensionCommandHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionCommandHaiku.cpp',
                'WebExtensionContextCommandsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextCommandsHaiku.cpp',
                'WebExtensionCommandShortcut.cpp': 'Shared/Extensions/WebExtensionCommandShortcut.cpp',
                'APIContentRuleListStore.cpp': 'UIProcess/API/APIContentRuleListStore.cpp',
                'WebExtensionDeclarativeNetRequestURLFilter.cpp': 'UIProcess/Extensions/haiku/WebExtensionDeclarativeNetRequestURLFilter.cpp',
                'WebExtensionPackageSnapshotHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionPackageSnapshotHaiku.cpp',
                'WebExtensionHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionHaiku.cpp',
                'WebExtension.cpp': 'UIProcess/Extensions/WebExtension.cpp',
                'WebExtensionMessagePortHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionMessagePortHaiku.cpp',
                'WebExtensionActionHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionActionHaiku.cpp',
                'WebExtensionContextActionsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextActionsHaiku.cpp',
                'WebExtensionContextPermissionsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextPermissionsHaiku.cpp',
                'WebExtensionControllerWebRequest.cpp': 'UIProcess/Extensions/WebExtensionControllerWebRequest.cpp',
                'WebExtensionContextWebRequest.cpp': 'UIProcess/Extensions/WebExtensionContextWebRequest.cpp',
                'WebExtensionWebRequestFilter.cpp': 'WebProcess/Extensions/WebExtensionWebRequestFilter.cpp',
                'WebExtensionWebRequestBody.cpp': 'WebProcess/Extensions/WebExtensionWebRequestBody.cpp',
                'WebExtensionTabHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionTabHaiku.cpp',
                'WebExtensionWindowHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionWindowHaiku.cpp',
                'WebExtensionContextTabsHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextTabsHaiku.cpp',
                'WebExtensionContextWebNavigationHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionContextWebNavigationHaiku.cpp',
                'WebExtensionNavigationURLFilter.cpp': 'WebProcess/Extensions/WebExtensionNavigationURLFilter.cpp',
                'WebExtensionNavigationParameters.cpp': 'WebProcess/Extensions/WebExtensionNavigationParameters.cpp',
                'WebExtensionContextAPIWebNavigation.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIWebNavigation.cpp',
                'WebExtensionDynamicScripts.cpp': 'UIProcess/Extensions/WebExtensionDynamicScripts.cpp',
                'WebExtensionControllerAPITestHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionControllerAPITestHaiku.cpp',
                'WebExtensionControllerHaiku.cpp': 'UIProcess/Extensions/haiku/WebExtensionControllerHaiku.cpp',
                'WebExtensionWebsiteData.cpp': 'UIProcess/Extensions/WebExtensionWebsiteData.cpp',
                'WebExtensionUtilities.cpp': 'Shared/Extensions/WebExtensionUtilities.cpp',
                'WebExtensionContextAPIDeclarativeNetRequest.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIDeclarativeNetRequest.cpp',
                'WebExtensionRegisteredScriptParser.cpp': 'Shared/Extensions/WebExtensionRegisteredScriptParser.cpp',
                'WebExtensionContextAPIScripting.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIScripting.cpp',
                'WebExtensionContextAPIStorage.cpp': 'UIProcess/Extensions/API/WebExtensionContextAPIStorage.cpp',
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


def regenerate_serializers(probe, manifest, units):
    """Regenerate the complete configured serializer set in an isolated tree."""
    source_root = probe.ENGINE / 'Source/WebKit'
    generator = source_root / 'Scripts/generate-serializers.py'
    commands = [line.split(' = ', 1)[1] for line in (probe.BUILD / 'build.ninja').read_text().splitlines()
                if line.startswith('  COMMAND = ') and str(generator) in line]
    if len(commands) != 1:
        raise RuntimeError('Expected one configured serializer generation command')
    words = shlex.split(commands[0])
    index = words.index(str(generator))
    interpreter = pathlib.Path(words[index - 1])
    arguments = words[index + 1:words.index('--output-dir', index)]
    if arguments[:2] != ['cpp', '--split-by-directory'] or not interpreter.is_file():
        raise RuntimeError('Unexpected native serializer generation command')
    inputs = probe.OUTPUT / 'serialization-inputs'
    watched = [generator]
    original_hashes = {}
    staged = []
    for item in arguments[2:]:
        original = pathlib.Path(item)
        if original.is_relative_to(source_root):
            relative = pathlib.Path('Source/WebKit') / original.relative_to(source_root)
        elif original.is_relative_to(probe.BUILD):
            relative = pathlib.Path('DerivedInputs') / original.relative_to(probe.BUILD)
        else:
            raise RuntimeError('Serializer input is outside the configured source/build trees: ' + item)
        if not relative.name.endswith('.serialization.in'):
            raise RuntimeError('Unexpected serializer input: ' + item)
        target = inputs / relative
        if not target.exists():
            data = original.read_bytes()
            original_hashes[str(original)] = hashlib.sha256(data).hexdigest()
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        watched.append(original)
        staged.append(str(target))
        name = str(target.relative_to(probe.OUTPUT))
        if name not in manifest['files']:
            manifest['files'][name] = {'source': str(target), 'sha256': probe.digest(target)}
    snapshot = {str(path): probe.digest(path) for path in watched}
    command = [str(interpreter), str(generator), 'cpp', '--split-by-directory', *staged, '--output-dir', str(probe.OUTPUT)]
    subprocess.run(command, cwd=inputs, check=True)
    if any(probe.digest(pathlib.Path(path)) != expected for path, expected in snapshot.items()):
        raise RuntimeError('Native serialization inputs changed during generation')
    generated = {path.name: probe.digest(path) for path in probe.OUTPUT.glob('GeneratedSerializers*')}
    cookie_schema = inputs / 'Source/WebKit/Shared/WebCoreArgumentCodersNetwork.serialization.in'
    if cookie_schema.exists() and 'struct WebCore::CookieChange {' in cookie_schema.read_text():
        if 'ArgumentCoder<WebCore::CookieChange>' not in (probe.OUTPUT / 'GeneratedSerializersSharedWebCoreArgumentCodersNetwork.cpp').read_text():
            raise RuntimeError('Cookie changes were not emitted in their configured Network serializer bundle')
    for name in units:
        if name in SERIALIZER_UNITS:
            manifest['files'][name] = {'source': str(probe.OUTPUT / name), 'sha256': generated[name], 'generated_from_serialization': True}
    manifest['serialization_generation'] = {'command': command, 'input_count': len(staged),
        'native_inputs': snapshot, 'copied_inputs': original_hashes, 'generated_files': generated}
    probe.EXTRA_WATCHED_INPUTS = (*getattr(probe, 'EXTRA_WATCHED_INPUTS', ()), *watched)
    (probe.OUTPUT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


def native(units=None, engine_root=DEFAULT_ENGINE, regenerate=False):
    spec = importlib.util.spec_from_file_location('extension_compile_probe',
        ROOT / 'tools/test-engine-extension-manifest-core.py')
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    probe.OUTPUT = ROOT / 'sources'
    probe.SOURCES = UNITS + EXTRA_UNITS + GENERATED_UNITS + SERIALIZER_UNITS + BINDING_UNITS + tuple(PAGE_UNITS) + tuple(NATIVE_PAGE_UNITS) + tuple(UI_API_UNITS) + tuple(WEB_CORE_UNITS)
    probe.ENGINE = pathlib.Path(engine_root)
    probe.BUILD = probe.ENGINE / 'WebKitBuild/Modern'
    probe.EXTRA_INCLUDE_DIRECTORIES = (probe.BUILD / 'WebCore/PrivateHeaders/WebCore',)
    if any(name in WEB_CORE_UNITS for name in units or ()):
        # WebKit's compile command omits WebCore's unexported sibling headers.
        # Use the configured WebCore search paths after the staged candidates.
        core_object = False
        for line in (probe.BUILD / 'build.ninja').read_text().splitlines():
            if line.startswith('build '):
                core_object = line.startswith('build Source/WebCore/CMakeFiles/WebCore.dir/') and '.cpp.o:' in line
            elif core_object and line.startswith('  INCLUDES = '):
                includes = shlex.split(line.split(' = ', 1)[1])
                probe.EXTRA_INCLUDE_DIRECTORIES = (probe.BUILD / 'WebCore/PrivateHeaders/WebCore',
                    *(pathlib.Path(flag[2:]) for flag in includes if flag.startswith('-I')))
                break
        else:
            raise RuntimeError('Configured WebCore include paths were not found')
    manifest = json.loads((probe.OUTPUT / 'source-manifest.json').read_text())
    engine = json.loads((probe.ENGINE / '.summit-source-manifest.json').read_text())
    if engine['patch_sha256'] != manifest['host_engine_patch_sha256']:
        raise RuntimeError('Native configured source does not match the staged host engine patch')
    if regenerate:
        regenerate_ipc(probe, manifest, units or UNITS)
        if any(name.startswith('serialization-inputs/') for name in manifest['files']):
            regenerate_serializers(probe, manifest, units or UNITS)
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
    if any(name in WEB_CORE_UNITS for name in units or ()):
        # Preserve quoted sibling headers when flattening a WebCore source unit.
        # Also stage the namespaced form so those headers cannot fall back to a
        # different native copy for their own <WebCore/...> includes.
        core_headers = set((engine / 'Source/WebCore/contentextensions').glob('*.h'))
        core_headers.add(engine / 'Source/WebCore/loader/ResourceLoadInfo.h')
        if overlay:
            for candidate in (pathlib.Path(overlay).resolve() / 'Source/WebCore/contentextensions').glob('*.h'):
                core_headers.add(engine / 'Source/WebCore/contentextensions' / candidate.name)
        for path in sorted(core_headers):
            for name in (path.name, 'WebCore/' + path.name):
                if name in files:
                    raise RuntimeError('Ambiguous staged WebCore header: ' + name)
                files[name] = (path, source(path), source(path).read_bytes())
    if overlay or any(name in (units or ()) for name in ('NetworkStorageSessionCurl.cpp', 'WebCookieManagerCurl.cpp')):
        for relative in ('platform/network/curl/CookieJarDB.h', 'platform/CookieChange.h', 'platform/Cookie.h'):
            path = engine / 'Source/WebCore' / relative
            if not source(path).is_file():
                continue
            for name in (path.name, 'WebCore/' + path.name):
                if name in files:
                    raise RuntimeError('Ambiguous staged cookie backend header: ' + name)
                files[name] = (path, source(path), source(path).read_bytes())
    for relative in ('Shared/NetworkStorageSession.h', 'UIProcess/Network/NetworkProcessProxy.h', 'NetworkProcess/Cookies/CookieStorage.h', 'UIProcess/API/APIHTTPCookieStore.h', 'NetworkProcess/Cookies/WebCookieManager.h', 'UIProcess/WebsiteData/WebsiteDataStore.h'):
        path = engine / 'Source/WebKit' / relative
        if source(path) != path or any(name in (units or ()) for name in ('APIHTTPCookieStore.cpp', 'WebCookieManagerCurl.cpp', 'WebCookieManagerMessageReceiver.cpp', 'WebsiteDataStore.cpp')):
            if path.name in files:
                raise RuntimeError('Ambiguous staged cookie API header: ' + path.name)
            files[path.name] = (path, source(path), source(path).read_bytes())
    if 'APIContentRuleListStore.cpp' in (units or ()):
        path = engine / 'Source/WebKit/UIProcess/API/APIContentRuleListStore.h'
        if path.name in files:
            raise RuntimeError('Ambiguous staged API header: ' + path.name)
        files[path.name] = (path, source(path), source(path).read_bytes())
    for name in units or UNITS:
        if name in BINDING_UNITS:
            if not generated_bindings:
                raise RuntimeError('Generated binding units require --generated-bindings')
            continue
        if name in GENERATED_UNITS + SERIALIZER_UNITS:
            if not regenerate:
                raise RuntimeError('Generated receiver units require --regenerate-ipc')
            continue
        directory = 'WebProcess' if name.endswith('Proxy.cpp') or name.startswith(('API/', 'Bindings/')) else 'UIProcess'
        relative = PAGE_UNITS.get(name, NATIVE_PAGE_UNITS.get(name, UI_API_UNITS.get(name, directory + '/Extensions/' + name)))
        path = engine / 'Source/WebCore' / WEB_CORE_UNITS[name] if name in WEB_CORE_UNITS else engine / 'Source/WebKit' / relative
        files[name] = (path, source(path), source(path).read_bytes())
    if any(name in PAGE_UNITS for name in units or ()) or (overlay and
            (pathlib.Path(overlay).resolve() / 'Source/WebKit/WebProcess/WebPage/WebPage.h').is_file()):
        # Keep quoted sibling includes on the same copied WebPage header.
        for path in sorted((engine / 'Source/WebKit/WebProcess/WebPage').glob('*.h')):
            if path.name in files:
                raise RuntimeError('Ambiguous staged page header: ' + path.name)
            files[path.name] = (path, source(path), source(path).read_bytes())
    if any(name in NATIVE_PAGE_UNITS or name.endswith('Haiku.cpp') for name in units or ()) or (overlay and
            (pathlib.Path(overlay).resolve() / 'Source/WebKit/UIProcess/haiku/WebViewPrivate.h').is_file()):
        for relative in ('UIProcess/haiku/WebViewPrivate.h', 'UIProcess/haiku/WebViewContextHaiku.h',
                         'UIProcess/haiku/WebViewStateHaiku.h', 'UIProcess/haiku/BrowserTabRegistryHaiku.h',
                         'UIProcess/haiku/ExtensionPermissionPromptHaiku.h',
                         'UIProcess/API/haiku/WebKitExtensionPermission.h',
                         'UIProcess/API/haiku/WebKitContext.h', 'UIProcess/API/haiku/WebKitView.h'):
            path = engine / 'Source/WebKit' / relative
            if not source(path).is_file():
                continue
            if path.name in files:
                raise RuntimeError('Ambiguous native page header: ' + path.name)
            files[path.name] = (path, source(path), source(path).read_bytes())
    if regenerate and overlay:
        for candidate in sorted((pathlib.Path(overlay).resolve() / 'Source/WebKit').rglob('*.messages.in')):
            relative = candidate.relative_to(pathlib.Path(overlay).resolve() / 'Source/WebKit')
            base = engine / 'Source/WebKit' / relative
            files['ipc-inputs/' + str(relative)] = (base, candidate, candidate.read_bytes())
    if regenerate and overlay:
        for candidate in sorted((pathlib.Path(overlay).resolve() / 'Source/WebKit').rglob('*.serialization.in')):
            relative = candidate.relative_to(pathlib.Path(overlay).resolve() / 'Source/WebKit')
            base = engine / 'Source/WebKit' / relative
            files['serialization-inputs/Source/WebKit/' + str(relative)] = (base, candidate, candidate.read_bytes())
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
    parser.add_argument('--unit', choices=UNITS + EXTRA_UNITS + GENERATED_UNITS + SERIALIZER_UNITS + BINDING_UNITS + tuple(PAGE_UNITS) + tuple(NATIVE_PAGE_UNITS) + tuple(UI_API_UNITS) + tuple(WEB_CORE_UNITS), action='append', help='Compile only this unit; repeatable')
    parser.add_argument('--engine-root', default=DEFAULT_ENGINE, help='Configured native engine source tree')
    parser.add_argument('--regenerate-ipc', action='store_true', help='Generate matching IPC headers in the isolated stage')
    parser.add_argument('--generated-bindings', help='Verified output from generate-extension-bindings-candidate.py')
    args = parser.parse_args()
    raise SystemExit(native(args.unit, args.engine_root, args.regenerate_ipc) if args.native else host(args.overlay, args.unit, args.engine_root, args.regenerate_ipc, args.generated_bindings))
