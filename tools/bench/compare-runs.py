#!/usr/bin/env python3
"""Per-suite comparison of two Speedometer result.json files (as written by serve-speedometer.py).

  python3 tools/bench/compare-runs.py .vm/bench/<run-A> .vm/bench/<run-B>

Prints, for every suite, the mean time of A and B, the ratio A/B, that ratio relative
to the geometric mean of all ratios ("rel": >1 means A is unusually slow on this
suite compared with its overall gap to B), and the same ratio split into the
synchronous part (script + forced style/layout inside the rAF callback) and the
asynchronous part (rendering update, paint, frame hand-off, zero-delay timer).
"""
import json
import math
import pathlib
import sys


def load(path):
    path = pathlib.Path(path)
    if path.is_dir():
        path = path / 'result.json'
    data = json.loads(path.read_text())
    return data.get('payload', data)


def split(metrics, suite):
    sync = sum(v['mean'] for k, v in metrics.items() if k.startswith(suite + '/') and k.endswith('/Sync'))
    asynchronous = sum(v['mean'] for k, v in metrics.items() if k.startswith(suite + '/') and k.endswith('/Async'))
    return sync, asynchronous


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = load(sys.argv[1]), load(sys.argv[2])
    viewport_a = (a.get('environment', {}).get('innerWidth'), a.get('environment', {}).get('innerHeight'))
    viewport_b = (b.get('environment', {}).get('innerWidth'), b.get('environment', {}).get('innerHeight'))
    if viewport_a != viewport_b:
        print(f'WARNING: content viewports differ: A {viewport_a[0]}x{viewport_a[1]}, '
              f'B {viewport_b[0]}x{viewport_b[1]}; score and suite ratios may include viewport work')
    ma, mb = a['metrics'], b['metrics']
    suites = [k for k in ma if '/' not in k and not k.startswith('Iteration-') and k not in ('Geomean', 'Score')
              and k in mb]
    rows = []
    for suite in suites:
        (sa, aa), (sb, ab) = split(ma, suite), split(mb, suite)
        rows.append((suite, ma[suite]['mean'], mb[suite]['mean'], ma[suite]['mean'] / mb[suite]['mean'],
                     sa / sb if sb else math.nan, aa / ab if ab else math.nan, sa, aa, sb, ab))
    geomean = math.exp(sum(math.log(r[3]) for r in rows) / len(rows))
    print(f"A score {a['score']['mean']:.3f} ± {a['score'].get('delta') or 0:.3f}   "
          f"B score {b['score']['mean']:.3f} ± {b['score'].get('delta') or 0:.3f}   geomean of A/B time ratios {geomean:.2f}")
    print(f"{'suite':44} {'A ms':>9} {'B ms':>9} {'A/B':>7} {'rel':>5} {'sync A/B':>9} {'async A/B':>10} {'A async %':>10}")
    for r in sorted(rows, key=lambda r: -r[3]):
        print(f'{r[0]:44} {r[1]:9.1f} {r[2]:9.1f} {r[3]:7.2f} {r[3] / geomean:5.2f} {r[4]:9.2f} {r[5]:10.2f} '
              f'{100 * r[7] / (r[6] + r[7]):9.1f}%')
    ta, tb = sum(r[6] + r[7] for r in rows), sum(r[8] + r[9] for r in rows)
    print(f"totals: A {ta:.0f} ms/iteration (async {100 * sum(r[7] for r in rows) / ta:.1f}%), "
          f"B {tb:.0f} ms/iteration (async {100 * sum(r[9] for r in rows) / tb:.1f}%)")


if __name__ == '__main__':
    main()
