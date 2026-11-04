"""tools/mesh_pack (offline smoothing for xbrun --mesh-pack) and runtime/build/meshpack_test.

Synthetic meshes only: an octahedron (closed, smooth), a cube split per face (hard normals, every
edge creased), a strip with a UV seam and two layered open tubes (a fine inner shirt under a coarse
outer vest). Checks the runtime's content hash constants (shared with
runtime/tools/meshpack_test.c), the vertex format round trip, the .wmesh writer and the Loop rules
(4x triangles, three influences summing to 1, per-corner UVs, creases) and the interpolating
default: original vertices stay put, layers do not cross, hems do not curl.
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from mesh_pack import cli, loop, wmesh  # noqa: E402
from mesh_pack.wmesh import Mesh, Vertex  # noqa: E402


def octahedron():
    P = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    V = [Vertex(p, p, (0.1 * i, 0.2), (0.0, 0.0), (i % 3, (i + 1) % 3, -1), (0.75, 0.25, 0.0)) for i, p in enumerate(P)]
    F = [(0, 2, 4), (2, 1, 4), (1, 3, 4), (3, 0, 4), (2, 0, 5), (1, 2, 5), (3, 1, 5), (0, 3, 5)]
    return Mesh(V, [i for f in F for i in f])


def split_cube():
    V, I = [], []
    for axis in range(3):
        for sign in (-1, 1):
            n = tuple(sign if k == axis else 0 for k in range(3))
            u, v = [k for k in range(3) if k != axis]
            base = len(V)
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                p = [0, 0, 0]
                p[axis], p[u], p[v] = sign, a, b
                V.append(Vertex(tuple(float(c) for c in p), n, ((a + 1) / 2, (b + 1) / 2), (0, 0), (0, -1, -1), (1, 0, 0)))
            I += [base, base + 1, base + 2, base, base + 2, base + 3]
    return Mesh(V, I)


def tube(radius, segments, rings, phase=0.0):
    """Open tube around Z with radial normals (a sleeve or a torso layer); both ends are hems."""
    V, I = [], []
    for j, z in enumerate(rings):
        for i in range(segments):
            a = 2 * math.pi * (i + phase) / segments
            c, s = math.cos(a), math.sin(a)
            V.append(Vertex((radius * c, radius * s, float(z)), (c, s, 0.0), (i / segments, float(j)), (0.0, 0.0),
                            (0, -1, -1), (1.0, 0.0, 0.0)))
    for j in range(len(rings) - 1):
        for i in range(segments):
            a, b = j * segments + i, j * segments + (i + 1) % segments
            I += [a, b, b + segments, a, b + segments, a + segments]
    return Mesh(V, I)


def merge(*meshes):
    V, I = [], []
    for m in meshes:
        I += [i + len(V) for i in m.indices]
        V += m.vertices
    return Mesh(V, I)


def ray_hit(o, d, a, b, c):
    """Distance along d from o to triangle abc (Moller-Trumbore), or None."""
    e1 = [b[k] - a[k] for k in range(3)]
    e2 = [c[k] - a[k] for k in range(3)]
    p = (d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0])
    det = sum(e1[k] * p[k] for k in range(3))
    if abs(det) < 1e-12:
        return None
    t0 = [o[k] - a[k] for k in range(3)]
    u = sum(t0[k] * p[k] for k in range(3)) / det
    q = (t0[1] * e1[2] - t0[2] * e1[1], t0[2] * e1[0] - t0[0] * e1[2], t0[0] * e1[1] - t0[1] * e1[0])
    v = sum(d[k] * q[k] for k in range(3)) / det
    if u < -1e-9 or v < -1e-9 or u + v > 1 + 1e-9:
        return None
    return sum(e2[k] * q[k] for k in range(3)) / det


def layer_gap(smoothed, n_inner, z_lo, z_hi):
    """Smallest (outer surface radius - inner vertex radius) along radial rays from the Z axis, for the
    first n_inner vertices (the inner tube) between z_lo and z_hi. Negative: the inner layer pokes out."""
    V, I = smoothed.vertices, smoothed.indices
    outer = [t for t in range(0, len(I), 3) if min(I[t:t + 3]) >= n_inner]
    gap = float("inf")
    for v in V[:n_inner]:
        x, y, z = v.pos
        if not z_lo <= z <= z_hi:
            continue
        r = math.hypot(x, y)
        o, d = (0.0, 0.0, z), (x / r, y / r, 0.0)
        hits = [h for t in outer for h in [ray_hit(o, d, *(V[i].pos for i in I[t:t + 3]))] if h is not None and h > 0]
        gap = min(gap, min(hits) - r)
    return gap


class HashTest(unittest.TestCase):
    def test_constants_match_runtime(self):
        h = wmesh.content_hash(bytes(range(96)), bytes(range(54)), struct.pack("<3H", 0, 1, 2))
        self.assertEqual(h, 0x89F37B56E5BDDF58)
        h = wmesh.content_hash(bytes(range(128)), bytes(range(72)), struct.pack("<3H", 1, 2, 3))
        self.assertEqual(h, 0x8C72595E17BCC73F)

    def test_runtime_test_binary(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/meshpack_test"], check=True)
        res = subprocess.run([os.path.join(ROOT, "runtime", "build", "meshpack_test")], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("ALL OK", res.stdout)


class FormatTest(unittest.TestCase):
    def test_round_trip(self):
        m = octahedron()
        s0, s1, ib = wmesh.encode(m)
        self.assertEqual((len(s0), len(s1), len(ib)), (6 * 32, 6 * 18, 48))
        d = wmesh.decode(s0, s1, ib)
        self.assertEqual(d.indices, m.indices)
        for a, b in zip(d.vertices, m.vertices):
            self.assertEqual(a.pos, b.pos)
            self.assertEqual(a.slots, b.slots)
            for x, y in zip(a.normal, b.normal):
                self.assertAlmostEqual(x, y, delta=2e-3)
        self.assertEqual(struct.unpack_from("<3h", s1, 0), (0, 3, -3))   # slot * 3, -3 unused

    def test_build_from_payload(self):
        m = octahedron()
        s0, s1, ib = wmesh.encode(m)
        d1, d2 = len(s0), len(s0) + len(s1)
        tail = d2 + len(ib)
        payload = bytearray(struct.pack("<4I", 1, 0x76, 3, tail)) + bytes(0x70) + s0 + s1 + ib
        payload += struct.pack("<9I", 0x00800001, 0, 0, 0x00800001, d1, 0, 0x00010001, d2, 0)
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "char.bin")
            with open(src, "wb") as f:
                f.write(payload)
            cli.main(["build", src, "-o", os.path.join(tmp, "pack")])
            name = "%016x.wmesh" % wmesh.content_hash(s0, s1, ib)
            with open(os.path.join(tmp, "pack", name), "rb") as f:
                out = wmesh.read_wmesh(f.read())
        self.assertEqual(len(out.indices), 4 * len(m.indices))


class LoopTest(unittest.TestCase):
    def check_influences(self, mesh):
        for v in mesh.vertices:
            self.assertEqual(len(v.slots), 3)
            self.assertAlmostEqual(sum(v.weights), 1.0, places=6)

    def test_smooth_closed_mesh(self):
        m = octahedron()
        s = loop.subdivide(m)
        self.assertEqual(len(s.indices), 4 * len(m.indices))
        self.assertEqual(len(s.vertices), 6 + 12)
        self.check_influences(s)
        # Smooth (no crease): the corners pull in, so the octahedron rounds off.
        self.assertLess(max(abs(c) for c in s.vertices[0].pos), 1.0)
        # Edge points blend four vertices: (3/8)(a + b) + (1/8)(c + d), still on the symmetric axis plane.
        r = [math.sqrt(sum(c * c for c in v.pos)) for v in s.vertices]
        self.assertLess(max(r) - min(r), 0.25)
        # Mixed influences: slots of both parents, weights averaged and renormalized.
        mixed = loop.mix_influences(m.vertices[0], m.vertices[2])
        self.assertEqual(dict(zip(*mixed)), {0: 0.5, 1: 0.125, 2: 0.375})

    def test_hard_cube_keeps_corners(self):
        m = split_cube()
        s = loop.subdivide(m, crease="normals")
        self.assertEqual(len(s.indices), 4 * len(m.indices))
        # Every cube corner touches three creased edges: it stays put, and creased edge points are midpoints.
        for v in s.vertices[:len(m.vertices)]:
            self.assertEqual(sorted(abs(c) for c in v.pos), [1.0, 1.0, 1.0])
        for v in s.vertices[len(m.vertices):]:
            self.assertTrue(all(abs(c) <= 1.0 for c in v.pos))
        # Per-corner UVs: face-edge points carry the midpoint of that face's UVs.
        for v in s.vertices[len(m.vertices):]:
            self.assertTrue(all(c in (0.0, 0.5, 1.0) for c in v.uv0))
        self.check_influences(s)

    def test_uv_seam_crease(self):
        # Two quads sharing an edge, with a UV seam on it (split vertices, same position).
        P = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0.5), (2, 1, 0.5)]
        V = [Vertex(p, (0, 0, 1), (p[0] / 4, p[1]), (0, 0), (0, -1, -1), (1, 0, 0)) for p in P]
        V += [V[1]._replace(uv0=(0.9, 0.0)), V[2]._replace(uv0=(0.9, 1.0))]   # 6, 7: seam copies of 1, 2
        I = [0, 1, 2, 0, 2, 3, 6, 4, 5, 6, 5, 7]
        m = Mesh(V, I)
        smooth = loop.subdivide(m, crease="none")
        creased = loop.subdivide(m, crease="uv")
        # Both seam copies move together (positions are smoothed on the welded topology).
        self.assertEqual(smooth.vertices[1].pos, smooth.vertices[6].pos)
        # The seam's edge point exists once per side, with that side's UVs.
        seam_pts = [v for v in creased.vertices[8:] if v.pos[0] == 1.0 and abs(v.pos[1] - 0.5) < 1e-9]
        self.assertEqual(sorted(v.uv0[0] for v in seam_pts), [0.25, 0.9])
        # Creased: the seam stays straight (edge point is the midpoint, z = 0).
        self.assertTrue(all(v.pos[2] == 0.0 for v in seam_pts))

    def test_normals_recomputed_unit(self):
        s = loop.smooth_normals(loop.subdivide(octahedron()))
        for v in s.vertices:
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in v.normal)), 1.0, places=6)
            self.assertGreater(sum(a * b for a, b in zip(v.normal, v.pos)), 0)   # outward


class LayerTest(unittest.TestCase):
    """A fine inner shirt (24 segments, r = 1) under a coarse outer vest (6 segments, R = 1.17, every
    flat face still outside the shirt). Loop pulls the coarse vest in further than the fine shirt, so
    the shirt pokes through; the default pipeline must keep the layers apart."""
    RINGS = [0, 1, 2, 3]

    def layered(self):
        inner = tube(1.0, 24, self.RINGS)
        return inner, merge(inner, tube(1.17, 6, self.RINGS, phase=0.5))

    def test_original_gap_positive(self):
        inner, m = self.layered()
        self.assertGreater(layer_gap(m, len(inner.vertices), 0.0, 3.0), 0.0)

    def test_loop_crosses_layers(self):
        inner, m = self.layered()
        self.assertLess(layer_gap(loop.subdivide(m), len(inner.vertices), 0.25, 2.75), 0.0)

    def test_default_keeps_layers_apart(self):
        inner, m = self.layered()
        s = cli.smooth(m, 1, "normals")
        self.assertEqual(len(s.indices), 4 * len(m.indices))
        n = len(inner.vertices)
        # Interpolating: every original vertex keeps its exact position.
        self.assertEqual([v.pos for v in s.vertices[:len(m.vertices)]], [v.pos for v in m.vertices])
        self.assertGreater(layer_gap(s, n, 0.25, 2.75), 0.0)

    def test_hems_do_not_curl(self):
        # Open ends: every hem edge point stays exactly on its hem plane (no curl) and rounds outward
        # from the flat chord midpoint without passing the circle the hem vertices sit on.
        _, m = self.layered()
        s = cli.smooth(m, 1, "normals")
        for hem in (0.0, 3.0):
            pts = [v.pos for v in s.vertices[len(m.vertices):] if abs(v.pos[2] - hem) < 0.2]
            self.assertEqual(len(pts), 24 + 6)
            for x, y, z in pts:
                self.assertEqual(z, hem)
                r = math.hypot(x, y)
                inner = r < 1.005
                lo, hi = (math.cos(math.pi / 24), 1.0) if inner else (1.17 * math.cos(math.pi / 6), 1.17)
                self.assertTrue(lo + 1e-4 < r <= hi + 1e-9, r)


def sheet(half, z, normal_of, slot=0):
    """Square (two triangles) in the plane at height z, normals given per corner."""
    P = [(-half, -half, z), (half, -half, z), (half, half, z), (-half, half, z)]
    V = [Vertex(p, normal_of(p), (0.0, 0.0), (0.0, 0.0), (slot, -1, -1), (1.0, 0.0, 0.0)) for p in P]
    return Mesh(V, [0, 1, 2, 0, 2, 3])


def dome_normal(p):
    n = (p[0], p[1], 1.0)
    d = math.sqrt(sum(c * c for c in n))
    return tuple(c / d for c in n)


class GuardTest(unittest.TestCase):
    """The two clamps on a new (edge) vertex: a cap on its offset from the flat edge midpoint, and a
    stop short of any other surface its offset would cross."""

    def offsets(self, m, s, n_new):
        """(edge length, offset vector from the flat midpoint) of the first n_new new vertices, which
        follow the original vertices in triangle-edge order (ab, bc, ca, skipping shared edges)."""
        out, seen, I = [], set(), m.indices
        for t in range(0, len(I), 3):
            for a, b in ((I[t], I[t + 1]), (I[t + 1], I[t + 2]), (I[t + 2], I[t])):
                if (min(a, b), max(a, b)) in seen:
                    continue
                seen.add((min(a, b), max(a, b)))
                p, q = m.vertices[a].pos, m.vertices[b].pos
                v = s.vertices[len(m.vertices) + len(out)].pos
                out.append((math.dist(p, q), tuple(v[k] - (p[k] + q[k]) / 2 for k in range(3))))
        return out[:n_new]

    def test_bulge_capped_by_edge_length(self):
        # Dome normals tilted 54.7 degrees apart: the raw bulge is about 0.118 of each edge; capped at 0.1.
        m = sheet(1.0, 0.0, dome_normal)
        s = cli.smooth(m, 1, "normals")
        for length, off in self.offsets(m, s, 5):
            self.assertAlmostEqual(math.sqrt(sum(c * c for c in off)), 0.1 * length, places=9)
            self.assertGreater(off[2], 0.0)

    def test_bulge_stops_short_of_other_shell(self):
        # The same sheet under a flat outer sheet 0.05 above (a separate shell): each new vertex keeps at
        # most 40% of the room to the other surface along its offset, so it stays below z = 0.02.
        inner = sheet(1.0, 0.0, dome_normal)
        m = merge(inner, sheet(2.0, 0.05, lambda p: (0.0, 0.0, 1.0)))
        s = cli.smooth(m, 1, "normals")
        for _, off in self.offsets(m, s, 5):
            self.assertGreater(off[2], 0.0)
            self.assertLessEqual(off[2], 0.4 * 0.05 + 1e-9)
        # Alone, the same sheet bulges up to the cap (the stop is caused by the other shell).
        alone = cli.smooth(inner, 1, "normals")
        self.assertGreater(max(v.pos[2] for v in alone.vertices), 0.25)

class WedgeTest(unittest.TestCase):
    """A vest panel's underside meets the shirt at an armhole: the two surfaces share a hinge edge and
    open at a thin wedge (0.15 high at 1 away). Their vertex normals face each other, so both bulge
    into the wedge; the fold rule must keep them apart."""

    def test_wedge_layers_stay_apart(self):
        def n(x, y, up):
            return dome_normal((x, y - 0.5, 1.0))[:2] + (up * dome_normal((x, y - 0.5, 1.0))[2],)

        def vert(p, up):
            return Vertex(p, n(p[0], p[1], up), (0.0, 0.0), (0.0, 0.0), (0, -1, -1), (1.0, 0.0, 0.0))
        H0, H1, S0, S1 = (-1.0, 0.0, 0.0), (1.0, 0.0, 0.0), (-1.0, 1.0, 0.0), (1.0, 1.0, 0.0)
        T0, T1 = (-1.0, 1.0, 0.15), (1.0, 1.0, 0.15)
        V = [vert(p, 1.0) for p in (H0, H1, S0, S1)] + [vert(p, -1.0) for p in (H0, H1, T0, T1)]
        m = Mesh(V, [0, 1, 3, 0, 3, 2, 4, 7, 5, 4, 6, 7])
        s = cli.smooth(m, 1, "normals")
        new = s.vertices[len(m.vertices):]
        # Five new vertices per side; the hinge is creased (normals 84 degrees apart): flat midpoint.
        shirt, vest = new[:5], new[5:]
        self.assertEqual(len(vest), 5)
        for v in shirt + vest:
            if v.pos[1] == 0.0:
                self.assertEqual(v.pos, (0.0, 0.0, 0.0))
        for v in shirt:
            if v.pos[1] > 0:
                self.assertLess(v.pos[2], 0.15 * v.pos[1] - 1e-6)
        for v in vest:
            if v.pos[1] > 0:
                self.assertGreater(v.pos[2], 1e-6)

if __name__ == "__main__":
    unittest.main()
