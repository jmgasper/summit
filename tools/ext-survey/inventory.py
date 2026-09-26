#!/usr/bin/env python3
"""Inventory manifest + chrome.*/browser.* API references for each unpacked extension."""
import json, os, re, sys, collections
ROOT = os.path.join(os.path.dirname(__file__), '..', 'unpacked')
API_RE = re.compile(r'\b(?:chrome|browser)\s*\.\s*([a-zA-Z]+)\s*\.\s*([a-zA-Z]+)(?:\s*\.\s*([a-zA-Z]+))?')
# namespaces we care about
NS = set('action alarms bookmarks browserAction browsingData commands contentSettings contextMenus contextualIdentities cookies debugger declarativeContent declarativeNetRequest devtools dns documentScan downloads enterprise extension find fontSettings gcm history i18n identity idle instanceID management menus notifications offscreen omnibox pageAction pageCapture permissions power printerProvider privacy proxy readingList runtime scripting search sessions sidePanel sidebarAction storage system tabCapture tabGroups tabs theme topSites tts ttsEngine userScripts webNavigation webRequest windows windowsId dom'.split())
def load_manifest(d):
    with open(os.path.join(d, 'manifest.json'), encoding='utf-8-sig') as f:
        txt = f.read()
    try:
        return json.loads(txt)
    except Exception:
        txt = re.sub(r'^\s*//.*$', '', txt, flags=re.M)
        return json.loads(txt)
def scan(d):
    refs = collections.Counter()
    for root, _, files in os.walk(d):
        if '_metadata' in root: continue
        for fn in files:
            if not fn.endswith(('.js', '.mjs', '.html')): continue
            try:
                s = open(os.path.join(root, fn), encoding='utf-8', errors='ignore').read()
            except Exception: continue
            for m in API_RE.finditer(s):
                ns, mem, sub = m.group(1), m.group(2), m.group(3)
                if ns not in NS: continue
                key = f'{ns}.{mem}'
                refs[key] += 1
    return refs
out = {}
for name in sorted(os.listdir(ROOT)):
    d = os.path.join(ROOT, name)
    try: m = load_manifest(d)
    except Exception as e:
        out[name] = {'error': str(e)}; continue
    bg = m.get('background', {})
    if 'service_worker' in bg: bgt = f"service_worker {bg['service_worker']} type={bg.get('type','classic')}"
    elif 'scripts' in bg or 'page' in bg:
        bgt = ('page ' + bg.get('page','')) if 'page' in bg else 'scripts[%d]' % len(bg['scripts'])
        bgt += ' persistent=%s' % bg.get('persistent', 'default')
    else: bgt = 'none'
    refs = scan(d)
    out[name] = {
        'name': m.get('name'), 'version': m.get('version'), 'mv': m.get('manifest_version'),
        'background': bgt, 'keys': sorted(m.keys()),
        'permissions': m.get('permissions', []), 'optional_permissions': m.get('optional_permissions', []),
        'host_permissions': m.get('host_permissions', []),
        'content_scripts': len(m.get('content_scripts', [])),
        'cs_worlds': sorted({cs.get('world','ISOLATED') for cs in m.get('content_scripts', [])}),
        'gecko': (m.get('browser_specific_settings') or m.get('applications') or {}).get('gecko', {}).get('id'),
        'dnr': m.get('declarative_net_request'),
        'api_refs': dict(sorted(refs.items())),
    }
json.dump(out, open(os.path.join(ROOT, '..', 'inventory.json'), 'w'), indent=1)
for n, v in out.items():
    if 'error' in v: print(n, 'ERROR', v['error']); continue
    print(f"== {n}: {v['name']} {v['version']} MV{v['mv']} bg={v['background']} gecko={v['gecko']}")
    print('   keys:', ' '.join(k for k in v['keys'] if k not in ('name','version','manifest_version','description','icons','author','homepage_url','default_locale','update_url','key','short_name')))
    print('   perms:', v['permissions'], 'opt:', v['optional_permissions'], 'hosts:', v['host_permissions'][:4])
    print('   cs:', v['content_scripts'], v['cs_worlds'])
    nss = collections.Counter(k.split('.')[0] for k in v['api_refs'])
    print('   ns:', ' '.join(sorted(nss)))
