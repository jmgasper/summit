#!/usr/bin/env python3
"""Simulate runtime_loader global-scope symbol lookups for an executable.

scope.py EXE LIBDIR... : BFS load order from EXE's NEEDED (searching LIBDIRs),
then for every image, every unique symbol named by a non-relative dynamic
relocation is looked up through the scope in order; prints per image the
lookups and the images skipped (by hash style) before the definition."""
import subprocess, sys, os, re, collections

def run(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout

def needed(path):
    return re.findall(r'\(NEEDED\)\s+Shared library: \[(.*?)\]', run('readelf', '-dW', path))

def hashstyle(path):
    d = run('readelf', '-dW', path)
    return 'gnu' if 'GNU_HASH' in d else 'sysv'

def defined(path):
    out = set()
    for line in run('readelf', '-W', '--dyn-syms', path).splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(':') and f[6] != 'UND' and f[4] in ('GLOBAL', 'WEAK') and f[3] != 'SECTION':
            out.add(f[7].split('@')[0])
    return out

def wanted(path):
    syms = set()
    for line in run('readelf', '-rW', path).splitlines():
        f = line.split()
        if len(f) >= 5 and f[2].startswith('R_') and 'RELATIV' not in f[2]:
            syms.add(f[4].split('@')[0])
    return syms

exe, dirs = sys.argv[1], sys.argv[2:]
def find(name):
    for d in dirs:
        p = os.path.join(d, name)
        if os.path.exists(p): return os.path.realpath(p)
    return None

order, seen, queue = [exe], {os.path.basename(exe)}, [exe]
while queue:
    cur = queue.pop(0)
    for n in needed(cur):
        if n in seen: continue
        seen.add(n)
        p = find(n)
        if not p: print('missing', n); continue
        order.append(p); queue.append(p)
info = [(os.path.basename(p), hashstyle(p), defined(p)) for p in order]
print('scope:', ' '.join(f'{n}({h})' for n, h, _ in info))
total = collections.Counter()
for p in order:
    w = wanted(p)
    skipped = collections.Counter(); found_in = collections.Counter(); unresolved = 0
    for s in w:
        for n, h, d in info:
            if s in d:
                found_in[n] += 1; break
            skipped[h] += 1
        else:
            unresolved += 1
    total.update(skipped)
    print(f'{os.path.basename(p):28} lookups {len(w):6}  skipped gnu {skipped["gnu"]:7} sysv {skipped["sysv"]:7}  unresolved {unresolved}  found: ' +
          ', '.join(f'{k} {v}' for k, v in found_in.most_common(5)))
print('total skipped', dict(total))
