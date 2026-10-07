#!/usr/bin/env python3
"""Generate resources/Summit.hvif, the Haiku vector icon for the app.

The artwork is a vector tracing of docs/summit-icon.png, a snow-capped peak with
a summit flag.  Geometry below stays in the pixel coordinates of that drawing
and is mapped into Haiku's 64x64 icon grid by `P()`, so the tracing remains
readable next to the original image.

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

# The tracing below uses pixel coordinates of the 1254x1254 source drawing,
# whose artwork spans x 185..1014 and y 120..1116 including its black outline.
# SCALE/ORIGIN centre that box in the 64x64 icon grid with a small margin.
SCALE = 0.059
ORIGIN_X, ORIGIN_Y = 7.55, 2.6


def P(x, y):
    return (round(ORIGIN_X + (x - 185) * SCALE, 3),
            round(ORIGIN_Y + (y - 120) * SCALE, 3))


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


def offset(points, distance, limit=None):
    """The polygon grown by `distance` (shrunk when negative), corners mitred.

    A mitre longer than `limit` times the distance is clipped square, the way
    AGG clips stroke joins, so the flag's sharp tip does not grow a spike.
    """
    edges = list(zip(points, points[1:] + points[:1]))
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in edges)
    sign = 1.0 if area > 0 else -1.0
    lines = []
    for (x0, y0), (x1, y1) in edges:
        length = math.hypot(x1 - x0, y1 - y0)
        nx, ny = sign * (y1 - y0) / length, -sign * (x1 - x0) / length
        lines.append(((x0 + nx * distance, y0 + ny * distance), (x1 - x0, y1 - y0)))
    result = []
    for corner, (p, d), (q, e) in zip(points, lines[-1:] + lines[:-1], lines):
        cross = d[0] * e[1] - d[1] * e[0]
        s = ((q[0] - p[0]) * e[1] - (q[1] - p[1]) * e[0]) / cross
        mx, my = p[0] + s * d[0], p[1] + s * d[1]
        reach = math.hypot(mx - corner[0], my - corner[1])
        convex = cross * area > 0
        if limit is None or convex != (distance > 0) or reach <= limit * abs(distance):
            result.append((mx, my))
            continue
        bx, by = (mx - corner[0]) / reach, (my - corner[1]) / reach
        cut = limit * abs(distance)
        for start, direction in ((p, d), (q, e)):
            t = (cut - ((start[0] - corner[0]) * bx + (start[1] - corner[1]) * by)) / \
                (direction[0] * bx + direction[1] * by)
            result.append((start[0] + t * direction[0], start[1] + t * direction[1]))
    return result


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


def ramp(start, end, first=0, last=255, steps=4):
    """Gradient stops for a straight blend from `start` to `end`.

    Haiku eases between neighbouring stops (INTERPOLATION_SMOOTH, which HVIF
    cannot switch off), so long blends get intermediate stops to stay linear.
    """
    stops = []
    for i in range(steps + 1):
        t = i / float(steps)
        stops.append((int(round(first + (last - first) * t)),
                      tuple(int(round(a + (b - a) * t)) for a, b in zip(start, end))))
    return stops


def radial_transform(cx, cy, rx, ry, angle):
    """Maps a circular gradient (radius 64) onto an ellipse in source pixels."""
    ox, oy = P(cx, cy)
    sx, sy = rx * SCALE / 64.0, ry * SCALE / 64.0
    c, s = math.cos(angle), math.sin(angle)
    return (c * sx, s * sx, -s * sy, c * sy, ox, oy)


# Width of the black outlines, drawn as filled rings around the centre lines
# below so the width can be given in source pixels (HVIF strokes come in whole
# icon units).  The drawing's own outlines are about 10.5 px, but Haiku blends
# edges in linear light, which leaves a line that thin pale and broken at icon
# sizes; 16 px reads the way the drawing does.
OUTLINE = 16

# Mountain outline, centre of the stroke, from the summit (hidden behind the
# pole) down the left side.  Every corner is a point of MOUNTAIN.
APEX, SHOULDER_L1, SHOULDER_L2 = (652, 322.5), (464, 568.5), (431.5, 585)
KNEE_L1, KNEE_L2, CORNER_L = (314, 800), (257, 846), (190.5, 952)
CORNER_B, CORNER_R = (732, 1110), (1009, 958.5)
SHOULDER_R2, SHOULDER_R1 = (863, 636.5), (796, 597)
MOUNTAIN = [APEX, SHOULDER_L1, SHOULDER_L2, KNEE_L1, KNEE_L2, CORNER_L,
            CORNER_B, CORNER_R, SHOULDER_R2, SHOULDER_R1]

# Where the snow ends: the left peak, the notch, the front ridge's peak, the
# tip of the snow tongue between the two right-hand faces, and the far face's
# peak.  The rest lie on the outline.
PEAK_L, NOTCH, PEAK_FRONT = (546.5, 602.5), (587, 718.5), (639.5, 676)
TONGUE, PEAK_FAR = (730.5, 773), (740.5, 604)
SNOW_L, FAR_R = (360, 716), (902, 723)
SHADE_TOP, SHADE_TOP_R = (650, 409), (682.5, 380)

# Lower edges of the sunlit left face and of its bright rim, and the feet of
# the ridges that divide the shaded faces.
LIT_KNEE, LIT_FOOT = (373, 831), (217, 960)
RIM = [(329.5, 815.5), (267.5, 862), (212, 958.5)]
RIDGE_FOOT, DARK_FOOT, DIAGONAL_FOOT = (746.5, 1102), (826.5, 1058.5), (962, 984)

SNOW = [APEX, SHOULDER_L1, SHOULDER_L2, SNOW_L, PEAK_L, NOTCH, PEAK_FRONT,
        TONGUE, SHADE_TOP, SHADE_TOP_R]
NEAR_FACE = [SNOW_L, PEAK_L, NOTCH, PEAK_FRONT, RIDGE_FOOT, CORNER_B,
             CORNER_L, KNEE_L2, KNEE_L1]
LIT_FACE = [SNOW_L, PEAK_L, LIT_KNEE, LIT_FOOT, CORNER_L, KNEE_L2, KNEE_L1]
LIT_RIM = [KNEE_L1] + RIM + [CORNER_L, KNEE_L2]
FRONT_FACE = [PEAK_FRONT, TONGUE, DARK_FOOT, RIDGE_FOOT]
FAR_FACE = [PEAK_FAR, FAR_R, CORNER_R, DIAGONAL_FOOT, TONGUE]
DARK_FACE = [TONGUE, DIAGONAL_FOOT, DARK_FOOT]

# Flag, centre of the stroke; its hoist is hidden under the pole's outline.
# The bright band along its top edge and the dark underside along its bottom
# edge both run out at the tip.
FLAG_TOP, FLAG_TIP, FLAG_BOTTOM = (667, 157), (916, 242), (667, 332)
FLAG = [FLAG_TOP, FLAG_TIP, FLAG_BOTTOM]
FLAG_FACE = [(667, 181), (864, 246), (667, 313)]
FLAG_BAND = [FLAG_TOP, FLAG_TIP, (864, 246), (667, 181)]

# Pole: a gold cylinder sunk into the snow, its top an ellipse.
POLE_LEFT, POLE_RIGHT = 620.6, 668
POLE_CAP_X, POLE_CAP_Y, POLE_CAP_RX, POLE_CAP_RY = 644.3, 144, 23.7, 14.5
POLE_FOOT_L, POLE_FOOT, POLE_FOOT_R = (620.6, 374.5), (647.5, 399.5), (668, 381)
POLE_BODY = [
    P(*POLE_FOOT_L),
    P(POLE_LEFT, POLE_CAP_Y) + P(POLE_LEFT, POLE_CAP_Y) +
    P(POLE_LEFT, POLE_CAP_Y - POLE_CAP_RY * KAPPA),
    P(POLE_CAP_X, POLE_CAP_Y - POLE_CAP_RY) +
    P(POLE_CAP_X - POLE_CAP_RX * KAPPA, POLE_CAP_Y - POLE_CAP_RY) +
    P(POLE_CAP_X + POLE_CAP_RX * KAPPA, POLE_CAP_Y - POLE_CAP_RY),
    P(POLE_RIGHT, POLE_CAP_Y) + P(POLE_RIGHT, POLE_CAP_Y - POLE_CAP_RY * KAPPA) +
    P(POLE_RIGHT, POLE_CAP_Y),
    P(*POLE_FOOT_R),
    P(*POLE_FOOT),
]


def pole_outline(width):
    """The pole widened by `width` with a round top, down to the mountain's
    outline; its bottom edge runs through the pole's feet, under the pole."""
    radius = (POLE_RIGHT - POLE_LEFT) / 2.0 + width
    top = POLE_CAP_Y - POLE_CAP_RY - width
    left, right = POLE_CAP_X - radius, POLE_CAP_X + radius
    k = radius * KAPPA
    # Where the mountain's slopes, centre of the stroke, pass the outline.
    left_slope = (SHOULDER_L1[1] - APEX[1]) / (APEX[0] - SHOULDER_L1[0])
    right_slope = (SHOULDER_R1[1] - APEX[1]) / (SHOULDER_R1[0] - APEX[0])
    left_foot = left, APEX[1] + (APEX[0] - left) * left_slope
    right_foot = right, APEX[1] + (right - APEX[0]) * right_slope
    return [
        P(*left_foot),
        P(left, top + radius) + P(left, top + radius) + P(left, top + radius - k),
        P(POLE_CAP_X, top) + P(POLE_CAP_X - k, top) + P(POLE_CAP_X + k, top),
        P(right, top + radius) + P(right, top + radius - k) + P(right, top + radius),
        P(*right_foot),
        P(*POLE_FOOT_R),
        P(*POLE_FOOT_L),
    ]


