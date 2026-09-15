#!/usr/bin/env python3
"""Check actual generated operation guards with the C++ preprocessor, without compiling WebKit."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(generated):
    generated = Path(generated).resolve()
    generation = json.loads((generated / 'binding-generation.json').read_text())
    if not generation['passed']:
        raise RuntimeError('Binding generation failed')
    for path, expected in generation['inputs'].items():
        if digest(Path(path)) != expected:
            raise RuntimeError('Binding input changed: ' + path)
    scripts = generated / 'generator'
    for path in scripts.iterdir():
        originals = [Path(name) for name in generation['inputs'] if name.endswith('/Bindings/Scripts/' + path.name)]
        if originals and digest(path) != digest(originals[0]):
            raise RuntimeError('Staged generator differs: ' + path.name)
    output = Path(tempfile.mkdtemp(prefix='extension-binding-platforms-', dir=ROOT / '.vm'))
    idl = output / 'WebExtensionAPIPlatformProbe.idl'
    idl.write_text('''[Conditional=WK_WEB_EXTENSIONS, UseCPPAPI] interface WebExtensionAPIPlatformProbe {
    [Platform=HAIKU] void nativeOnly();
    [Platform=HAIKU, Conditional=FEATURE_A|FEATURE_B] void eitherFeature();
    [Platform=HAIKU, Conditional=FEATURE_A&FEATURE_B, MainWorldOnly] void bothFeatures();
    [Platform=HAIKU, Dynamic] void dynamicNative();
    [Conditional=FEATURE_A] void featureOnly();
};
''')
    core = ROOT / '.cache/WebKit/Source/WebCore/bindings/scripts'
    listing = output / 'idls.txt'
    listing.write_text(str(idl) + '\n')
    command = ['perl', '-I', str(scripts), str(core / 'generate-bindings.pl'),
               '--outputDir', str(output), '--generator', 'Extensions',
               '--idlAttributesFile', str(scripts / 'IDLAttributes.json'),
               '--idlFileNamesList', str(listing), str(idl)]
    subprocess.run(command, check=True, capture_output=True, text=True)
    compiler = shutil.which('c++')
    if not compiler:
        raise RuntimeError('C++ preprocessor unavailable')
    records = []
    checks = 0
    for haiku in (0, 1):
        for a in (0, 1):
            for b in (0, 1):
                expected = {'nativeOnly': bool(haiku), 'eitherFeature': bool(haiku and (a or b)),
                            'bothFeatures': bool(haiku and a and b), 'dynamicNative': bool(haiku), 'featureOnly': bool(a)}
                for suffix in ('h', 'cpp'):
                    source = output / ('JSWebExtensionAPIPlatformProbe.' + suffix)
                    text = source.read_text()
                    text = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', text, flags=re.M)
                    prefix = f'''#define ENABLE(x) FLAG_##x
#define PLATFORM(x) PLATFORM_##x
#define FLAG_WK_WEB_EXTENSIONS 1
#define FLAG_FEATURE_A {a}
#define FLAG_FEATURE_B {b}
#define PLATFORM_HAIKU {haiku}
#define PLATFORM_COCOA {1-haiku}
'''
                    result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + text,
                                            text=True, capture_output=True, check=True)
                    for name, present in expected.items():
                        checks += 1
                        actual = bool(re.search(r'\b' + name + r'\b', result.stdout))
                        if actual != present:
                            raise RuntimeError(f'{suffix}: {name}, Haiku={haiku}, A={a}, B={b}: expected {present}')
                    records.append({'source': source.name, 'haiku': haiku, 'feature_a': a, 'feature_b': b,
                                    'preprocessed_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
    for haiku in (0, 1):
        for suffix in ('h', 'cpp'):
            source = generated / ('JSWebExtensionAPICommands.' + suffix)
            if digest(source) != generation['files'][source.name]:
                raise RuntimeError('Generated commands binding changed')
            stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
            prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
            result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                    text=True, capture_output=True, check=True)
            for name, present in {'getAll': True, 'update': bool(haiku), 'reset': bool(haiku)}.items():
                checks += 1
                if bool(re.search(r'\b' + name + r'\b', result.stdout)) != present:
                    raise RuntimeError('Incorrect command binding exposure: ' + name)
            for interface, names in {
                'Permissions': ('getAll', 'contains', 'request', 'remove', 'onAdded', 'onRemoved'),
                'Namespace': ('permissions',),
            }.items():
                source = generated / ('JSWebExtensionAPI' + interface + '.' + suffix)
                if digest(source) != generation['files'][source.name]:
                    raise RuntimeError('Generated binding changed: ' + source.name)
                stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                        text=True, capture_output=True, check=True)
                for name in names:
                    checks += 1
                    if not re.search(r'\b' + name + r'\b', result.stdout):
                        raise RuntimeError(f'Missing {interface}.{name}: Haiku={haiku}, source={suffix}')
    if (generated / 'JSWebExtensionAPITabs.cpp').exists():
        common = ('query', 'get', 'getCurrent', 'tabIdentifierNone', 'onActivated', 'onAttached', 'onCreated',
                  'onDetached', 'onHighlighted', 'onMoved', 'onRemoved', 'onReplaced', 'onUpdated')
        cocoa_only = ('create', 'getSelected', 'duplicate', 'update', 'move', 'remove', 'reload', 'goBack',
                      'goForward', 'getZoom', 'setZoom', 'detectLanguage', 'toggleReaderMode', 'captureVisibleTab',
                      'executeScript', 'insertCSS', 'removeCSS', 'sendMessage', 'connect')
        for haiku in (0, 1):
            for suffix in ('h', 'cpp'):
                source = generated / ('JSWebExtensionAPITabs.' + suffix)
                if digest(source) != generation['files'][source.name]:
                    raise RuntimeError('Generated tabs binding changed')
                stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
                result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                        text=True, capture_output=True, check=True)
                for name in common + cocoa_only:
                    checks += 1
                    pattern = r'\bstatic JSValueRef ' + name + r'\(' if suffix == 'h' else r'\bJSWebExtensionAPITabs::' + name + r'\('
                    present = bool(re.search(pattern, result.stdout))
                    if present != (name in common or not haiku):
                        raise RuntimeError(f'Incorrect tabs.{name} exposure: Haiku={haiku}, source={suffix}')
                namespace = generated / ('JSWebExtensionAPINamespace.' + suffix)
                if digest(namespace) != generation['files'][namespace.name]:
                    raise RuntimeError('Generated namespace binding changed')
                stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', namespace.read_text(), flags=re.M)
                result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                        text=True, capture_output=True, check=True)
                pattern = r'\bstatic JSValueRef tabs\(' if suffix == 'h' else r'\bJSWebExtensionAPINamespace::tabs\('
                checks += 1
                if not re.search(pattern, result.stdout):
                    raise RuntimeError(f'Missing tabs namespace: Haiku={haiku}, source={suffix}')
    if (generated / 'JSWebExtensionAPIWindows.cpp').exists():
        surfaces = {
            'Windows': (('get', 'getCurrent', 'getLastFocused', 'getAll', 'windowIdentifierNone',
                         'windowIdentifierCurrent', 'onCreated', 'onRemoved', 'onFocusChanged'), ('create', 'update', 'remove')),
            'WindowsEvent': (('addListener', 'removeListener', 'hasListener'), ()),
            'Namespace': (('windows',), ()),
        }
        for haiku in (0, 1):
            for suffix in ('h', 'cpp'):
                prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
                for interface, (common, cocoa_only) in surfaces.items():
                    source = generated / ('JSWebExtensionAPI' + interface + '.' + suffix)
                    if digest(source) != generation['files'][source.name]:
                        raise RuntimeError('Generated binding changed: ' + source.name)
                    stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                    result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                            text=True, capture_output=True, check=True)
                    if haiku and suffix == 'cpp' and interface != 'Namespace':
                        checks += 1
                        if re.search(r'\b(?:toNSDictionary|NSString|NSDictionary)\b', result.stdout):
                            raise RuntimeError('Native binding contains Objective-C argument handling: ' + interface)
                    for name in common + cocoa_only:
                        pattern = r'\bstatic JSValueRef ' + name + r'\(' if suffix == 'h' else r'\bJSWebExtensionAPI' + interface + '::' + name + r'\('
                        checks += 1
                        if bool(re.search(pattern, result.stdout)) != (name in common or not haiku):
                            raise RuntimeError(f'Incorrect {interface}.{name} exposure: Haiku={haiku}, source={suffix}')
    if (generated / 'JSWebExtensionAPIAction.cpp').exists():
        common = ('getTitle', 'setTitle', 'getBadgeText', 'setBadgeText', 'getBadgeBackgroundColor', 'setBadgeBackgroundColor',
                  'enable', 'disable', 'isEnabled', 'getPopup', 'setPopup', 'onClicked')
        haiku_only = ('getBadgeTextColor', 'setBadgeTextColor')
        cocoa_only = ('setIcon', 'openPopup')
        for haiku in (0, 1):
            for suffix in ('h', 'cpp'):
                prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
                for interface, names in (('Action', common + haiku_only + cocoa_only), ('Namespace', ('action', 'browserAction', 'pageAction'))):
                    source = generated / ('JSWebExtensionAPI' + interface + '.' + suffix)
                    if digest(source) != generation['files'][source.name]:
                        raise RuntimeError('Generated action binding changed: ' + source.name)
                    stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                    result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                            text=True, capture_output=True, check=True)
                    for name in names:
                        checks += 1
                        pattern = r'\bstatic JSValueRef ' + name + r'\(' if suffix == 'h' else r'\bJSWebExtensionAPI' + interface + '::' + name + r'\('
                        expected = interface == 'Namespace' or name in common or name in (haiku_only if haiku else cocoa_only)
                        if bool(re.search(pattern, result.stdout)) != expected:
                            raise RuntimeError(f'Incorrect {interface}.{name} exposure: Haiku={haiku}, source={suffix}')
                    if interface == 'Action' and suffix == 'cpp':
                        checks += 1
                        if re.search(r'\b(?:toNSDictionary|NSString|NSDictionary)\b', result.stdout):
                            raise RuntimeError('Action C++ binding contains Objective-C argument conversion')
                        for name in ('enable', 'disable'):
                            checks += 1
                            if f'impl->{name}(context, tabId,' not in result.stdout:
                                raise RuntimeError('Action tab identifier must retain its raw JavaScript value')
                        if not haiku:
                            checks += 1
                            if 'impl->setIcon(*frame, context, details,' not in result.stdout:
                                raise RuntimeError('Cocoa ImageData must retain the raw JavaScript value')
    if (generated / 'JSWebExtensionAPICookies.cpp').exists():
        for haiku in (0, 1):
            for interface, members in (('Cookies', {'get': True, 'getAll': True, 'set': True, 'remove': True,
                                                  'getAllCookieStores': True, 'onChanged': True}),
                                       ('Namespace', {'cookies': True})):
                for suffix in ('h', 'cpp'):
                    source = generated / ('JSWebExtensionAPI' + interface + '.' + suffix)
                    if digest(source) != generation['files'][source.name]:
                        raise RuntimeError('Generated cookie binding changed: ' + source.name)
                    stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                    prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
                    result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                            text=True, capture_output=True, check=True)
                    for name, present in members.items():
                        checks += 1
                        if bool(re.search(r'\b' + name + r'\b', result.stdout)) != present:
                            raise RuntimeError('Incorrect cookie binding exposure: ' + name)
                    if interface == 'Cookies' and suffix == 'cpp':
                        for name in ('get', 'getAll', 'set', 'remove'):
                            checks += 1
                            if f'impl->{name}(details, callback.releaseNonNull(), exceptionString)' not in result.stdout:
                                raise RuntimeError('Cookie method lost its C++ details/exception adapter: ' + name)
                        checks += 1
                        if 'String exceptionString;' not in result.stdout or 'NSString' in result.stdout:
                            raise RuntimeError('C++ cookie bindings retain an Objective-C exception path')
                    records.append({'source': source.name, 'haiku': haiku,
                                    'preprocessed_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
    if (generated / 'JSWebExtensionAPIMenus.cpp').exists():
        methods = ('create', 'update', 'remove', 'removeAll', 'onClicked', 'actionMenuTopLevelLimit')
        calls = ('impl->createMenu(*page, *frame, context, properties,',
                 'impl->update(*page, *frame, context, identifier, properties,',
                 'impl->remove(context, identifier,', 'impl->removeAll(')
        for haiku in (0, 1):
            for interface, names in (('Menus', methods), ('Namespace', ('menus', 'contextMenus'))):
                for suffix in ('h', 'cpp'):
                    source = generated / ('JSWebExtensionAPI' + interface + '.' + suffix)
                    if digest(source) != generation['files'][source.name]:
                        raise RuntimeError('Generated menu binding changed: ' + source.name)
                    stripped = re.sub(r'^\s*#(?:include|import|pragma)\b[^\n]*', '', source.read_text(), flags=re.M)
                    prefix = f'#define ENABLE(x) 1\n#define PLATFORM(x) PLATFORM_##x\n#define PLATFORM_HAIKU {haiku}\n#define PLATFORM_COCOA {1-haiku}\n'
                    result = subprocess.run([compiler, '-E', '-P', '-x', 'c++', '-'], input=prefix + stripped,
                                            text=True, capture_output=True, check=True)
                    for name in names:
                        checks += 1
                        pattern = r'\bstatic JSValueRef ' + name + r'\(' if suffix == 'h' else r'\bJSWebExtensionAPI' + interface + '::' + name + r'\('
                        if not re.search(pattern, result.stdout):
                            raise RuntimeError('Missing native or Cocoa menu surface: ' + name)
                    if suffix == 'cpp' and interface == 'Menus':
                        for call in calls:
                            checks += 1
                            if call not in result.stdout:
                                raise RuntimeError('Menu method lost its raw C++ adapter: ' + call)
                        checks += 1
                        if re.search(r'\b(?:toNSDictionary|toNSObject|NSString|NSDictionary)\b', result.stdout):
                            raise RuntimeError('C++ menu bindings contain Objective-C conversion')
                    if suffix == 'cpp':
                        for name in names:
                            match = re.search(r'JSValueRef JSWebExtensionAPI' + interface + '::' + name + r'\([^\n]*\)\n\{(.*?)\n\}', result.stdout, re.S)
                            checks += 1
                            if not match:
                                raise RuntimeError('Missing menu getter or operation: ' + name)
                            if interface == 'Namespace':
                                checks += 1
                                gate = f'isForMainWorld && JSStringIsEqualToUTF8CString(propertyName, "{name}") && impl->isPropertyAllowed("{name}"_s, page.get())'
                                if gate not in result.stdout:
                                    raise RuntimeError('Menu alias lost its dynamic main-world permission gate: ' + name)
                            elif 'impl->isForMainWorld()' not in match[1]:
                                raise RuntimeError('Menu operation lost its main-world restriction: ' + name)
                            if interface == 'Menus' and name in ('create', 'update', 'remove', 'removeAll'):
                                checks += 1
                                if ('JSObjectMakeDeferredPromise' in match[1]) != (name != 'create'):
                                    raise RuntimeError('Menu synchronous ID or optional promise behavior changed: ' + name)
                    records.append({'source': source.name, 'haiku': haiku,
                                    'preprocessed_sha256': hashlib.sha256(result.stdout.encode()).hexdigest()})
    report = {'scope': 'actual Perl generation and C++ preprocessing; no Cocoa or Haiku engine compile/runtime',
              'generation': str(generated), 'fixture_sha256': digest(idl), 'command': command,
              'checks': checks, 'cases': records, 'passed': True}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': str(output / 'result.json'), 'checks': checks, 'passed': True}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated-bindings', required=True)
    main(parser.parse_args().generated_bindings)
