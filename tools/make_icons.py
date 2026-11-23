# SPDX-License-Identifier: Apache-2.0
"""Draws the plugin's icons (the IPR window's toolbar, the commands, tags, the light, fog, environment, scatter and proxy
objects) into plugin/cyclesrender/res/icons.

    python tools/make_icons.py

Run it only to change an icon; the PNG files it writes are checked in. Each icon is described
below as a function inside(x, y) on the unit square (y down); the script samples it 6 x 6
times per pixel for smooth edges and writes it on a transparent background, light grey unless
the function gives colours of its own. Standard library only.
"""

import math
import os
import struct
import zlib

SIZE = 64
SAMPLES = 6
GREY = (200, 200, 200)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "plugin", "cyclesrender", "res", "icons")


def write_png(path, pixels):
    """pixels: rows of (r, g, b, a)."""
    raw = b"".join(b"\x00" + bytes(c for pixel in row for c in pixel) for row in pixels)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", len(pixels[0]), len(pixels), 8, 6, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        handle.write(chunk(b"IEND", b""))


def render(name, inside, color=GREY):
    """inside(x, y) -> bool, or an (r, g, b) colour for that point, or None/False for nothing."""
    rows = []
    for py in range(SIZE):
        row = []
        for px in range(SIZE):
            hits, total = 0, [0, 0, 0]
            for sy in range(SAMPLES):
                for sx in range(SAMPLES):
                    value = inside((px + (sx + 0.5) / SAMPLES) / SIZE, (py + (sy + 0.5) / SAMPLES) / SIZE)
                    if value:
                        rgb = color if value is True else value
                        hits += 1
                        total = [t + c for t, c in zip(total, rgb)]
            if hits:
                row.append((total[0] // hits, total[1] // hits, total[2] // hits, 255 * hits // (SAMPLES * SAMPLES)))
            else:
                row.append((0, 0, 0, 0))
        rows.append(row)
    os.makedirs(OUT, exist_ok=True)
    write_png(os.path.join(OUT, name + ".png"), rows)


def rounded_box(x, y, x0, y0, x1, y1, r):
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return x0 <= x <= x1 and y0 <= y <= y1 and math.hypot(x - cx, y - cy) <= r


def triangle(x, y, a, b, c):
    def side(p, q):
        return (x - q[0]) * (p[1] - q[1]) - (p[0] - q[0]) * (y - q[1])
    d1, d2, d3 = side(a, b), side(b, c), side(c, a)
    return not ((d1 < 0 or d2 < 0 or d3 < 0) and (d1 > 0 or d2 > 0 or d3 > 0))


def play(x, y):
    return triangle(x, y, (0.24, 0.14), (0.24, 0.86), (0.84, 0.5))


def stop(x, y):
    return rounded_box(x, y, 0.2, 0.2, 0.8, 0.8, 0.08)


def pause(x, y):
    return rounded_box(x, y, 0.2, 0.16, 0.42, 0.84, 0.04) or rounded_box(x, y, 0.58, 0.16, 0.8, 0.84, 0.04)


def pause_active(x, y):
    """The pause icon while the render is paused: white bars on a blue button."""
    if pause(x, y):
        return (255, 255, 255)
    return (79, 97, 190) if rounded_box(x, y, 0.0, 0.0, 1.0, 1.0, 0.16) else None


def reload_icon(x, y):
    """Two arrows chasing each other around a circle."""
    dx, dy = x - 0.5, y - 0.5
    radius, angle = math.hypot(dx, dy), math.degrees(math.atan2(dy, dx)) % 360
    ring = 0.25 <= radius <= 0.37 and ((20 <= angle <= 170) or (200 <= angle <= 350))
    heads = False
    for tip_angle in (10.0, 190.0):
        a = math.radians(tip_angle)
        tip = (0.5 + 0.31 * math.cos(a) - 0.17 * math.sin(a) * -1, 0.5 + 0.31 * math.sin(a) - 0.17 * math.cos(a))
        inner = (0.5 + 0.14 * math.cos(a + 0.5), 0.5 + 0.14 * math.sin(a + 0.5))
        outer = (0.5 + 0.48 * math.cos(a + 0.5), 0.5 + 0.48 * math.sin(a + 0.5))
        heads = heads or triangle(x, y, tip, inner, outer)
    return ring or heads


def lock(closed):
    """A padlock whose body is a film camera: a box with a lens, and the viewfinder cone at its side."""
    def inside(x, y):
        left = 0.17 if closed else 0.08
        box = rounded_box(x, y, left, 0.44, left + 0.48, 0.86, 0.07)
        cone = triangle(x, y, (left + 0.47, 0.65), (left + 0.66, 0.51), (left + 0.66, 0.79))
        lens = math.hypot(x - (left + 0.24), y - 0.65)
        camera = (box and not 0.05 <= lens <= 0.11) or cone
        if closed:
            dx, dy = x - (left + 0.24), y - 0.44
            shackle = (0.13 <= math.hypot(dx, dy) <= 0.21 and dy <= 0) or (0.13 <= abs(dx) <= 0.21 and 0 <= dy <= 0.02)
        else:
            # Swung open: the shackle stands to the right, held by one leg only.
            dx, dy = x - (left + 0.52), y - 0.26
            shackle = (0.13 <= math.hypot(dx, dy) <= 0.21 and dy <= 0) or (-0.21 <= dx <= -0.13 and 0 <= dy <= 0.2)
        return camera or shackle
    return inside


def snapshot(x, y):
    """A picture with a second one behind it."""
    front = rounded_box(x, y, 0.24, 0.14, 0.9, 0.7, 0.06)
    window = rounded_box(x, y, 0.3, 0.2, 0.84, 0.64, 0.02)
    mountain = triangle(x, y, (0.32, 0.62), (0.5, 0.36), (0.66, 0.62)) or triangle(x, y, (0.52, 0.62), (0.68, 0.44), (0.82, 0.62))
    sun = math.hypot(x - 0.72, y - 0.3) < 0.05
    back = (rounded_box(x, y, 0.1, 0.3, 0.76, 0.86, 0.06) and not rounded_box(x, y, 0.16, 0.24, 0.9, 0.8, 0.06)
            and not rounded_box(x, y, 0.2, 0.1, 0.94, 0.74, 0.06))
    return (front and not window) or (window and (mountain or sun)) or back


def frame(x, y):
    """Four corner brackets."""
    def bracket(cx, cy, sx, sy):
        u, v = (x - cx) * sx, (y - cy) * sy
        return (0 <= u <= 0.24 and 0 <= v <= 0.08) or (0 <= u <= 0.08 and 0 <= v <= 0.24)
    return bracket(0.16, 0.16, 1, 1) or bracket(0.84, 0.16, -1, 1) or bracket(0.16, 0.84, 1, -1) or bracket(0.84, 0.84, -1, -1)


def gear(x, y):
    dx, dy = x - 0.5, y - 0.5
    radius, angle = math.hypot(dx, dy), math.atan2(dy, dx)
    tooth = abs(((angle / (2 * math.pi) * 8) % 1.0) - 0.5) < 0.24
    return 0.14 <= radius <= (0.44 if tooth else 0.33)


def app(x, y):
    """The command's icon: a blue hexagon with a lighter sphere, after the mock-up's title bar."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    distance = math.hypot(x - 0.5, y - 0.5)
    if distance < 0.22:
        shade = 1.0 - math.hypot(x - 0.42, y - 0.42) * 1.6
        return (int(90 + 110 * shade), int(110 + 110 * shade), 255)
    return (50, 60, 200)


def app_grey(x, y):
    """The same shape without colour: the icon of the two material commands."""
    colour = app(x, y)
    if not colour:
        return None
    grey = min(255, int(0.3 * colour[0] + 0.5 * colour[1] + 0.2 * colour[2] + 40))
    return (grey, grey, grey)


def camera_tag(x, y):
    """The Cycles Camera tag's icon: the plugin's hexagon in grey, a dark background with a
    lighter circle, so it reads as the plugin's but not as a colour."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        shade = max(0.0, 1.0 - math.hypot(x - 0.42, y - 0.42) * 1.6)
        grey = int(170 + 80 * shade)
        return (grey, grey, grey)
    return (72, 72, 72)


def light(x, y):
    """The icon of the Cycles Light object and of the Cycles Light tag: the camera tag's kind of
    variant in yellow, the plugin's hexagon in a dark yellow with a lighter, shaded yellow circle."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        shade = max(0.0, 1.0 - math.hypot(x - 0.42, y - 0.42) * 1.6)
        return (255, int(200 + 50 * shade), int(40 + 150 * shade))
    return (150, 112, 12)


def fog(x, y):
    """The icon of the Cycles Fog object: the light's kind of variant in a misty blue grey, the
    plugin's hexagon dark, the circle filled with soft bands of fog that thin out towards the top."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        band = 0.5 + 0.5 * math.sin((y + 0.06 * math.sin(x * 9.0)) * 34.0)
        mist = min(1.0, max(0.0, 0.35 + 0.45 * band + 0.6 * (y - 0.4)))
        return (int(120 + 125 * mist), int(135 + 115 * mist), int(155 + 100 * mist))
    return (52, 66, 82)


def environment(x, y):
    """The icon of the Cycles Environment object: the light's kind of variant in sky colours, the plugin's
    hexagon in a deep blue with a sky in the circle, light at its horizon and a small sun."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        if math.hypot(x - 0.58, y - 0.42) < 0.06:
            return (255, 236, 170)
        if y > 0.56:
            return (70, 90, 70)
        height = min(1.0, max(0.0, (0.56 - y) / 0.3))
        return (int(200 - 120 * height), int(225 - 95 * height), int(245 - 20 * height))
    return (28, 46, 98)


# Where the scatter icon's dots sit in its circle (x, y, radius): a few stones of different sizes, placed
# by hand so that they look strewn and none touches another or the circle's edge.
SCATTER_DOTS = ((0.40, 0.40, 0.045), (0.55, 0.36, 0.035), (0.62, 0.50, 0.05), (0.44, 0.56, 0.04), (0.53, 0.64, 0.03),
                (0.36, 0.49, 0.025), (0.66, 0.62, 0.025), (0.50, 0.47, 0.025))


def scatter(x, y):
    """The icon of the Cycles Scatter object: the light's kind of variant in greens, the plugin's hexagon in a
    deep green with copies strewn over a lighter ground in the circle."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        for cx, cy, r in SCATTER_DOTS:
            d = math.hypot(x - cx, y - cy)
            if d < r:
                shade = max(0.0, 1.0 - math.hypot(x - cx + r * 0.3, y - cy + r * 0.3) / r)
                return (int(200 + 55 * shade), int(225 + 30 * shade), int(150 + 80 * shade))
        return (78, 128, 70)
    return (30, 72, 40)


# The proxy icon's box, seen from a corner: the six corners around its outline (clockwise from the top) and
# the corner in the middle that faces the viewer.
PROXY_OUTLINE = ((0.50, 0.335), (0.64, 0.415), (0.64, 0.585), (0.50, 0.665), (0.36, 0.585), (0.36, 0.415))
PROXY_FRONT = (0.50, 0.50)


def proxy(x, y):
    """The icon of the Cycles Proxy object: the light's kind of variant in violets, the plugin's hexagon in a
    deep violet with a box drawn as lines in the circle: what stands in for a file's content in the viewport."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        edges = [(PROXY_OUTLINE[i], PROXY_OUTLINE[(i + 1) % 6]) for i in range(6)]
        edges += [(PROXY_FRONT, PROXY_OUTLINE[i]) for i in (1, 3, 5)]
        for (ax, ay), (bx, by) in edges:
            length = math.hypot(bx - ax, by - ay)
            along = min(1.0, max(0.0, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / (length * length)))
            if math.hypot(x - ax - along * (bx - ax), y - ay - along * (by - ay)) < 0.014:
                return (236, 226, 255)
        return (112, 92, 168)
    return (58, 40, 104)


if __name__ == "__main__":
    for icon_name, shape in (("play", play), ("stop", stop), ("pause", pause), ("pause_active", pause_active), ("reload", reload_icon),
                             ("lock_closed", lock(True)), ("lock_open", lock(False)), ("snapshot", snapshot),
                             ("frame", frame), ("settings", gear), ("cyclesrender", app), ("cyclesrender_grey", app_grey),
                             ("camera_tag", camera_tag), ("light_object", light), ("light_tag", light), ("fog_object", fog),
                             ("environment_object", environment), ("scatter_object", scatter), ("proxy_object", proxy)):
        render(icon_name, shape)
    print("icons written to", os.path.normpath(OUT))
