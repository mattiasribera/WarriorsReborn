"""Writes mods/91-own-crate/own/crate.glb: the example mod's wooden crate, made entirely here (no
game data). A 0.6 m crate standing on its origin: a slightly smaller box of planks inside twelve
edge beams, plus a node named "collision" (a plain 0.6 m box) for its collision patch. glTF 2.0
binary, Y-up, metres, as Blender's glTF exporter writes (docs/MODDING.md "Your own assets").
UVs: the planks use the top half of the texture, the beams the bottom half (the mod's assets.ini
paints both halves with procedural wood). Python 3 standard library only; the output is
deterministic, so running it again rewrites the same bytes.

usage: python3 tools/mod_examples/make_crate_glb.py [OUT.glb]
"""
import json
import os
import struct
import sys

SIZE, BEAM = 0.6, 0.06


def box(lo, hi, uv_band):
    """24 vertices (4 per face, flat normals) and 12 triangles of an axis box in glTF axes (y up).
    Each face maps its two in-plane axes onto the texture band (v0, v1) by metres / 0.6."""
    v0, v1 = uv_band
    faces = [  # (normal, axis u, axis v)
        ((1, 0, 0), 2, 1), ((-1, 0, 0), 2, 1), ((0, 1, 0), 0, 2), ((0, -1, 0), 0, 2), ((0, 0, 1), 0, 1), ((0, 0, -1), 0, 1)]
    h = SIZE / 2
    pos, nrm, uv, idx = [], [], [], []
    for n, a, b in faces:
        axis = [i for i in range(3) if n[i]][0]
        c = hi[axis] if n[axis] > 0 else lo[axis]
        corners = [(lo[a], lo[b]), (hi[a], lo[b]), (hi[a], hi[b]), (lo[a], hi[b])]
        base = len(pos)
        for ca, cb in corners:
            p = [0.0, 0.0, 0.0]
            p[axis], p[a], p[b] = c, ca, cb
            pos.append(p)
            nrm.append(list(map(float, n)))
            uv.append([round((ca + h) / SIZE, 6), round(v0 + (v1 - v0) * (cb - lo[b]) / max(hi[b] - lo[b], 1e-6), 6)])
        # counter-clockwise seen from outside (the normal's side)
        p0, p1, p2 = pos[base], pos[base + 1], pos[base + 2]
        e1 = [p1[i] - p0[i] for i in range(3)]
        e2 = [p2[i] - p0[i] for i in range(3)]
        g = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        q = [0, 1, 2, 0, 2, 3] if sum(g[i] * n[i] for i in range(3)) > 0 else [0, 2, 1, 0, 3, 2]
        idx += [base + k for k in q]
    return pos, nrm, uv, idx


def crate():
    h = SIZE / 2
    parts = [box([-h + 0.02, 0.02, -h + 0.02], [h - 0.02, SIZE - 0.02, h - 0.02], (0.0, 0.5))]
    for x in (-h, h - BEAM):  # beams along z
        for y in (0.0, SIZE - BEAM):
            parts.append(box([x, y, -h], [x + BEAM, y + BEAM, h], (0.5, 1.0)))
    for z in (-h, h - BEAM):  # beams along x
        for y in (0.0, SIZE - BEAM):
            parts.append(box([-h, y, z], [h, y + BEAM, z + BEAM], (0.5, 1.0)))
    for x in (-h, h - BEAM):  # posts (y)
        for z in (-h, h - BEAM):
            parts.append(box([x, 0.0, z], [x + BEAM, SIZE, z + BEAM], (0.5, 1.0)))
    pos, nrm, uv, idx = [], [], [], []
    for p, n, u, i in parts:
        base = len(pos)
        pos += p
        nrm += n
        uv += u
        idx += [base + k for k in i]
    return pos, nrm, uv, idx


def winding_check(pos, nrm, idx):
    """Every triangle's geometric normal points the way its vertex normal does."""
    for t in range(0, len(idx), 3):
        a, b, c = (pos[idx[t + k]] for k in range(3))
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        g = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        if sum(g[i] * nrm[idx[t]][i] for i in range(3)) <= 0:
            raise SystemExit("triangle %d faces inwards" % (t // 3))


def glb(meshes):
    """meshes: [(name, pos, nrm, uv or None, idx)] -> .glb bytes, one node per mesh."""
    binb, views, accs, gl_meshes = bytearray(), [], [], []

    def view(data):
        while len(binb) % 4:
            binb.append(0)
        views.append({"buffer": 0, "byteOffset": len(binb), "byteLength": len(data)})
        binb.extend(data)
        return len(views) - 1

    def acc(rows, typ, comp=5126, minmax=False):
        flat = [x for r in rows for x in (r if isinstance(r, list) else [r])]
        fmt = "<%d%s" % (len(flat), "f" if comp == 5126 else "H")
        a = {"bufferView": view(struct.pack(fmt, *flat)), "componentType": comp, "count": len(rows), "type": typ}
        if minmax:
            a["min"] = [min(r[i] for r in rows) for i in range(3)]
            a["max"] = [max(r[i] for r in rows) for i in range(3)]
        accs.append(a)
        return len(accs) - 1

    for name, pos, nrm, uv, idx in meshes:
        attrs = {"POSITION": acc(pos, "VEC3", minmax=True), "NORMAL": acc(nrm, "VEC3")}
        if uv:
            attrs["TEXCOORD_0"] = acc(uv, "VEC2")
        gl_meshes.append({"name": name, "primitives": [{"attributes": attrs, "indices": acc(idx, "SCALAR", 5123), "mode": 4}]})
    doc = {"asset": {"version": "2.0", "generator": "WarriorsReborn tools/mod_examples/make_crate_glb.py"},
           "scene": 0, "scenes": [{"nodes": list(range(len(meshes)))}],
           "nodes": [{"name": m[0], "mesh": i} for i, m in enumerate(meshes)],
           "meshes": gl_meshes, "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": len(binb)}]}
    js = json.dumps(doc, separators=(",", ":"), sort_keys=True).encode()
    js += b" " * (-len(js) % 4)
    binb += b"\0" * (-len(binb) % 4)
    total = 12 + 8 + len(js) + 8 + len(binb)
    return (struct.pack("<4sII", b"glTF", 2, total) + struct.pack("<I4s", len(js), b"JSON") + js +
            struct.pack("<I4s", len(binb), b"BIN\0") + bytes(binb))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "mods", "91-own-crate", "own", "crate.glb")
    pos, nrm, uv, idx = crate()
    winding_check(pos, nrm, idx)
    h = SIZE / 2
    cp, cn, _, ci = box([-h, 0.0, -h], [h, SIZE, h], (0.0, 1.0))
    winding_check(cp, cn, ci)
    data = glb([("crate", pos, nrm, uv, idx), ("collision", cp, cn, None, ci)])
    with open(out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %d vertices, %d triangles" % (out, len(data), len(pos), len(idx) // 3))


if __name__ == "__main__":
    main()
