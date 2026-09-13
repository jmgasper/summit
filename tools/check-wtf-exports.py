#!/usr/bin/env python3
"""Check captured native dynamic exports against the preserved WTF link failure.

Inputs must come from `nm -D -C --defined-only`, not a static archive or ordinary
symbol table. This checks export coverage and selected ownership sentinels; it
does not claim an engine build or runtime test passed.
"""
import argparse
import hashlib
import json
import pathlib
import re


SENTINELS = ('WTF::initializeMainThread()', 'WTF::RunLoop::mainSingleton()',
             'WTF::MemoryPressureHandler::singleton()', 'g_config')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exports(path):
    result = set()
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'\s*[0-9a-fA-F]+\s+[A-Za-z]\s+(.+)', line)
        if match:
            result.add(match[1])
    if not result:
        raise ValueError('No defined dynamic symbols found in ' + str(path))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--missing', type=pathlib.Path, required=True,
                        help='Preserved tests/fixtures/modern-link-missing-symbols.json')
    parser.add_argument('--jsc-exports', type=pathlib.Path, required=True)
    parser.add_argument('--webkit-exports', type=pathlib.Path,
                        help='Also check that these WTF definitions remain owned by JSC')
    parser.add_argument('--output', type=pathlib.Path, required=True)
    args = parser.parse_args()
    missing = json.loads(args.missing.read_text())
    # The preserved compiler diagnostics include the return type on this
    # explicit template instantiation, unlike the other WTF symbols.
    expected = {name for name in missing
                if name.startswith('WTF::') or name.startswith('unsigned int WTF::weakRandomNumber<')}
    if len(expected) != 115:
        raise ValueError('Expected the frozen set of 115 unresolved WTF symbols; found ' + str(len(expected)))
    jsc = exports(args.jsc_exports)
    unresolved = sorted(expected - jsc)
    missing_sentinels = sorted(set(SENTINELS) - jsc)
    duplicate = None
    paths = [args.missing, args.jsc_exports]
    if args.webkit_exports:
        duplicate = sorted((expected | set(SENTINELS)) & exports(args.webkit_exports))
        paths.append(args.webkit_exports)
    report = {
        'kind': 'captured-native-wtf-dynamic-export-check',
        'input_sha256': {str(path): digest(path) for path in paths},
        'expected_wtf_count': len(expected),
        'jsc_exported_expected_count': len(expected) - len(unresolved),
        'missing_wtf_exports': unresolved,
        'missing_jsc_ownership_sentinels': missing_sentinels,
        'webkit_ownership_checked': duplicate is not None,
        'duplicate_wtf_definitions_in_webkit': duplicate,
        'passed': not unresolved and not missing_sentinels and not duplicate,
        'scope': 'dynamic export coverage only; no engine build or runtime assertions',
    }
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'WTF dynamic exports: {len(expected) - len(unresolved)}/115; '
          f'ownership sentinels missing: {len(missing_sentinels)}; '
          f'WebKit duplicates: {len(duplicate) if duplicate is not None else "not checked"}')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
