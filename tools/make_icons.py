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
    """Two arrows chasing each other around a circle, with a little space between the tip of each and the
    tail of the other."""
    def at(angle, radius):
        return (0.5 + radius * math.cos(math.radians(angle)), 0.5 + radius * math.sin(math.radians(angle)))
    radius, angle = math.hypot(x - 0.5, y - 0.5), math.degrees(math.atan2(y - 0.5, x - 0.5)) % 360
    for start in (10.0, 190.0):
        # The shaft over 115 degrees, then the head, whose tip ends 22 degrees before the other arrow's tail.
        if 0.24 <= radius <= 0.36 and start <= angle <= start + 116:
            return True
        if triangle(x, y, at(start + 115, 0.14), at(start + 115, 0.46), at(start + 158, 0.30)):
            return True
    return False


def lock(closed):
    """A padlock whose body is a camera seen from the front, as the Cycles Camera tag's icon shows one: the
    ring around its lens and the small window beside it are cut out."""
    def inside(x, y):
        left = 0.17 if closed else 0.06
        lens = math.hypot(x - (left + 0.33), y - 0.65)
        camera = (rounded_box(x, y, left, 0.42, left + 0.66, 0.88, 0.06) and not 0.075 <= lens < 0.14
                  and not rounded_box(x, y, left + 0.5, 0.48, left + 0.6, 0.54, 0.015))
        if closed:
            dx, dy = x - (left + 0.33), y - 0.42
            shackle = (0.15 <= math.hypot(dx, dy) <= 0.23 and dy <= 0) or (0.15 <= abs(dx) <= 0.23 and 0 <= dy <= 0.02)
        else:
            # Swung open: the shackle stands to the right, held by one leg only.
            dx, dy = x - (left + 0.7), y - 0.24
            shackle = (0.15 <= math.hypot(dx, dy) <= 0.23 and dy <= 0) or (-0.23 <= dx <= -0.15 and 0 <= dy <= 0.2)
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


def hexagon(x, y):
    """The plugin's hexagon, which every object's and tag's icon is drawn in."""
    dx, dy = abs(x - 0.5), abs(y - 0.5)
    return dy <= 0.44 and dx <= 0.4 and dx * 0.55 + dy <= 0.46


def camera_tag(x, y):
    """The Cycles Camera tag's icon: the plugin's hexagon in a dark grey (so it reads as the plugin's but
    not as a colour) with a camera seen from the front, as large as the hexagon takes: a body with the
    viewfinder's hump on top, a lens in its middle and a small window beside it."""
    if not hexagon(x, y):
        return None
    dark = (62, 62, 62)
    body = rounded_box(x, y, 0.21, 0.36, 0.79, 0.72, 0.05) or rounded_box(x, y, 0.39, 0.28, 0.61, 0.40, 0.03)
    if not body:
        return dark
    lens = math.hypot(x - 0.5, y - 0.54)
    if lens < 0.075:
        return (190, 190, 190)
    if lens < 0.135 or rounded_box(x, y, 0.66, 0.41, 0.74, 0.46, 0.015):
        return dark
    return (244, 244, 244)


def light(x, y):
    """The icon of the Cycles Light object and of the Cycles Light tag: the plugin's hexagon in a dark
    yellow with a sun as large as the hexagon takes: a bright disc with eight rays."""
    if not hexagon(x, y):
        return None
    distance = math.hypot(x - 0.5, y - 0.5)
    if distance < 0.14:
        shade = max(0.0, 1.0 - math.hypot(x - 0.46, y - 0.46) / 0.17)
        return (255, int(236 + 19 * shade), int(150 + 90 * shade))
    # A ray: a short bar pointing away from the centre, every 45 degrees.
    turn = (math.atan2(y - 0.5, x - 0.5) + math.pi / 8) % (math.pi / 4) - math.pi / 8
    if 0.185 <= distance * math.cos(turn) <= 0.3 and abs(distance * math.sin(turn)) < 0.026:
        return (255, 240, 170)
    return (132, 92, 8)


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


# Where the scatter icon's dots sit (x, y, radius): a few stones of different sizes, placed by hand so that
# they look strewn and none touches another or the hexagon's edge.
SCATTER_DOTS = ((0.36, 0.36, 0.065), (0.57, 0.30, 0.05), (0.67, 0.50, 0.07), (0.42, 0.58, 0.055), (0.55, 0.70, 0.045),
                (0.30, 0.49, 0.035), (0.72, 0.67, 0.035), (0.50, 0.46, 0.035))


def scatter(x, y):
    """The icon of the Cycles Scatter object: the plugin's hexagon in a deep green with copies strewn over
    it, as large as the hexagon takes."""
    if not hexagon(x, y):
        return None
    for cx, cy, r in SCATTER_DOTS:
        if math.hypot(x - cx, y - cy) < r:
            shade = max(0.0, 1.0 - math.hypot(x - cx + r * 0.3, y - cy + r * 0.3) / r)
            return (int(200 + 55 * shade), int(225 + 30 * shade), int(150 + 80 * shade))
    return (34, 84, 46)


# The proxy icon's box, seen from a corner: the six corners around its outline (clockwise from the top) and
# the corner in the middle that faces the viewer.
PROXY_OUTLINE = ((0.50, 0.24), (0.725, 0.37), (0.725, 0.63), (0.50, 0.76), (0.275, 0.63), (0.275, 0.37))
PROXY_FRONT = (0.50, 0.50)


def proxy(x, y):
    """The icon of the Cycles Proxy object: the plugin's hexagon in a deep violet with a box drawn as lines,
    as large as the hexagon takes: what stands in for a file's content in the viewport."""
    if not hexagon(x, y):
        return None
    edges = [(PROXY_OUTLINE[i], PROXY_OUTLINE[(i + 1) % 6]) for i in range(6)]
    edges += [(PROXY_FRONT, PROXY_OUTLINE[i]) for i in (1, 3, 5)]
    for (ax, ay), (bx, by) in edges:
        length = math.hypot(bx - ax, by - ay)
        along = min(1.0, max(0.0, ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / (length * length)))
        if math.hypot(x - ax - along * (bx - ax), y - ay - along * (by - ay)) < 0.022:
            return (236, 226, 255)
    return (66, 46, 118)


if __name__ == "__main__":
    for icon_name, shape in (("play", play), ("stop", stop), ("pause", pause), ("pause_active", pause_active), ("reload", reload_icon),
                             ("lock_closed", lock(True)), ("lock_open", lock(False)), ("snapshot", snapshot),
                             ("frame", frame), ("settings", gear), ("cyclesrender", app), ("cyclesrender_grey", app_grey),
                             ("camera_tag", camera_tag), ("light_object", light), ("light_tag", light), ("fog_object", fog),
                             ("environment_object", environment), ("scatter_object", scatter), ("proxy_object", proxy)):
        render(icon_name, shape)
    print("icons written to", os.path.normpath(OUT))
