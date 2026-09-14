#!/usr/bin/env python3
"""Compile upstream extension core in isolation; this is not a runtime test.

Uses the configured native Modern target without invoking CMake or Ninja.
Only the copied cmakeconfig.h enables WK_WEB_EXTENSIONS. A failed compile
remains a failed gate; missing implementations are never replaced by stubs.
"""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit')
BUILD = ENGINE / 'WebKitBuild/Modern'
OUTPUT = ROOT / 'build-extension-manifest-tests'
OBJECT = 'Source/WebKit/CMakeFiles/WebKit.dir/UIProcess/API/haiku/WebKitView.cpp.o'
SOURCES = ('WebExtension.cpp', 'WebExtensionMatchPattern.cpp',
           'WebExtensionLocalization.cpp', 'WebExtensionPermission.cpp',
           'WebExtensionUtilities.cpp', 'WebExtensionResources.cpp',
           'haiku/WebExtensionHaiku.cpp')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_scheme_registration(text):
    """Move the original method intact, retaining it for the full WebKit target."""
    signature = 'void WebExtensionMatchPattern::registerCustomURLScheme(String urlScheme)\n'
    next_signature = 'bool WebExtensionMatchPattern::isWebExtensionURL(const URL& url)\n'
    if text.count(signature) != 1 or text.count(next_signature) != 1:
        raise SystemExit('The scheme registration extraction no longer applies exactly.')
    begin, end = text.index(signature), text.index(next_signature)
    method = text[begin:end]
    original_license = text[:text.index('#include "config.h"')]
    includes = ('#include "WebProcessMessages.h"\n', '#include "WebProcessPool.h"\n',
                '#include <WebCore/LegacySchemeRegistry.h>\n')
    core = text[:begin] + text[end:]
    for include in includes:
        if core.count(include) != 1:
            raise SystemExit('Expected one scheme-registration include: ' + include)
        core = core.replace(include, '')
    integration = (original_license + '#include "config.h"\n#include "WebExtensionMatchPattern.h"\n\n'
                   '#if ENABLE(WK_WEB_EXTENSIONS)\n\n' + ''.join(includes)
                   + '#include <wtf/URLParser.h>\n\nnamespace WebKit {\n\n'
                   'using namespace WTF;\nusing namespace WebCore;\n\n' + method
                   + '} // namespace WebKit\n\n#endif // ENABLE(WK_WEB_EXTENSIONS)\n')
    return core, integration


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', choices=SOURCES, action='append',
                        help='Compile only this upstream translation unit; repeatable.')
    parser.add_argument('--header-fix', action='store_true',
                        help='Apply the proposed GLib constructor guard only in the isolated header copy.')
    parser.add_argument('--core-extraction', action='store_true',
                        help='Also move the intact scheme-registration method to its own copied translation unit.')
    parser.add_argument('--content-extensions', action='store_true',
                        help='Also enable the content-rule feature in the isolated configuration.')
    parser.add_argument('--configured-extensions', action='store_true',
                        help='Use a feature-enabled configuration without changing its feature gates.')
    args = parser.parse_args()
    OUTPUT.mkdir(exist_ok=True)
    watched_inputs = (BUILD / 'build.ninja', ENGINE / '.summit-source-manifest.json',
                      ENGINE / 'Source/WebKit/WebKitPrefix.h')
    input_snapshot = {str(path): digest(path) for path in watched_inputs}
    fields = {}
    with (BUILD / 'build.ninja').open() as stream:
        for line in stream:
            if line.startswith('build ' + OBJECT + ':'):
                for line in stream:
                    if not line.strip():
                        break
                    key, _, value = line.strip().partition(' = ')
                    fields[key] = value
                break
    if not all(key in fields for key in ('DEFINES', 'INCLUDES', 'FLAGS')):
        raise SystemExit('Configure the native Modern WebKit target before this compile preflight.')

    config = (BUILD / 'cmakeconfig.h').read_text()
    original_gate = '#define ENABLE_WK_WEB_EXTENSIONS ' + ('1' if args.configured_extensions else '0')
    if config.count(original_gate) != 1:
        raise SystemExit('Expected exactly one matching extension feature in the Modern configuration.')
    isolated_config = config.replace(original_gate, '#define ENABLE_WK_WEB_EXTENSIONS 1')
    if args.content_extensions:
        content_gate = '#define ENABLE_CONTENT_EXTENSIONS ' + ('1' if args.configured_extensions else '0')
        if isolated_config.count(content_gate) != 1:
            raise SystemExit('Expected one matching content-rule feature in the Modern configuration.')
        isolated_config = isolated_config.replace(content_gate, '#define ENABLE_CONTENT_EXTENSIONS 1')
    (OUTPUT / 'cmakeconfig.h').write_text(isolated_config)
    # Preserve the production prefix, but do not consume its feature-disabled PCH.
    prefix = ENGINE / 'Source/WebKit/WebKitPrefix.h'
    raw_flags = shlex.split(' '.join(fields[key] for key in ('DEFINES', 'INCLUDES', 'FLAGS')))
    flags = []
    iterator = iter(raw_flags)
    for flag in iterator:
        if flag == '-include':
            next(iterator)
        elif not flag.startswith(('-fdiagnostics-color=', '-fmax-errors=')):
            flags.append(flag)
    flags = ['-I' + str(OUTPUT), '-iquote', str(OUTPUT), *flags,
             '-include', str(prefix), '-fdiagnostics-color=never', '-fmax-errors=5']
    source_manifest = json.loads((OUTPUT / 'source-manifest.json').read_text())
    for name, info in source_manifest['files'].items():
        if digest(OUTPUT / name) != info['sha256']:
            raise SystemExit('Source snapshot changed; rerun the upload wrapper: ' + name)
    header_fix = None
    if args.header_fix or args.core_extraction:
        header = OUTPUT / 'WebExtension.h'
        original = '#else\n    explicit WebExtension(GFile *resourcesFile, RefPtr<API::Error>&);'
        text = header.read_text()
        if text.count(original) != 1 and '#elif USE(GLIB)\n    explicit WebExtension(GFile ' not in text:
            raise SystemExit('The proposed GLib constructor guard no longer applies exactly.')
        header.write_text(text.replace(original, original.replace('#else', '#elif USE(GLIB)')))
        header_fix = {'file': 'WebExtension.h', 'original_sha256': source_manifest['files']['WebExtension.h']['sha256'],
                      'isolated_sha256': digest(header), 'change': '#else -> #elif USE(GLIB) before GFile constructor'}
    extraction = None
    if args.core_extraction:
        matcher = OUTPUT / 'WebExtensionMatchPattern.cpp'
        integration = OUTPUT / 'WebExtensionMatchPatternProcessPool.cpp'
        if 'void WebExtensionMatchPattern::registerCustomURLScheme' in matcher.read_text():
            core_text, integration_text = extract_scheme_registration(matcher.read_text())
            matcher.write_text(core_text)
            integration.write_text(integration_text)
        elif not integration.exists():
            raise SystemExit('Extracted production scheme registration source is missing.')
        extraction = {'core': matcher.name, 'core_sha256': digest(matcher),
                      'integration': integration.name, 'integration_sha256': digest(integration),
                      'integration_compiled': False,
                      'change': 'Move registerCustomURLScheme unchanged to full-runtime translation unit'}
    report = {
        'scope': 'compile-only; no extension runtime or manifest assertions executed',
        'configured_object': OBJECT,
        'original_cmakeconfig_sha256': hashlib.sha256(config.encode()).hexdigest(),
        'isolated_cmakeconfig_sha256': digest(OUTPUT / 'cmakeconfig.h'),
        'native_engine_patch_sha256': json.loads((ENGINE / '.summit-source-manifest.json').read_text())['patch_sha256'],
        'native_input_snapshot': input_snapshot,
        'source_manifest': source_manifest,
        'proposed_header_fix': header_fix,
        'proposed_scheme_registration_extraction': extraction,
        'content_extensions_enabled_for_probe': args.content_extensions,
        'uses_feature_enabled_configuration': args.configured_extensions,
        'units': [],
    }
    for name in args.source or SOURCES:
        source = OUTPUT / name
        expected = report['source_manifest']['files'][name]['sha256']
        if extraction and name == extraction['core']:
            expected = extraction['core_sha256']
        if digest(source) != expected:
            raise SystemExit('Source snapshot changed: ' + name)
        obj = OUTPUT / (source.stem + '.o')
        obj.unlink(missing_ok=True)
        command = ['c++', *flags, '-c', str(source), '-o', str(obj)]
        started = time.monotonic()
        print('COMPILE', name, flush=True)
        result = subprocess.run(command, cwd=BUILD, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        (OUTPUT / (source.stem + '.log')).write_text(result.stdout)
        print(result.stdout, end='', flush=True)
        report['units'].append({'source': name, 'exit_code': result.returncode,
                                'elapsed_seconds': round(time.monotonic() - started, 2),
                                'source_sha256': digest(source),
                                'object_sha256': digest(obj) if obj.exists() else None})
        print('COMPILE_RESULT', name, result.returncode, flush=True)
    report['all_compiled'] = all(unit['exit_code'] == 0 for unit in report['units'])
    report['engine_configuration_unchanged'] = digest(BUILD / 'cmakeconfig.h') == report['original_cmakeconfig_sha256']
    report['native_input_snapshot_unchanged'] = all(digest(pathlib.Path(path)) == expected
                                                  for path, expected in input_snapshot.items())
    (OUTPUT / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['engine_configuration_unchanged'] or not report['native_input_snapshot_unchanged']:
        raise SystemExit('Engine source/configuration changed during the preflight; results require another snapshot.')
    if not report['all_compiled']:
        raise SystemExit('Upstream extension core compile gate failed; see per-source logs and results.json.')
    print(str(len(report['units'])) + ' upstream extension core translation unit(s) compiled. '
          'No linking or runtime assertions were performed.')


if __name__ == '__main__':
    main()
