#!/usr/bin/env python3
"""Record raw conformance counts and compare failures with pinned expectations."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re

import yaml


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results', type=Path)
    parser.add_argument('log', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    corpus = root / '.cache/WebKit/JSTests/test262'
    load = lambda path: yaml.load(path.read_text(), Loader=getattr(yaml, 'CSafeLoader', yaml.SafeLoader))
    results = load(args.results / 'results.yaml')
    failures = [r for r in results if r['result'].endswith('fail')]
    exits = {(path, mode): int(code) for path, mode, code in re.findall(
        r'^! NEW FAIL (.*?) \((default|strict mode|module|raw)\) \(Exit code: (\d+)\)$',
        args.log.read_text(), re.MULTILINE)}
    if len(exits) != len(failures) or any((r['path'], r['mode']) not in exits for r in failures):
        raise SystemExit('The log must contain every failure from this result report (ignore expectations when running).')
    comparisons = {}
    expectations = {}
    for name in ['expectations.yaml', 'expectations-linux.yaml']:
        expected = load(corpus / name)
        expectations[name] = expected
        comparisons[name] = {
            'sha256': digest(corpus / name),
            'matching_failure_exit_codes': sum(
                expected.get(r['path'], {}).get(r['mode']) == exits[(r['path'], r['mode'])]
                for r in failures),
        }
    counts = collections.Counter(r['result'] for r in results)
    output = {
        'upstream_commit': json.loads((root / 'engine/sources.lock.json').read_text())['upstream']['commit'],
        'counts': {
            'discovered_files': len({r['path'] for r in results}),
            'executions': len(results) - counts['skip'],
            'passed': sum(n for result, n in counts.items() if result.endswith('pass')),
            'failed': len(failures),
            'failed_files': len({r['path'] for r in failures}),
            'skipped_files': counts['skip'],
            'failure_exit_codes': dict(collections.Counter(exits.values())),
        },
        'engine_sha256': (args.results / 'engine-sha256.txt').read_text().splitlines(),
        'results_sha256': digest(args.results / 'results.yaml'),
        'log_sha256': digest(args.log),
        'skip_config_sha256': digest(corpus / 'config.yaml'),
        'expectations_comparison': comparisons,
        'failures': [
            {key: value for key, value in {
                'path': r['path'], 'mode': r['mode'],
                'exit_code': exits[(r['path'], r['mode'])],
                'error': r.get('error'),
                'timeout': r.get('output') == 'Timeout',
                'upstream_matching_expectations': [name for name, expected in expectations.items()
                    if expected.get(r['path'], {}).get(r['mode']) == exits[(r['path'], r['mode'])]],
            }.items() if value is not None}
            for r in failures
        ],
    }
    args.output.write_text(json.dumps(output, indent=2, ensure_ascii=False) + '\n')
    print(json.dumps({'counts': output['counts'], 'expectations_comparison': comparisons}, indent=2))


if __name__ == '__main__':
    main()
