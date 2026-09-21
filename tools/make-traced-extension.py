#!/usr/bin/env python3
"""Unpack an extension and inject the API gap tracer into a diagnostic copy.

The output folder is for compatibility investigation only. It is a modified
package, so results from it never count as published-extension verification.
"""
import argparse
import json
import os
import pathlib
import re
import shutil
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHIM = ROOT / 'tools/extension-gap-tracer/summit-gap-shim.js'
SHIM_NAME = 'summit-gap-shim.js'


def unpack(package, destination):
    data = package.read_bytes()
    # CRX3: "Cr24", version, header length, then a ZIP archive.
    if data[:4] == b'Cr24':
        offset = 12 + int.from_bytes(data[8:12], 'little')
        archive = destination.parent / (destination.name + '.zip')
        archive.write_bytes(data[offset:])
        package = archive
    with zipfile.ZipFile(package) as bundle:
        for member in bundle.infolist():
            target = (destination / member.filename).resolve()
            if not target.is_relative_to(destination.resolve()):
                raise SystemExit('Archive member escapes the destination: ' + member.filename)
        bundle.extractall(destination)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('package', type=pathlib.Path, help='XPI, ZIP, CRX or unpacked folder')
    parser.add_argument('destination', type=pathlib.Path)
    parser.add_argument('--content-scripts', action='store_true',
                        help='also announce every manifest content script as it runs in a page')
    args = parser.parse_args()
    if args.destination.exists():
        shutil.rmtree(args.destination)
    if args.package.is_dir():
        shutil.copytree(args.package, args.destination)
    else:
        args.destination.mkdir(parents=True)
        unpack(args.package, args.destination)
    # Signature manifests no longer describe the modified copy.
    shutil.rmtree(args.destination / 'META-INF', ignore_errors=True)
    shutil.rmtree(args.destination / '_metadata', ignore_errors=True)
    shim = SHIM.read_text()
    dump = os.environ.get('SUMMIT_GAP_DUMP_TEXT')
    if dump:
        shim = 'globalThis.__summitGapDumpText = ' + json.dumps(dump) + ';\n' + shim
    (args.destination / SHIM_NAME).write_text(shim)
    pages = 0
    for page in args.destination.rglob('*.html'):
        text = page.read_text(encoding='utf-8', errors='surrogateescape')
        tag = '<script src="/' + SHIM_NAME + '"></script>'
        updated, count = re.subn(r'(<head[^>]*>)', lambda match: match.group(1) + tag, text, count=1, flags=re.I)
        if not count:
            updated = tag + text
        page.write_text(updated, encoding='utf-8', errors='surrogateescape')
        pages += 1
    scripts = trace_content_scripts(args.destination) if args.content_scripts else 0
    print('Traced copy:', args.destination, 'pages:', pages, 'content scripts:', scripts)


def trace_content_scripts(destination):
    """Announce each content script in the page console, before and after it runs.

    A content script that never reports has not been injected; one that reports
    only its start threw while running.
    """
    manifest = json.loads((destination / 'manifest.json').read_text(encoding='utf-8'))
    traced = 0
    for index, entry in enumerate(manifest.get('content_scripts') or []):
        for path in entry.get('js') or []:
            script = destination / path
            if not script.is_file():
                continue
            label = json.dumps('[SUMMIT-CS] %d %s' % (index, path))
            text = script.read_text(encoding='utf-8', errors='surrogateescape')
            script.write_text(
                'console.warn(%s, "start", location.href, typeof chrome, typeof browser);\n' % label
                + text
                + '\nconsole.warn(%s, "end", location.href);\n' % label,
                encoding='utf-8', errors='surrogateescape')
            traced += 1
    return traced


if __name__ == '__main__':
    main()
