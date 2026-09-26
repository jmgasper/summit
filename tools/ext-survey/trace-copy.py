#!/usr/bin/env python3
"""Make a traced diagnostic copy (tools/make-traced-extension.py) and also load the shim
first in an MV3 service worker (classic: importScripts, module: import)."""
import json, pathlib, subprocess, sys
ROOT = pathlib.Path('/mnt/HaikuWork/apps/summit')
pkg, dest = sys.argv[1], pathlib.Path(sys.argv[2])
subprocess.run([sys.executable, str(ROOT / 'tools/make-traced-extension.py'), pkg, str(dest), '--content-scripts'], check=True)
m = json.loads((dest / 'manifest.json').read_text(encoding='utf-8-sig'))
bg = m.get('background', {})
sw = bg.get('service_worker')
if sw:
    p = dest / sw.lstrip('./').lstrip('/') if not sw.startswith('./') else dest / sw[2:]
    text = p.read_text(encoding='utf-8', errors='surrogateescape')
    if bg.get('type') == 'module':
        text = 'import "/summit-gap-shim.js";\n' + text
    else:
        text = 'importScripts("/summit-gap-shim.js");\n' + text
    p.write_text(text, encoding='utf-8', errors='surrogateescape')
    print('service worker traced:', p)
