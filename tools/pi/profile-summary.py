#!/usr/bin/env python3
"""Summarise Haiku `profile -a` output: threads with at least MIN ticks, the
executable their team runs (first image of the image table), and their
hottest functions.  profile-summary.py FILE [MIN_TICKS] [TOP] [FILTER]"""
import re, sys
path = sys.argv[1]
minimum = int(sys.argv[2]) if len(sys.argv) > 2 else 20
top = int(sys.argv[3]) if len(sys.argv) > 3 else 8
needle = sys.argv[4] if len(sys.argv) > 4 else ''
text = open(path, errors='replace').read()
blocks = re.split(r'\nprofiling results for thread ', '\n' + text)
rows = []
for block in blocks[1:]:
    head = re.match(r'"(.*)" \((\d+)\):', block)
    if not head:
        continue
    name, tid = head.groups()
    ticks = re.search(r'total ticks:\s+(\d+) \((\d+) us\)', block)
    if not ticks:
        continue
    images = re.findall(r'^\s+\d+\s+\d+\s+\d+\s+(/\S+)$', block, re.M)
    exe = next((i for i in images if '/lib/' not in i and 'runtime_loader' not in i and '/add-ons/' not in i), images[0] if images else '?')
    funcs = re.findall(r'^\s+(\d+)\s+(\d+)\s+([\d.]+)\s+\d+\s+(.+)$', block, re.M)
    rows.append((int(ticks.group(1)), name, tid, exe, funcs))
for ticks, name, tid, exe, funcs in sorted(rows, reverse=True):
    if ticks < minimum or (needle and needle not in exe and needle not in name):
        continue
    print(f'{ticks:6} ticks  {name} ({tid})  [{exe}]')
    for hits, us, pct, func in funcs[:top]:
        print(f'         {pct:>6}%  {func[:110]}')
