#!/usr/bin/env python3
"""Grab the workstation's real frame buffer (not a screenshot) as a PNG.

Haiku's screenshot and the VNC server read the screen through BScreen, which
averages a desktop drawn at 200% back down to its logical size, so neither can
show whether text is sharp. `nvscanout --dump` copies the frame buffer the card
scans out. The crop is given in frame buffer pixels and done on the workstation,
so only the crop crosses the network.

  python3 tools/bench/fbgrab.py out.png [--crop X Y W H]
"""
import argparse
import pathlib
import subprocess

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[2]
CROP = r'''
import sys
width, x, y, w, h = map(int, sys.argv[1:6])
with open("/boot/home/summit/bench/fb.raw", "rb") as f:
    out = sys.stdout.buffer
    for row in range(y, y + h):
        f.seek((row * width + x) * 4)
        out.write(f.read(w * 4))
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output')
    parser.add_argument('--crop', type=int, nargs=4, metavar=('X', 'Y', 'W', 'H'))
    args = parser.parse_args()
    info = subprocess.run(['bash', str(ROOT / 'tools/ws.sh'),
                           'mkdir -p /boot/home/summit/bench && /boot/home/tests/nvscanout --dump /boot/home/summit/bench/fb.raw'],
                          stdout=subprocess.PIPE, check=True, text=True).stdout
    dims = [line for line in info.splitlines() if line.startswith('dumped ')][0].split()[1]
    width, height = map(int, dims.split('x'))
    x, y, w, h = args.crop or (0, 0, width, height)
    raw = subprocess.run(['bash', str(ROOT / 'tools/ws.sh'),
                          f"python3 -c '{CROP}' {width} {x} {y} {w} {h}"],
                         stdout=subprocess.PIPE, check=True).stdout
    Image.frombytes('RGB', (w, h), raw, 'raw', 'BGRX').save(args.output)
    print(f'{args.output}: {w}x{h} from a {width}x{height} frame buffer')


if __name__ == '__main__':
    main()
