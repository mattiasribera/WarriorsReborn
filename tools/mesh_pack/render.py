"""Tiny software renderer for before/after comparisons: orthographic, z-buffered, flat-shaded
(so facets and silhouettes show), grayscale PNG. Game space is Z-up; the view turns `yaw` degrees
around Z and looks along +Y."""
import math
import struct
import zlib


def _project(mesh, yaw):
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return [(c * x - s * y, s * x + c * y, z) for x, y, z in (v.pos for v in mesh.vertices)]


def bounds(meshes, yaw):
    pts = [p for m in meshes for p in _project(m, yaw)]
    return (min(p[0] for p in pts), max(p[0] for p in pts), min(p[2] for p in pts), max(p[2] for p in pts))


def render(mesh, size, yaw=0.0, box=None, crop=None):
    """Rows of grayscale bytes (size x size). box: (x0, x1, z0, z1) world framing (shared between
    before and after); crop: (u0, u1, v0, v1) fraction of that box to zoom in on."""
    P = _project(mesh, yaw)
    x0, x1, z0, z1 = box or bounds([mesh], yaw)
    if crop:
        u0, u1, v0, v1 = crop
        x0, x1, z0, z1 = x0 + (x1 - x0) * u0, x0 + (x1 - x0) * u1, z1 - (z1 - z0) * v1, z1 - (z1 - z0) * v0
    span = max(x1 - x0, z1 - z0) * 1.04
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    sc = size / span
    scr = [((p[0] - cx) * sc + size / 2, size / 2 - (p[2] - cz) * sc, p[1]) for p in P]
    zbuf = [float("inf")] * (size * size)
    img = bytearray([24]) * (size * size)
    L = (0.35, -0.8, 0.5)
    ln = math.sqrt(sum(c * c for c in L))
    L = tuple(c / ln for c in L)
    I = mesh.indices
    for t in range(0, len(I), 3):
        a, b, c = P[I[t]], P[I[t + 1]], P[I[t + 2]]
        u = [b[k] - a[k] for k in range(3)]
        w = [c[k] - a[k] for k in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        d = math.sqrt(sum(k * k for k in n)) or 1.0
        shade = abs(sum(n[k] * L[k] for k in range(3)) / d)   # two-sided
        col = int(40 + 200 * shade)
        A, B, C = scr[I[t]], scr[I[t + 1]], scr[I[t + 2]]
        area = (B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0])
        if abs(area) < 1e-12:
            continue
        minx, maxx = max(0, int(min(A[0], B[0], C[0]))), min(size - 1, int(max(A[0], B[0], C[0])) + 1)
        miny, maxy = max(0, int(min(A[1], B[1], C[1]))), min(size - 1, int(max(A[1], B[1], C[1])) + 1)
        for py in range(miny, maxy + 1):
            fy = py + 0.5
            for px in range(minx, maxx + 1):
                fx = px + 0.5
                w0 = ((B[0] - fx) * (C[1] - fy) - (B[1] - fy) * (C[0] - fx)) / area
                w1 = ((C[0] - fx) * (A[1] - fy) - (C[1] - fy) * (A[0] - fx)) / area
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                z = w0 * A[2] + w1 * B[2] + w2 * C[2]
                o = py * size + px
                if z < zbuf[o]:
                    zbuf[o] = z
                    img[o] = col
    return [bytes(img[r * size:(r + 1) * size]) for r in range(size)]


def side_by_side(*images, gap=8):
    h = len(images[0])
    return [(b"\x50" * gap).join(im[r] for im in images) for r in range(h)]


def write_png(path, rows):
    w, h = len(rows[0]), len(rows)
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