# The soft shadow cast to the lower right is an elliptical gradient behind the
# mountain, fitted to the drawing's shadow: solid out to SHADOW_SOLID of the
# radius, then fading away.
SHADOW_X, SHADOW_Y, SHADOW_RX, SHADOW_RY = 634, 939, 426, 199
SHADOW_ANGLE, SHADOW_SOLID = 0.127, 0.594


def rotated_box(cx, cy, rx, ry, angle):
    """The rectangle around an ellipse, which the shadow's gradient fills."""
    c, s = math.cos(angle), math.sin(angle)
    return [(cx + c * u * rx - s * v * ry, cy + s * u * rx + c * v * ry)
            for u, v in ((-1, -1), (1, -1), (1, 1), (-1, 1))]


BLACK = (0, 0, 0)


def ring(icon, style, points, width):
    """A band `width` wide centred on the polygon: outer edge and reversed inner edge."""
    outer = offset(points, width / 2.0, limit=2.0)
    inner = offset(points, -width / 2.0)
    icon.shape(style, [icon.path(Path(poly(outer))),
                       icon.path(Path(poly(inner[::-1])))])


def build():
    icon = Icon()

    ink = icon.style(Solid(*BLACK))
    shadow = icon.style(Gradient(GRADIENT_CIRCULAR, [(0, BLACK + (106,))] +
        ramp(BLACK + (106,), BLACK + (0,), first=int(SHADOW_SOLID * 255)),
        radial_transform(SHADOW_X, SHADOW_Y, SHADOW_RX, SHADOW_RY, SHADOW_ANGLE)))
    shade = icon.style(Gradient(GRADIENT_LINEAR,
        ramp((186, 199, 224), (217, 232, 250)),
        linear_transform(692, 571, 833, 537)))
    snow = icon.style(Gradient(GRADIENT_LINEAR,
        ramp((232, 237, 245), (255, 254, 244)),
        linear_transform(686, 710, 521, 482)))
    rock = icon.style(Gradient(GRADIENT_LINEAR,
        ramp((57, 113, 157), (123, 167, 198)),
        linear_transform(661, 1086, 425, 747)))
    rock_lit = icon.style(Solid(155, 193, 215))
    rock_rim = icon.style(Solid(178, 213, 233))
    rock_front = icon.style(Solid(44, 81, 112))
    rock_far = icon.style(Gradient(GRADIENT_LINEAR,
        ramp((37, 70, 99), (54, 94, 128)),
        linear_transform(774, 823, 924, 770)))
    rock_dark = icon.style(Solid(30, 60, 88))
    flag_underside = icon.style(Solid(150, 95, 4))
    flag = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (253, 208, 52)), (128, (243, 178, 14)), (255, (222, 152, 8))],
        linear_transform(690, 220, 860, 262)))
    flag_band = icon.style(Gradient(GRADIENT_LINEAR,
        ramp((255, 246, 190), (250, 210, 60)),
        linear_transform(680, 180, 880, 240)))
    pole = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (232, 165, 12)), (61, (253, 228, 76)), (104, (250, 198, 42)),
        (137, (232, 170, 20)), (140, (131, 78, 3)), (232, (133, 82, 3)),
        (255, (90, 52, 2))],
        linear_transform(620.6, 250, 668, 250)))
    pole_cap = icon.style(Gradient(GRADIENT_LINEAR, [
        (0, (243, 215, 120)), (60, (253, 252, 225)), (104, (253, 243, 164)),
        (137, (253, 211, 82)), (169, (239, 177, 41)), (201, (202, 138, 18)),
        (255, (160, 100, 3))],
        linear_transform(620.6, 144, 668, 144)))

    icon.draw(shadow, poly(rotated_box(SHADOW_X, SHADOW_Y, SHADOW_RX, SHADOW_RY,
                                       SHADOW_ANGLE)))

    # Mountain: the faces, painted over the shaded snow, then the outline.
    icon.draw(shade, poly(MOUNTAIN))
    icon.draw(snow, poly(SNOW))
    icon.draw(rock, poly(NEAR_FACE))
    icon.draw(rock_lit, poly(LIT_FACE))
    icon.draw(rock_rim, poly(LIT_RIM))
    icon.draw(rock_front, poly(FRONT_FACE))
    icon.draw(rock_far, poly(FAR_FACE))
    icon.draw(rock_dark, poly(DARK_FACE))
    ring(icon, ink, MOUNTAIN, OUTLINE)

    # Summit flag between the pole's outline and the pole itself, which covers
    # the summit and the flag's hoist.
    icon.draw(ink, pole_outline(OUTLINE))
    icon.draw(flag_underside, poly(FLAG))
    icon.draw(flag, poly(FLAG_FACE))
    icon.draw(flag_band, poly(FLAG_BAND))
    ring(icon, ink, FLAG, OUTLINE)
    icon.draw(pole, POLE_BODY)
    icon.draw(pole_cap, ellipse(POLE_CAP_X, POLE_CAP_Y, POLE_CAP_RX, POLE_CAP_RY))

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
