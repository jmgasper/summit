#!/usr/bin/env python3
"""List the WebExtension JS surface built on Haiku from the IDL (Platform=COCOA excluded)."""
import re, os, json, sys
IDL = '/mnt/HaikuWork/apps/summit/.cache/WebKit/Source/WebKit/WebProcess/Extensions/Interfaces'
def parse(path):
    s = open(path).read()
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    m = re.search(r'interface\s+(\w+)\s*\{(.*)\}', s, re.S)
    if not m: return None, {}
    name, body = m.group(1), m.group(2)
    members = {}
    for stmt in body.split(';'):
        stmt = ' '.join(stmt.split())
        if not stmt: continue
        attrs = ''
        if stmt.startswith('['):
            depth = 0
            for i, ch in enumerate(stmt):
                if ch == '[': depth += 1
                elif ch == ']':
                    depth -= 1
                    if depth == 0: attrs, rest = stmt[1:i], stmt[i+1:].strip(); break
        else: rest = stmt
        plat = re.search(r'Platform=(\w+)', attrs)
        plat = plat.group(1) if plat else 'ALL'
        cond = re.search(r'Conditional=([\w&| ]+)', attrs)
        mm = re.match(r'(?:readonly\s+)?attribute\s+(\w+)\s+(\w+)', rest)
        if mm: members[mm.group(2)] = {'kind': 'attr', 'type': mm.group(1), 'platform': plat, 'cond': cond.group(1) if cond else None}; continue
        mm = re.match(r'(?:static\s+)?[\w<>?]+\s+(\w+)\s*\(', rest)
        if mm: members[mm.group(1)] = {'kind': 'fn', 'platform': plat, 'cond': cond.group(1) if cond else None}
    return name, members
ifaces = {}
for fn in sorted(os.listdir(IDL)):
    if fn.endswith('.idl'):
        n, m = parse(os.path.join(IDL, fn))
        if n: ifaces[n] = m
ns = ifaces['WebExtensionAPINamespace']
surface = {}
for nsname, info in ns.items():
    if info['platform'] == 'COCOA': continue
    iface = ifaces.get(info['type'], {})
    surface[nsname] = sorted(k for k, v in iface.items() if v['platform'] != 'COCOA')
# nested (storage.local etc) fine as members
json.dump({'surface': surface, 'cocoa_only': {i: sorted(k for k, v in m.items() if v['platform']=='COCOA') for i, m in ifaces.items() if any(v['platform']=='COCOA' for v in m.values())}}, open('/mnt/HaikuWork/apps/summit/.cache/ext-survey/haiku-surface.json','w'), indent=1)
for k, v in sorted(surface.items()): print(k, ':', ' '.join(v))
print('\nCOCOA-only members:')
for i, m in ifaces.items():
    c = [k for k, v in m.items() if v['platform']=='COCOA']
    if c: print(' ', i, c)
