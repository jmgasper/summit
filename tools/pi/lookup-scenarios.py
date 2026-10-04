#!/usr/bin/env python3
"""Like scope.py, totals only, under scenarios: GNU hash for given images,
and -Bsymbolic-functions (own FUNC symbols bound at link time) for given images."""
import subprocess, sys, os, re, collections, pickle
def run(*a): return subprocess.run(a, capture_output=True, text=True).stdout
def needed(p): return re.findall(r'\(NEEDED\)\s+Shared library: \[(.*?)\]', run('readelf', '-dW', p))
def hashstyle(p): return 'gnu' if 'GNU_HASH' in run('readelf', '-dW', p) else 'sysv'
def defined(p):
    out = {}
    for line in run('readelf', '-W', '--dyn-syms', p).splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(':') and f[6] != 'UND' and f[4] in ('GLOBAL', 'WEAK') and f[3] != 'SECTION':
            out[f[7].split('@')[0]] = f[3]
    return out
def wanted(p):
    s = set()
    for line in run('readelf', '-rW', p).splitlines():
        f = line.split()
        if len(f) >= 5 and f[2].startswith('R_') and 'RELATIV' not in f[2]: s.add(f[4].split('@')[0])
    return s
exe, system_dir, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
def find(n):
    for d in dirs:
        p = os.path.join(d, n)
        if os.path.exists(p): return os.path.realpath(p)
order, seen, q = [exe], {os.path.basename(exe)}, [exe]
while q:
    c = q.pop(0)
    for n in needed(c):
        if n in seen: continue
        seen.add(n); p = find(n)
        if p: order.append(p); q.append(p)
imgs = [dict(name=os.path.basename(p), path=p, hash=hashstyle(p), defs=defined(p), wants=wanted(p), system=p.startswith(system_dir)) for p in order]
def simulate(gnu_ours, symbolic_ours):
    sk = collections.Counter(); found = 0
    for img in imgs:
        for s in img['wants']:
            if symbolic_ours and not img['system'] and img['defs'].get(s) == 'FUNC': continue
            for o in imgs:
                if s in o['defs']: found += 1; break
                h = 'gnu' if (o['hash'] == 'gnu' or (gnu_ours and not o['system'])) else 'sysv'
                sk[h] += 1
    return sk['sysv'], sk['gnu'], found
for label, g, b in [('now', False, False), ('our libs GNU hash', True, False), ('our libs GNU hash + Bsymbolic-functions', True, True)]:
    s, g2, f = simulate(g, b)
    print(f'{label:42} sysv skips {s:8}  gnu skips {g2:8}  found {f:7}  cost {s + 0.2 * g2 + f:10.0f}')
