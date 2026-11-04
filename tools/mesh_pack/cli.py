"""python3 -m mesh_pack (PYTHONPATH=tools):

  build PAYLOAD... -o DIR [--levels N] [--crease none|normals|uv] [--scheme pn|loop]
        character payloads (0x47 chunks, head {1, 0x76}) -> DIR/<hash>.wmesh, where <hash> is the
        runtime's content hash of the original draw (xbrun --mesh-pack DIR)
  render PAYLOAD -o PNG [--levels N] [--crease ...] [--scheme ...] [--compare loop] [--yaw DEG]
        [--size PX] [--crop u0,u1,v0,v1]
        before | after (| the --compare scheme), flat-shaded, same framing
  --scheme pn (default): interpolating PN-triangle edge curves, original vertices unchanged, layer
        crossings clamped (pn.py); loop: approximating Loop subdivision (shrinks, loop.py)
  info FILE.wmesh|PAYLOAD
"""
import argparse
import os
import sys

from . import loop, pn, render, wmesh

SCHEMES = ("pn", "loop")


def smooth(mesh, levels, crease, scheme="pn", stats=None):
    """pn keeps the original vertices and normals; loop recomputes normals on the smoothed surface."""
    if scheme not in SCHEMES:
        raise ValueError("scheme must be one of %s" % ", ".join(SCHEMES))
    for _ in range(levels):
        mesh = pn.subdivide(mesh, crease, stats) if scheme == "pn" else loop.subdivide(mesh, crease)
    return loop.smooth_normals(mesh) if levels and scheme == "loop" else mesh


def load_payload(path):
    with open(path, "rb") as f:
        s0, s1, ib = wmesh.character_buffers(f.read())
    return wmesh.content_hash(s0, s1, ib), wmesh.decode(s0, s1, ib)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="mesh_pack")
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("payloads", nargs="+")
    b.add_argument("-o", "--out", required=True)
    r = sub.add_parser("render")
    r.add_argument("payload")
    r.add_argument("-o", "--out", required=True)
    r.add_argument("--yaw", type=float, default=0.0)
    r.add_argument("--size", type=int, default=480)
    r.add_argument("--crop")
    for p in (b, r):
        p.add_argument("--levels", type=int, default=1)
        p.add_argument("--crease", choices=loop.CREASE_MODES, default="normals")
        p.add_argument("--scheme", choices=SCHEMES, default="pn")
    r.add_argument("--compare", choices=SCHEMES)
    i = sub.add_parser("info")
    i.add_argument("file")
    a = ap.parse_args(argv)
    if a.cmd == "build":
        os.makedirs(a.out, exist_ok=True)
        for path in a.payloads:
            h, m = load_payload(path)
            st = {}
            s = smooth(m, a.levels, a.crease, a.scheme, st)
            dst = os.path.join(a.out, "%016x.wmesh" % h)
            wmesh.write_wmesh(dst, s)
            extra = (" (edges: %(curved)d curved, %(flat)d flat, %(capped)d capped, %(stopped)d stopped)" % st
                     if st else "")
            print("%s: %d -> %d triangles, %d -> %d vertices -> %s%s" % (path, len(m.indices) // 3, len(s.indices) // 3,
                                                                         len(m.vertices), len(s.vertices), dst, extra))
    elif a.cmd == "render":
        _, m = load_payload(a.payload)
        meshes = [m, smooth(m, a.levels, a.crease, a.scheme)]
        if a.compare:
            meshes.append(smooth(m, a.levels, a.crease, a.compare))
        box = render.bounds([m], a.yaw)
        crop = tuple(float(x) for x in a.crop.split(",")) if a.crop else None
        render.write_png(a.out, render.side_by_side(*(render.render(x, a.size, a.yaw, box, crop) for x in meshes)))
        print("%s: %d -> %d triangles -> %s" % (a.payload, len(m.indices) // 3, len(meshes[1].indices) // 3, a.out))
    else:
        with open(a.file, "rb") as f:
            data = f.read()
        if data[:4] == wmesh.MAGIC:
            m = wmesh.read_wmesh(data)
            print("%d vertices, %d triangles" % (len(m.vertices), len(m.indices) // 3))
        else:
            s0, s1, ib = wmesh.character_buffers(data)
            print("hash %016x, %d vertices, %d triangles" % (wmesh.content_hash(s0, s1, ib), len(s0) // wmesh.STRIDE0,
                                                              len(ib) // 6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
