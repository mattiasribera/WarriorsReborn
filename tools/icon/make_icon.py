#!/usr/bin/env python3
"""Draws the port's application icon: an original, purely geometric Ferris wheel (a "Wonder Wheel":
ring, spokes, gondolas, A-frame base) in red on a dark rounded square. No game artwork, logo or
font is used; every pixel comes from the shapes below. Standard library only (no Pillow).

    python3 tools/icon/make_icon.py [--png-dir DIR]

writes assets/icon/wonder-wheel.ico (16, 24, 32, 48, 64, 128, 256 px) and the vector version
assets/icon/wonder-wheel.svg (the detailed level), drawn from the same shapes; --png-dir also
writes each size as a PNG there, to look at. Small sizes use fewer, thicker parts so they stay
readable: 16 px is ring, hub and A-frame only.

The ICO holds 32-bit DIBs (with the 1-bit AND mask) below 256 px and a PNG at 256 px, the usual
layout that Windows' resource compilers and Explorer accept.
"""
import math
import os
import struct
import sys
import zlib

SIZES = (16, 24, 32, 48, 64, 128, 256)
SS = 4  # supersamples per axis for anti-aliasing

BG = (0x11, 0x11, 0x11)
RED = (0xC8, 0x10, 0x2E)
RADIUS = 0.2                 # corner radius of the rounded square (fraction of the size)
HUB = (0.5, 0.42)            # wheel centre (unit square, y down)
WHEEL_R = 0.27               # ring radius
FOOT_Y = 0.87                # ground line


def shapes(size):
    """The red parts for an icon of this size: ("ring", cx, cy, r, half_width), ("disc", cx, cy, r)
    and ("line", ax, ay, bx, by, half_width) with round ends."""
    cx, cy = HUB
    if size <= 16:
        ring_w, spoke_w, spokes, cars, leg_w, inner = 0.05, 0.03, 0, 0, 0.05, False
    elif size <= 32:
        ring_w, spoke_w, spokes, cars, leg_w, inner = 0.035, 0.02, 8, 8, 0.035, False
    else:
        ring_w, spoke_w, spokes, cars, leg_w, inner = 0.026, 0.009, 16, 12, 0.028, True
    out = [("ring", cx, cy, WHEEL_R, ring_w)]
    if inner:
        out.append(("ring", cx, cy, WHEEL_R * 0.55, 0.012))
    for i in range(spokes):
        a = math.pi / 4 + 2 * math.pi * i / spokes
        out.append(("line", cx, cy, cx + WHEEL_R * math.cos(a), cy + WHEEL_R * math.sin(a), spoke_w))
    for i in range(cars):  # gondolas: beads just outside the ring
        a = -math.pi / 2 + 2 * math.pi * i / cars
        r = WHEEL_R + (0.025 if size <= 32 else 0.035)
        out.append(("disc", cx + r * math.cos(a), cy + r * math.sin(a), 0.034 if size <= 32 else 0.027))
    out += [
        ("line", cx, cy, 0.29, FOOT_Y, leg_w),
        ("line", cx, cy, 0.71, FOOT_Y, leg_w),
        ("line", 0.2, FOOT_Y, 0.8, FOOT_Y, leg_w),
        ("disc", cx, cy, 0.06 if size <= 16 else 0.045),
    ]
    return out


def rounded_square_dist(x, y):
    """Signed distance (negative inside) to the rounded square spanning the unit square."""
    cx = min(max(x, RADIUS), 1 - RADIUS)
    cy = min(max(y, RADIUS), 1 - RADIUS)
    return math.hypot(x - cx, y - cy) - RADIUS


def seg_dist(px, py, ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = min(max(t, 0.0), 1.0)
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def inside(parts, x, y):
    for p in parts:
        if p[0] == "ring" and abs(math.hypot(x - p[1], y - p[2]) - p[3]) <= p[4]:
            return True
        if p[0] == "disc" and math.hypot(x - p[1], y - p[2]) <= p[3]:
            return True
        if p[0] == "line" and seg_dist(x, y, *p[1:5]) <= p[5]:
            return True
    return False


def render(size):
    """Rows of (r, g, b, a) byte tuples, top to bottom."""
    parts = shapes(size)
    rows = []
    for py in range(size):
        row = []
        for px in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(SS):
                for sx in range(SS):
                    x, y = (px + (sx + 0.5) / SS) / size, (py + (sy + 0.5) / SS) / size
                    if rounded_square_dist(x, y) > 0:
                        continue
                    c = RED if inside(parts, x, y) else BG
                    for i in range(3):
                        acc[i] += c[i]
                    acc[3] += 255.0
            alpha = acc[3] / (SS * SS)
            if alpha <= 0:
                row.append((0, 0, 0, 0))
            else:  # colours of the covered samples only
                n = acc[3] / 255.0
                row.append(tuple(int(round(acc[i] / n)) for i in range(3)) + (int(round(alpha)),))
        rows.append(row)
    return rows


def svg(size=256):
    def f(v):
        return f"{v * 100:.2f}"
    red = "#%02x%02x%02x" % RED
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="{size}" height="{size}">',
           f'<rect width="100" height="100" rx="{f(RADIUS)}" fill="#%02x%02x%02x"/>' % BG]
    for p in shapes(size):
        if p[0] == "ring":
            out.append(f'<circle cx="{f(p[1])}" cy="{f(p[2])}" r="{f(p[3])}" fill="none" stroke="{red}" '
                       f'stroke-width="{f(2 * p[4])}"/>')
        elif p[0] == "disc":
            out.append(f'<circle cx="{f(p[1])}" cy="{f(p[2])}" r="{f(p[3])}" fill="{red}"/>')
        else:
            out.append(f'<line x1="{f(p[1])}" y1="{f(p[2])}" x2="{f(p[3])}" y2="{f(p[4])}" stroke="{red}" '
                       f'stroke-width="{f(2 * p[5])}" stroke-linecap="round"/>')
    return "\n".join(out + ["</svg>", ""])


def png(rows):
    size = len(rows)
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def dib(rows):
    size = len(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    stride = ((size + 31) // 32) * 4
    mask = bytearray()
    for row in reversed(rows):
        bits = bytearray(stride)
        for x, px in enumerate(row):
            if px[3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    return header + xor + bytes(mask)


def ico(images):
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    blobs = []
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs.append(data)
    return out + b"".join(blobs)


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    png_dir = sys.argv[sys.argv.index("--png-dir") + 1] if "--png-dir" in sys.argv else None
    out_dir = os.path.join(root, "assets", "icon")
    os.makedirs(out_dir, exist_ok=True)
    images = []
    for s in SIZES:
        rows = render(s)
        images.append((s, png(rows) if s >= 256 else dib(rows)))
        if png_dir:
            with open(os.path.join(png_dir, f"icon-{s}.png"), "wb") as f:
                f.write(png(rows))
    with open(os.path.join(out_dir, "wonder-wheel.ico"), "wb") as f:
        f.write(ico(images))
    with open(os.path.join(out_dir, "wonder-wheel.svg"), "w") as f:
        f.write(svg())
    print(f"assets/icon/wonder-wheel.ico: {', '.join(str(s) for s in SIZES)} px; assets/icon/wonder-wheel.svg")


if __name__ == "__main__":
    main()
