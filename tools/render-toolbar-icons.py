#!/usr/bin/env python3
"""Preview the toolbar silhouettes (fonttools, CairoSVG and Pillow required).

The native HVIF rendering must also be checked in Summit on Haiku.
"""
import argparse
import importlib.util
import io
from pathlib import Path
import xml.etree.ElementTree as ET

import cairosvg
from PIL import Image, ImageDraw
from fontTools.pens.boundsPen import BoundsPen
from fontTools.svgLib.path import parse_path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('toolbar', ROOT / 'tools/make-toolbar-icons.py')
toolbar = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolbar)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    sheet = Image.new('RGB', (1020, 370), '#ffffff')
    draw = ImageDraw.Draw(sheet)
    rows = [('#eeeeee', '#333333', '#976000', '#1e6f55', 'Light'),
            ('#402a68', '#f3eefc', '#ffd070', '#83debf', 'Private'),
            ('#292d32', '#eeeeee', '#ffd070', '#83debf', 'Dark'),
            ('#eeeeee', '#aaaaaa', '#aaaaaa', '#aaaaaa', 'Disabled')]
    for row, (background, ink, amber, green, title) in enumerate(rows):
        y = 35 + row * 80
        draw.rectangle((0, y, 1020, y + 79), fill=background)
        draw.text((12, y + 32), title, fill=ink)
        for index, (name, source, extent) in enumerate(toolbar.ICONS):
            svg = ET.parse(ROOT / 'resources/artwork/fontawesome' / f'{source}.svg')
            paths = [p.attrib['d'] for p in svg.iter('{http://www.w3.org/2000/svg}path')]
            bounds = BoundsPen(None)
            for path in paths:
                parse_path(path, bounds)
            left, top, right, bottom = bounds.bounds
            scale = extent / max(right - left, bottom - top)
            x_offset = 10 - (left + right) * scale / 2
            y_offset = 10 - (top + bottom) * scale / 2
            color = amber if name in ('bookmark-filled', 'lock-warning') else green if name == 'reader-active' else ink
            xml = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 20 20">'
            xml += f'<g fill="{color}" transform="translate({x_offset} {y_offset}) scale({scale})">'
            xml += ''.join(f'<path d="{path}"/>' for path in paths) + '</g></svg>'
            x = 110 + index * 60
            for size, dy in ((20, 5), (40, 32)):
                bitmap = Image.open(io.BytesIO(cairosvg.svg2png(bytestring=xml.encode(), output_width=size, output_height=size)))
                sheet.paste(bitmap, (x + (40 - size) // 2, y + dy), bitmap)
            if row == 0:
                label = name.replace('bookmark', 'star').replace('-active', ' on').replace('-warning', ' alert').replace('-filled', ' saved')
                draw.text((x - 2, 12), label, fill='#333333')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.output)
    print(args.output)


if __name__ == '__main__':
    main()
