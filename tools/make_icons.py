# SPDX-License-Identifier: Apache-2.0
"""Draws the IPR window's toolbar icons into plugin/cyclesrender/res/icons.

    python tools/make_icons.py

Run it only to change an icon; the PNG files it writes are checked in. Each icon is described
below as a function inside(x, y) on the unit square (y down); the script samples it 6 x 6
times per pixel for smooth edges and writes a light-grey shape on a transparent background.
Standard library only.
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
    """The Cycles Camera tag's icon (the maintainer's wish, 0.2.0): the plugin's hexagon in grey,
    a dark background with a lighter circle, so it reads as the plugin's but not as a colour."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    hexagon = dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46
    if not hexagon:
        return None
    if math.hypot(x - 0.5, y - 0.5) < 0.24:
        shade = max(0.0, 1.0 - math.hypot(x - 0.42, y - 0.42) * 1.6)
        grey = int(170 + 80 * shade)
        return (grey, grey, grey)
    return (72, 72, 72)


if __name__ == "__main__":
    for icon_name, shape in (("play", play), ("stop", stop), ("pause", pause), ("pause_active", pause_active), ("reload", reload_icon),
                             ("lock_closed", lock(True)), ("lock_open", lock(False)), ("snapshot", snapshot),
                             ("frame", frame), ("settings", gear), ("cyclesrender", app), ("cyclesrender_grey", app_grey),
                             ("camera_tag", camera_tag)):
        render(icon_name, shape)
    print("icons written to", os.path.normpath(OUT))
