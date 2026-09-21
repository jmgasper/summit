#!/usr/bin/env python3
"""Derive the architecture-independent Mesa patch used by the Summit VM build.

The KunanyiOS Mesa 25.3.6 port keeps one complete patch
(tools/rock5-itx/mesa/mesa-haiku-native.patch). It mixes the Haiku EGL/HGL
corrections with the ARM64 Panfrost/Mali CSF backend. The x86_64 QEMU guest
only needs the software rasterizers, so this script keeps every file section
outside the Panfrost trees and writes the result next to this script.

Usage: make-patch.py [REFERENCE_PATCH]
"""
import hashlib
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT = pathlib.Path(
    '/mnt/HaikuWork/src/haiku/tools/rock5-itx/mesa/mesa-haiku-native.patch')
# SHA-256 of the reference patch this subset was derived from.
REFERENCE_SHA256 = '19ca86d5aa36039da00ea8a2c0e590fe5d7b31a8f18f046f59d1d31b1ffc6187'
SKIP_PREFIXES = (
    'src/gallium/drivers/panfrost/',
    'src/gallium/winsys/panfrost/',
    'src/panfrost/',
)
OUTPUT = HERE / 'mesa-25.3.6-haiku-x86_64.patch'


def main():
    source = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    data = source.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != REFERENCE_SHA256:
        print('warning: reference patch hash differs from the pinned one:\n  '
              + digest, file=sys.stderr)
    sections = re.split(r'(?m)^(?=diff --git )', data.decode())
    kept, skipped = [], []
    for section in sections:
        match = re.match(r'diff --git a/(\S+) ', section)
        if not match:
            continue
        name = match.group(1)
        if name.startswith(SKIP_PREFIXES):
            skipped.append(name)
        else:
            kept.append(name)
            OUTPUT_PARTS.append(section)
    OUTPUT.write_text(''.join(OUTPUT_PARTS))
    print('reference sha256:', digest)
    print('kept:')
    for name in kept:
        print('  ' + name)
    print('skipped %d Panfrost/Mali sections' % len(skipped))
    print('wrote', OUTPUT, hashlib.sha256(OUTPUT.read_bytes()).hexdigest())


OUTPUT_PARTS = []
if __name__ == '__main__':
    main()
