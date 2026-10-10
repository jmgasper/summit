#!/usr/bin/env python3
"""Generate themeable native toolbar vectors from pinned Font Awesome SVGs.

Requires fonttools (only on the artwork-authoring host). No font or SVG
translator is needed by Summit. Geometry fits a 20-point canvas, including
optical padding; Chrome.cpp must preserve that canvas when drawing.
"""
import importlib.util
from pathlib import Path
import xml.etree.ElementTree as ET

from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.transformPen import TransformPen
from fontTools.svgLib.path import parse_path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hvif', ROOT / 'tools/make-summit-icon.py')
hvif = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hvif)

# Name, Font Awesome source, longest visible dimension in logical pixels.
# Paired states share geometry and sizing; a filled star denotes a saved page.
ICONS = (
    ('back', 'solid-arrow-left', 16),
    ('forward', 'solid-arrow-right', 16),
    ('reload', 'solid-arrow-rotate-right', 16),
    ('stop', 'solid-xmark', 13),
    ('plus', 'solid-plus', 14),
    ('bookmark', 'regular-star', 18),
    ('bookmark-filled', 'solid-star', 18),
    ('downloads', 'solid-download', 16),
    ('home', 'solid-house', 17),
    ('more', 'solid-ellipsis', 14),
    ('go', 'solid-arrow-right', 16),
    ('lock', 'solid-lock', 14),
    ('lock-warning', 'solid-triangle-exclamation', 16),
    ('reader', 'solid-book-open', 17),
    ('reader-active', 'solid-book-open', 17),
)


class VectorPen(BasePen):
    """Preserve cubic contours and counters in one HVIF path-source shape."""
    def __init__(self):
        super().__init__(None)
        self.paths = []

    def _moveTo(self, point):
        self.points = [list(point) * 3]

    def _lineTo(self, point):
        self.points.append(list(point) * 3)

    def _curveToOne(self, first, second, end):
        self.points[-1][4:6] = first
        self.points.append([*end, *second, *end])

    def _closePath(self):
        if self.points[-1][:2] == self.points[0][:2] and len(self.points) > 1:
            self.points[0][2:4] = self.points.pop()[2:4]
        self.paths.append(hvif.Path(self.points))

    def _endPath(self):
        raise ValueError('Toolbar silhouettes must be closed')


def main():
    for name, source, extent in ICONS:
        svg = ET.parse(ROOT / 'resources/artwork/fontawesome' / f'{source}.svg')
        paths = [p.attrib['d'] for p in svg.iter('{http://www.w3.org/2000/svg}path')]
        bounds = BoundsPen(None)
        for path in paths:
            parse_path(path, bounds)
        left, top, right, bottom = bounds.bounds
        scale = extent / max(right - left, bottom - top) * 64 / 20
        transform = (scale, 0, 0, scale,
                     32 - (left + right) * scale / 2,
                     32 - (top + bottom) * scale / 2)
        pen = VectorPen()
        for path in paths:
            parse_path(path, TransformPen(pen, transform))
        icon = hvif.Icon()
        ink = icon.style(hvif.Solid(0, 0, 0))
        icon.shape(ink, [icon.path(p) for p in pen.paths])
        output = ROOT / 'resources/toolbar' / f'{name}.hvif'
        output.write_bytes(icon.to_bytes())
        print(f'{output.relative_to(ROOT)}: {output.stat().st_size} bytes')


if __name__ == '__main__':
    main()
