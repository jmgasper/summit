#!/usr/bin/env python3
"""Generate resources/Summit.hvif, the Haiku vector icon for the app.

The artwork is a vector tracing of docs/summit-icon.png, a snow-capped peak with
a summit flag in front of a globe.  Geometry below stays in the coordinate space
of that drawing and is mapped into Haiku's 64x64 icon grid by `P()`, so the
tracing remains readable next to the original image.

Run `python3 tools/make-summit-icon.py` after editing; the result is checked in
and `resources/Summit.rdef` imports it as the BEOS:ICON resource.
"""
import math
import os
import sys

# ---------------------------------------------------------------- HVIF output

MAGIC = b'ncif'

STYLE_SOLID_COLOR = 1
STYLE_GRADIENT = 2
STYLE_SOLID_COLOR_NO_ALPHA = 3
STYLE_SOLID_GRAY = 4
STYLE_SOLID_GRAY_NO_ALPHA = 5

GRADIENT_FLAG_TRANSFORM = 1 << 1
GRADIENT_FLAG_NO_ALPHA = 1 << 2

PATH_FLAG_CLOSED = 1 << 1
PATH_FLAG_NO_CURVES = 1 << 3

SHAPE_TYPE_PATH_SOURCE = 10

GRADIENT_LINEAR = 0
GRADIENT_CIRCULAR = 1


def write_coord(out, value):
    rounded = math.floor(value + 0.5)
    if rounded == value and -32 <= rounded <= 95:
        out.append(int(rounded) + 32)
        return
    packed = int((value + 128.0) * 102.0) & 0x7FFF
    packed |= 0x8000
    out.append(packed >> 8)
    out.append(packed & 0xFF)


def write_float24(out, value):
    """1 sign bit, 6 exponent bits (bias 32), 17 mantissa bits, implicit one."""
    if value == 0.0:
        out += b'\0\0\0'
        return
    sign = 1 if value < 0 else 0
    value = abs(value)
    exponent = int(math.floor(math.log(value, 2)))
    mantissa = int(round((value / 2.0 ** exponent - 1.0) * (1 << 17)))
    if mantissa >= (1 << 17):          # rounding carried into the exponent
        mantissa = 0
        exponent += 1
    if not -32 <= exponent < 32:
        raise ValueError('float24 out of range: %r' % value)
    packed = (sign << 23) | ((exponent + 32) << 17) | mantissa
    out.append((packed >> 16) & 0xFF)
    out.append((packed >> 8) & 0xFF)
    out.append(packed & 0xFF)


class Solid:
    def __init__(self, r, g, b, a=255):
        self.color = (r, g, b, a)

    def write(self, out):
        r, g, b, a = self.color
        if r == g == b:
            if a == 255:
                out.append(STYLE_SOLID_GRAY_NO_ALPHA)
                out.append(r)
            else:
                out.append(STYLE_SOLID_GRAY)
                out += bytes((r, a))
        elif a == 255:
            out.append(STYLE_SOLID_COLOR_NO_ALPHA)
            out += bytes((r, g, b))
        else:
            out.append(STYLE_SOLID_COLOR)
            out += bytes((r, g, b, a))


class Gradient:
    """`stops` are (offset 0..255, (r, g, b[, a])) in gradient space 0..64."""

    def __init__(self, kind, stops, transform):
        self.kind = kind
        self.stops = [(off, tuple(c) if len(c) == 4 else tuple(c) + (255,))
                      for off, c in stops]
        self.transform = transform

    def write(self, out):
        opaque = all(stop[1][3] == 255 for stop in self.stops)
        flags = GRADIENT_FLAG_TRANSFORM | (GRADIENT_FLAG_NO_ALPHA if opaque else 0)
        out.append(STYLE_GRADIENT)
        out += bytes((self.kind, flags, len(self.stops)))
        for value in self.transform:
            write_float24(out, value)
        for offset, color in self.stops:
            out.append(offset)
            out += bytes(color[:3] if opaque else color)


class Path:
    """Points are (x, y), or (x, y, inX, inY, outX, outY) for curves."""

    def __init__(self, points, closed=True):
        self.points = points
        self.closed = closed

    def write(self, out):
        straight = all(len(p) == 2 for p in self.points)
        flags = (PATH_FLAG_CLOSED if self.closed else 0) | \
                (PATH_FLAG_NO_CURVES if straight else 0)
        out += bytes((flags, len(self.points)))
        for point in self.points:
            if straight:
                write_coord(out, point[0])
                write_coord(out, point[1])
                continue
            if len(point) == 2:
                point = point * 3
            for value in point:
                write_coord(out, value)


class Shape:
    def __init__(self, style, paths):
        self.style = style
        self.paths = paths if isinstance(paths, (list, tuple)) else [paths]

    def write(self, out):
        out += bytes((SHAPE_TYPE_PATH_SOURCE, self.style, len(self.paths)))
        out += bytes(self.paths)
        out.append(0)


