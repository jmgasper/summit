#!/usr/bin/env python3
"""Look at a capture made by fbcap on the workstation (bench/cap-NAME.raw).

  frames.py NAME                        counts: frames with a band of flat black rows (a picture's box
                                        without the picture), frames that are one flat colour (page not
                                        painted), and bands of flat rows that come and go within 3 frames
  frames.py NAME sheet FIRST COUNT [COLUMNS [STRIDE]]
                                        contact sheet of frames FIRST, FIRST+STRIDE, ... as NAME-FIRST.png

The capture is fetched once into the current directory. fbcap reads a frame from top to bottom in
about 20 ms, so a frame that was on screen for 16 ms shows as a band, not as a whole picture.
"""
import os, pathlib, struct, subprocess, sys
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[3]

def load(name):
    local = f'cap-{name}.raw'
    if not os.path.exists(local):
        with open(local, 'wb') as f:
            subprocess.run(['bash', str(ROOT / 'tools/ws.sh'), f'cat /boot/home/summit/bench/cap-{name}.raw'], stdout=f, check=True)
    data = open(local, 'rb').read()
    nl = data.index(b'\n')
    _, w, h, count = data[:nl].split()
    return data, nl + 1, int(w), int(h), int(count)

def main():
    name = sys.argv[1]
    data, off, w, h, count = load(name)
    size = 8 + w * h * 4
    if len(sys.argv) > 2 and sys.argv[2] == 'sheet':
        first, n = int(sys.argv[3]), int(sys.argv[4])
        cols = int(sys.argv[5]) if len(sys.argv) > 5 else 10
        stride = int(sys.argv[6]) if len(sys.argv) > 6 else 1
        sheet = Image.new('RGB', (cols * (w + 4), ((n + cols - 1) // cols) * (h + 4)), (255, 0, 255))
        for k in range(n):
            i = first + k * stride
            if i >= count:
                break
            frame = Image.frombytes('RGB', (w, h), data[off + i * size + 8: off + (i + 1) * size], 'raw', 'BGRX')
            sheet.paste(frame, ((k % cols) * (w + 4), (k // cols) * (h + 4)))
        out = f'{name}-{first}.png'
        sheet.save(out)
        print(out, sheet.size)
        return
    flat = []; times = []
    for i in range(count):
        base = off + i * size
        times.append(struct.unpack_from('<q', data, base)[0])
        rows = []
        for y in range(h):
            row = data[base + 8 + y * w * 4: base + 8 + (y + 1) * w * 4]
            rows.append(row[:3] if row == row[:4] * w else None)
        flat.append(rows)
    black = [i for i, rows in enumerate(flat) if sum(1 for r in rows if r is not None and max(r) < 8) >= h * 0.1]
    unpainted = [i for i, rows in enumerate(flat) if sum(1 for r in rows if r is not None) >= h * 0.9]
    flashes = []
    for i in range(1, count - 2):
        cur = flat[i]; best = (0, 0, None); start = 0
        for y in range(1, h + 1):
            if y == h or cur[y] is None or cur[y] != cur[start] or cur[start] is None:
                if cur[start] is not None and y - start > best[0]:
                    best = (y - start, start, cur[start])
                start = y
        n, y0, colour = best
        if n < h * 0.12:
            continue
        same = lambda j: sum(1 for y in range(y0, y0 + n) if flat[j][y] == colour)
        if same(i - 1) < n * 0.5 and (same(i + 1) < n * 0.5 or same(i + 2) < n * 0.5):
            flashes.append((i, y0, n, '%02x%02x%02x' % (colour[2], colour[1], colour[0])))
    print(f'{name}: {count} frames of {w}x{h} in {(times[-1] - times[0]) / 1e6:.1f} s; black-box frames {len(black)}, '
          f'unpainted frames {len(unpainted)}, bands that come and go {flashes[:20]}')

if __name__ == '__main__':
    main()
