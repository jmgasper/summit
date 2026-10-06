#!/usr/bin/env python3
"""Regenerate Summit's toolbar HVIFs; the four Haiku originals are kept as-is."""
import importlib.util
import math
from pathlib import Path

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('hvif', root / 'tools/make-summit-icon.py')
hvif = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hvif)


def icon():
    value = hvif.Icon()
    ink = value.style(hvif.Solid(42, 65, 68))
    # The same light-to-dark green used by Haiku's navigation arrows.
    green = value.style(hvif.Gradient(0, [(0, (155, 239, 146)), (255, (15, 151, 57))],
                                    (0, .38, -.38, 0, 32, 32)))
    gold = value.style(hvif.Gradient(0, [(0, (255, 240, 143)), (255, (232, 159, 13))],
                                   (0, .38, -.38, 0, 32, 32)))
    return value, ink, green, gold


def save(name, value):
    (root / 'resources/toolbar' / (name + '.hvif')).write_bytes(value.to_bytes())


def star(radius, inner):
    return [(32 + (radius if i % 2 == 0 else inner) * math.cos(-math.pi / 2 + i * math.pi / 5),
             33 + (radius if i % 2 == 0 else inner) * math.sin(-math.pi / 2 + i * math.pi / 5)) for i in range(10)]


for name, filled in [('bookmark', False), ('bookmark-filled', True)]:
    value, ink, green, gold = icon()
    value.draw(ink, star(28, 13))
    face = gold if filled else value.style(hvif.Solid(245, 245, 229))
    value.draw(face, star(23, 10.5))
    save(name, value)

value, ink, green, gold = icon()
value.draw(ink, [(25, 7), (39, 7), (39, 25), (57, 25), (57, 39), (39, 39), (39, 57), (25, 57), (25, 39), (7, 39), (7, 25), (25, 25)])
value.draw(green, [(28, 10), (36, 10), (36, 28), (54, 28), (54, 36), (36, 36), (36, 54), (28, 54), (28, 36), (10, 36), (10, 28), (28, 28)])
save('plus', value)

value, ink, green, gold = icon()
value.draw(ink, [(25, 6), (39, 6), (39, 29), (51, 29), (32, 48), (13, 29), (25, 29)])
value.draw(green, [(28, 9), (36, 9), (36, 32), (44, 32), (32, 44), (20, 32), (28, 32)])
value.draw(ink, [(7, 43), (13, 43), (13, 53), (51, 53), (51, 43), (57, 43), (57, 59), (7, 59)])
save('downloads', value)

# Clockwise circular arrow, filled polygons rather than font glyphs.
value, ink, green, gold = icon()
def arc(radius, start, end):
    return [(32 + radius * math.cos(math.radians(a)), 32 + radius * math.sin(math.radians(a)))
            for a in range(start, end + (1 if end > start else -1), 5 if end > start else -5)]
value.draw(ink, arc(25, -50, 230) + arc(13, 230, -50))
value.draw(green, arc(22, -50, 225) + arc(16, 225, -50))
value.draw(ink, [(42, 9), (58, 7), (55, 30), (35, 16)])
value.draw(green, [(45, 12), (54, 11), (52, 24), (41, 16)])
save('reload', value)

value, ink, green, gold = icon()
for cx in (12, 32, 52):
    value.draw(ink, [(cx + 5 * math.cos(a * math.pi / 8), 32 + 5 * math.sin(a * math.pi / 8)) for a in range(16)])
save('more', value)
