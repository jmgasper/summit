#!/usr/bin/env python3
"""Render the supplied SVG toolbar artwork (requires CairoSVG).

The SVGs stay editable in resources/artwork. Checked-in PNGs let Haiku's
Translation Kit load their gradients, clipping and transparent shadows without
requiring an SVG translator. Forward is the requested 180-degree rotation.
"""
from pathlib import Path
import xml.etree.ElementTree as ET
import cairosvg

root = Path(__file__).resolve().parents[1]
ET.register_namespace('', 'http://www.w3.org/2000/svg')
for name in ('back', 'forward', 'reload', 'home', 'downloads', 'bookmark', 'lock'):
    source = root / 'resources/artwork' / (('back' if name == 'forward' else name) + '.svg')
    svg = ET.parse(source).getroot()
    if name == 'forward':
        group = ET.Element('{http://www.w3.org/2000/svg}g', {'transform': 'rotate(180 32 32)'})
        for child in list(svg):
            if child.tag.split('}')[-1] not in ('defs', 'title', 'desc'):
                svg.remove(child)
                group.append(child)
        svg.append(group)
    for size in (24, 48, 96):
        output = root / 'resources/toolbar' / f'{name}-{size}.png'
        cairosvg.svg2png(bytestring=ET.tostring(svg), write_to=str(output),
                        output_width=size, output_height=size)
        print(output.relative_to(root))