class Icon:
    def __init__(self):
        self.styles = []
        self.paths = []
        self.shapes = []

    def style(self, style):
        self.styles.append(style)
        return len(self.styles) - 1

    def path(self, path):
        self.paths.append(path)
        return len(self.paths) - 1

    def shape(self, style, paths):
        self.shapes.append(Shape(style, paths))

    def draw(self, style, points, closed=True):
        self.shape(style, [self.path(Path(points, closed))])

    def to_bytes(self):
        out = bytearray(MAGIC)
        for group in (self.styles, self.paths, self.shapes):
            if len(group) > 255:
                raise ValueError('too many entries')
            out.append(len(group))
            for item in group:
                item.write(out)
        return bytes(out)


# ------------------------------------------------------------ source geometry

# The tracing below uses pixel coordinates of the source drawing, whose opaque
# artwork spans x 269..1074 and y 169..923.  SCALE/ORIGIN centre that box in the
# 64x64 icon grid with a small margin.
SCALE = 0.072
ORIGIN_X, ORIGIN_Y = 3.0, 4.8


def P(x, y):
    return (round(ORIGIN_X + (x - 269) * SCALE, 3),
            round(ORIGIN_Y + (y - 169) * SCALE, 3))


def poly(points):
    return [P(x, y) for x, y in points]


KAPPA = 0.5522847498


def ellipse(cx, cy, rx, ry):
    """Four-point closed bezier ellipse, clockwise in screen coordinates."""
    ox, oy = rx * KAPPA, ry * KAPPA
    return [
        P(cx + rx, cy) + P(cx + rx, cy - oy) + P(cx + rx, cy + oy),
        P(cx, cy + ry) + P(cx + ox, cy + ry) + P(cx - ox, cy + ry),
        P(cx - rx, cy) + P(cx - rx, cy + oy) + P(cx - rx, cy - oy),
        P(cx, cy - ry) + P(cx - ox, cy - ry) + P(cx + ox, cy - ry),
    ]


def parallel(x0, x1, y, sag, thickness):
    """A latitude line: two shallow arcs between the same end points."""
    span = x1 - x0
    top = 4.0 * (sag - thickness / 2.0) / 3.0
    bottom = 4.0 * (sag + thickness / 2.0) / 3.0
    return [
        P(x0, y) + P(x0 + span / 3.0, y + bottom) + P(x0 + span / 3.0, y + top),
        P(x1, y) + P(x0 + 2 * span / 3.0, y + top) + P(x0 + 2 * span / 3.0, y + bottom),
    ]


def meridian(cx, cy, radius, bulge, thickness):
    """A longitude line: two half ellipses from pole to pole."""
    outer = bulge + math.copysign(thickness / 2.0, bulge)
    inner = bulge - math.copysign(thickness / 2.0, bulge)
    ky = radius * KAPPA
    return [
        P(cx, cy - radius) + P(cx + inner * KAPPA, cy - radius) + P(cx + outer * KAPPA, cy - radius),
        P(cx + outer, cy) + P(cx + outer, cy - ky) + P(cx + outer, cy + ky),
        P(cx, cy + radius) + P(cx + outer * KAPPA, cy + radius) + P(cx + inner * KAPPA, cy + radius),
        P(cx + inner, cy) + P(cx + inner, cy + ky) + P(cx + inner, cy - ky),
    ]


def linear_transform(x0, y0, x1, y1):
    """Maps a gradient onto the icon-space segment between two source points.

    Haiku evaluates a linear gradient over -64..+64 in gradient space, so the
    segment is centred on the transform's translation and stop offsets are just
    the fraction travelled along it.
    """
    ax, ay = P(x0, y0)
    bx, by = P(x1, y1)
    dx, dy = (bx - ax) / 128.0, (by - ay) / 128.0
    return (dx, dy, -dy, dx, (ax + bx) / 2.0, (ay + by) / 2.0)


GLOBE_CX, GLOBE_CY, GLOBE_R = 825.0, 569.0, 238.0

MOUNTAIN_OUTLINE = [
    (617, 296), (762, 520), (806, 550), (935, 802), (700, 930),
    (269, 796), (333, 700), (397, 640), (477, 490),
]

MOUNTAIN_BODY = [
    (617, 312), (751, 520), (795, 550), (921, 798), (698, 913),
    (288, 789), (345, 702), (408, 642), (488, 492),
]

SNOW = [
    (596, 336), (554, 400), (488, 492), (440, 583), (547, 508), (577, 580),
    (586, 605), (615, 574), (651, 620), (663, 638), (669, 604), (665, 580),
    (648, 500), (626, 400),
]

NEAR_RIDGE = [
    (547, 508), (577, 580), (586, 605), (615, 574), (651, 620), (663, 638),
    (685, 665), (696, 710), (711, 770), (725, 830), (739, 890), (742, 898),
    (698, 913), (288, 789), (345, 702), (408, 642), (440, 583),
]

NEAR_RIDGE_LIT = [
    (547, 508), (513, 560), (481, 605), (436, 665), (372, 725), (309, 785),
    (297, 791), (288, 789), (345, 702), (408, 642), (441, 583),
]

FRONT_EDGE = [
    (663, 638), (685, 665), (692, 700), (704, 745), (716, 790), (728, 840),
    (742, 896), (698, 913), (688, 875), (668, 785), (656, 740), (646, 700),
    (638, 668),
]

FAR_RIDGE_SHADE = [
    (702, 522), (734, 545), (777, 575), (818, 605), (840, 635), (854, 662),
    (877, 700), (900, 745), (924, 800), (745, 894), (739, 890), (685, 665),
]

FLAG_OUTLINE = [(603, 176), (802, 243), (603, 316)]
FLAG_BODY = [(617, 199), (773, 243), (617, 293)]

POLE_OUTLINE = [
    P(592, 350), P(592, 193) + P(592, 193) + P(592, 193 - 21 * KAPPA),
    P(613, 172) + P(613 - 21 * KAPPA, 172) + P(613 + 21 * KAPPA, 172),
    P(634, 193) + P(634, 193 - 21 * KAPPA) + P(634, 193),
    P(634, 350),
]
POLE_BODY = [
    P(602, 345), P(602, 203) + P(602, 203) + P(602, 203 - 11 * KAPPA),
    P(613, 192) + P(613 - 11 * KAPPA, 192) + P(613 + 11 * KAPPA, 192),
    P(624, 203) + P(624, 203 - 11 * KAPPA) + P(624, 203),
    P(624, 345),
]

BLACK = (0, 0, 0)


def build():
    icon = Icon()

    ink = icon.style(Solid(*BLACK))
    globe_fill = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (185, 228, 253)), (38, (142, 210, 252)), (77, (95, 175, 248)),
        (115, (29, 111, 204)), (153, (12, 92, 186)), (191, (3, 71, 158)),
        (255, (0, 44, 108))],
        linear_transform(657, 401, 993, 737)))
    graticule = icon.style(Solid(255, 255, 255, 110))
    snow = icon.style(Solid(253, 252, 246))
    shade = icon.style(Solid(200, 212, 235))
    rock = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (123, 161, 187)), (60, (124, 163, 189)), (121, (117, 157, 185)),
        (181, (106, 147, 177)), (235, (91, 137, 169)), (255, (84, 129, 162))],
        linear_transform(540, 520, 600, 900)))
    rock_lit = icon.style(Solid(166, 191, 206))
    rock_edge = icon.style(Solid(48, 87, 110))
    rock_dark = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (29, 63, 84)), (120, (31, 66, 88)), (200, (38, 72, 94)),
        (255, (41, 76, 99))],
        linear_transform(700, 760, 890, 760)))
    flag = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (253, 208, 66)), (60, (249, 181, 26)), (180, (242, 170, 14)),
        (255, (228, 155, 8))],
        linear_transform(690, 200, 690, 292)))
    pole = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (252, 192, 48)), (120, (244, 172, 20)), (150, (158, 92, 1)),
        (255, (143, 83, 0))],
        linear_transform(602, 250, 624, 250)))

    # Globe, behind the mountain.
    icon.draw(ink, ellipse(GLOBE_CX, GLOBE_CY, GLOBE_R + 11, GLOBE_R + 11))
    icon.draw(globe_fill, ellipse(GLOBE_CX, GLOBE_CY, GLOBE_R, GLOBE_R))
    for offset in (-137.0, 1.0, 131.0):
        half = math.sqrt(GLOBE_R ** 2 - offset ** 2)
        icon.draw(graticule, parallel(GLOBE_CX - half, GLOBE_CX + half,
                                      GLOBE_CY + offset, 24.0, 12.0))
    for bulge in (-62.0, 62.0, 168.0):
        icon.draw(graticule, meridian(GLOBE_CX, GLOBE_CY, GLOBE_R, bulge, 12.0))

    # Mountain: outline, then the faces painted inside it.
    icon.draw(ink, poly(MOUNTAIN_OUTLINE))
    icon.draw(shade, poly(MOUNTAIN_BODY))
    icon.draw(snow, poly(SNOW))
    icon.draw(rock, poly(NEAR_RIDGE))
    icon.draw(rock_lit, poly(NEAR_RIDGE_LIT))
    icon.draw(rock_edge, poly(FRONT_EDGE))
    icon.draw(rock_dark, poly(FAR_RIDGE_SHADE))

    # Summit flag; the pole is drawn over the flag's hoist edge.
    icon.draw(ink, poly(FLAG_OUTLINE))
    icon.draw(flag, poly(FLAG_BODY))
    icon.draw(ink, POLE_OUTLINE)
    icon.draw(pole, POLE_BODY)

    return icon.to_bytes()


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = os.path.join(root, 'resources', 'Summit.hvif')
    data = build()
    with open(target, 'wb') as out:
        out.write(data)
    print('wrote %s (%d bytes)' % (os.path.relpath(target, root), len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
