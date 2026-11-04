"""Interpolating subdivision of a skinned character mesh: PN-triangle edge curves, 1-to-4 split.

Every original vertex keeps its exact position and normal; only the new edge vertices move. Each
edge P1-P2 becomes the cubic curve of a PN triangle (Vlachos et al., "Curved PN Triangles", 2001):
its control points are the 1/3 and 2/3 points of the edge projected onto the tangent planes of the
two endpoint normals. The new vertex is that curve at its middle, which works out to

    M + d,   M = (P1 + P2) / 2,   d = -((P2 - P1).N1 * N1 + (P1 - P2).N2 * N2) / 8

and its normal is the PN quadratic normal at the middle. The curve only depends on the edge's two
endpoints and normals, so both faces of an edge get the same point, and no vertex is pulled toward
its neighbours: separate layers (a shirt under a vest) keep their original separation and the
silhouette does not slim, unlike Loop, which is approximating.

Edge rules (positions are computed on the welded geometric topology, so split vertices that share a
position move together; endpoint normals are the average of the split copies on the edge's faces):
- open boundary (one face: hems, sleeve ends, vest openings): the same PN curve. It bulges along the
  endpoint normals, perpendicular to the surface, so a hem ring rounds like the rings above it and
  stays on its plane; it never curls in or out of the surface.
- creased edge (two faces whose sides disagree, per `crease`) or non-manifold edge (three or more
  faces): the flat midpoint, so hard edges and seams stay straight.

Two clamps on the offset d (a new vertex only ever moves toward the flat midpoint M):
1. Cap: |d| <= MAX_BULGE * |P2 - P1|. With unit normals PN never exceeds 1/8 of the edge (a
   90-degree turn); the cap at 1/10 trims only the sharpest turns (more than about 53 degrees between
   the endpoint normals), typical of tips and folds on a low-poly mesh.
2. Other surfaces: "other" triangles are the original triangles that share no vertex with the
   edge's faces (another shell such as an eye inside the head, or a far part of the same shell), and
   the triangles that do share a vertex but face back toward the edge's faces (geometric normals more
   than 90 degrees apart: a fold, such as the underside of a vest panel meeting the shirt at the
   armhole in a thin wedge). t is the smallest of: the distance from M to an other triangle, and the
   distance from an other triangle's own (not shared) vertices to one of the edge's faces (the new
   vertex drags the middle of those faces along, so an eye close to the middle of a head face counts
   too). Then |d| <= ROOM * t. Two layers that bulge toward each other use at most 2 * ROOM = 80% of
   the room between them, so they cannot meet; surfaces that already touch (t = 0) keep their flat
   midpoints there. Only triangles within REACH * |d| of the edge's faces are searched, since
   farther ones cannot limit d.
"""
import math

from .loop import CREASE_MODES, _lerp, _normalize, creased, edge_map, mix_influences, weld
from .wmesh import Mesh, Vertex

MAX_BULGE = 0.1
ROOM = 0.4
REACH = 1.0 / ROOM


def _sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _length(a):
    return math.sqrt(_dot(a, a))


def pn_offset(p1, p2, n1, n2):
    """Offset of the PN edge curve's middle from the flat midpoint (unit normals n1, n2)."""
    e = _sub(p2, p1)
    w12, w21 = _dot(e, n1), -_dot(e, n2)
    return tuple(-(w12 * a + w21 * b) / 8 for a, b in zip(n1, n2))


def pn_normal(p1, p2, n1, n2):
    """PN quadratic normal at the middle of the edge."""
    e = _sub(p2, p1)
    ee = _dot(e, e)
    s = tuple(a + b for a, b in zip(n1, n2))
    v = 2 * _dot(e, s) / ee if ee > 1e-24 else 0.0
    return _normalize(tuple(0.25 * s[k] + 0.5 * (s[k] - v * e[k]) for k in range(3)))


def _point_triangle_distance(p, a, b, c):
    """Distance from p to the closest point of triangle abc (Ericson, Real-Time Collision Detection 5.1.5)."""
    ab, ac, ap = _sub(b, a), _sub(c, a), _sub(p, a)
    d1, d2 = _dot(ab, ap), _dot(ac, ap)
    if d1 <= 0 and d2 <= 0:
        q = a
    else:
        bp = _sub(p, b)
        d3, d4 = _dot(ab, bp), _dot(ac, bp)
        cp = _sub(p, c)
        d5, d6 = _dot(ab, cp), _dot(ac, cp)
        vc, vb, va = d1 * d4 - d3 * d2, d5 * d2 - d1 * d6, d3 * d6 - d5 * d4
        if d3 >= 0 and d4 <= d3:
            q = b
        elif d6 >= 0 and d5 <= d6:
            q = c
        elif vc <= 0 and d1 >= 0 and d3 <= 0:
            q = tuple(a[k] + ab[k] * d1 / (d1 - d3) for k in range(3))
        elif vb <= 0 and d2 >= 0 and d6 <= 0:
            q = tuple(a[k] + ac[k] * d2 / (d2 - d6) for k in range(3))
        elif va <= 0 and d4 - d3 >= 0 and d5 - d6 >= 0:
            w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
            q = tuple(b[k] + (c[k] - b[k]) * w for k in range(3))
        else:
            den = va + vb + vc
            v, w = vb / den, vc / den
            q = tuple(a[k] + ab[k] * v + ac[k] * w for k in range(3))
    return _length(_sub(p, q))


class _TriangleGrid:
    """Uniform grid over the original triangles (geometric ids) for the other-surface test."""

    def __init__(self, gpos, gtris, cell):
        self.gpos, self.gtris, self.cell = gpos, gtris, cell
        self.cells = {}
        for t, tri in enumerate(gtris):
            if len(set(tri)) < 3:
                continue
            lo, hi = self._box([gpos[g] for g in tri])
            for key in self._keys(lo, hi):
                self.cells.setdefault(key, []).append(t)

    def _box(self, pts):
        return [min(p[k] for p in pts) for k in range(3)], [max(p[k] for p in pts) for k in range(3)]

    def _keys(self, lo, hi):
        c = self.cell
        r = [range(int(math.floor(lo[k] / c)), int(math.floor(hi[k] / c)) + 1) for k in range(3)]
        return [(i, j, k) for i in r[0] for j in r[1] for k in r[2]]

    def room(self, o, faces, reach, ring, facing, own_faces):
        """Smallest distance (within reach) from o to an other triangle, or from an other triangle's
        vertices outside ring to one of faces (point triples); None when there is none. own_faces
        (indices) are never other; a triangle with a geometric id in ring is other only when its
        normal opposes facing (a fold)."""
        lo, hi = self._box([o] + [p for f in faces for p in f])
        lo, hi = [c - reach for c in lo], [c + reach for c in hi]
        best, seen = None, set()
        for key in self._keys(lo, hi):
            for t in self.cells.get(key, ()):
                if t in seen or t in own_faces:
                    continue
                seen.add(t)
                tri = self.gtris[t]
                pts = [self.gpos[g] for g in tri]
                own = [p for g, p in zip(tri, pts) if g not in ring]
                if len(own) < 3 and _dot(_cross(_sub(pts[1], pts[0]), _sub(pts[2], pts[0])), facing) >= 0:
                    continue
                h = min([_point_triangle_distance(o, *pts)]
                        + [_point_triangle_distance(q, *f) for q in own for f in faces])
                if h <= reach and (best is None or h < best):
                    best = h
        return best


def subdivide(mesh, crease="normals", stats=None):
    """One level of interpolating PN subdivision; 4x the triangles, original vertices unchanged.
    stats (optional dict) gets counts: curved, flat, capped, stopped edges."""
    if crease not in CREASE_MODES:
        raise ValueError("crease must be one of %s" % ", ".join(CREASE_MODES))
    V, I = mesh.vertices, mesh.indices
    gid, ng = weld(V)
    gpos = [None] * ng
    for i, v in enumerate(V):
        gpos[gid[i]] = v.pos
    tris = [tuple(I[t:t + 3]) for t in range(0, len(I), 3)]
    edges = edge_map(gid, tris)
    ftris = [tuple(gid[i] for i in tri) for tri in tris]
    mean_edge = sum(_length(_sub(gpos[a], gpos[b])) for a, b in edges) / max(1, len(edges))
    grid = _TriangleGrid(gpos, ftris, max(mean_edge, 1e-6))
    count = {"curved": 0, "flat": 0, "capped": 0, "stopped": 0}

    odd, curved = {}, set()
    for e, uses in edges.items():
        p1, p2 = gpos[e[0]], gpos[e[1]]
        mid = _lerp(p1, p2)
        flat = len(uses) > 2 or (len(uses) == 2 and creased(V, uses, crease))
        if not flat:
            s1 = [sum(V[u[1]].normal[k] for u in uses) for k in range(3)]
            s2 = [sum(V[u[2]].normal[k] for u in uses) for k in range(3)]
            flat = _length(s1) < 1e-6 or _length(s2) < 1e-6
        if flat:
            odd[e] = mid
            count["flat"] += 1
            continue
        d = pn_offset(p1, p2, _normalize(s1), _normalize(s2))
        dl = _length(d)
        cap = MAX_BULGE * _length(_sub(p2, p1))
        if dl > cap:
            count["capped"] += 1
            d, dl = tuple(c * cap / dl for c in d), cap
        if dl > 1e-12:
            ring = set(e) | {u[3] for u in uses}
            faces = [(p1, p2, gpos[u[3]]) for u in uses]
            facing = [0.0, 0.0, 0.0]
            for u in uses:
                a, b, c = (gpos[g] for g in ftris[u[0]])
                n = _cross(_sub(b, a), _sub(c, a))
                nl = _length(n)
                if nl > 1e-24:
                    facing = [facing[k] + n[k] / nl for k in range(3)]
            hit = grid.room(mid, faces, REACH * dl, ring, facing, {u[0] for u in uses})
            if hit is not None and ROOM * hit < dl:
                count["stopped"] += 1
                d, dl = tuple(c * ROOM * hit / dl for c in d), ROOM * hit
        odd[e] = tuple(mid[k] + d[k] for k in range(3))
        curved.add(e)
        count["curved"] += 1

    out = list(V)
    split_odd = {}   # (split a, split b) of one face's edge -> new split vertex

    def edge_vertex(x, y):
        key = (x, y) if x < y else (y, x)
        if key in split_odd:
            return split_odd[key]
        gx, gy = gid[x], gid[y]
        e = (gx, gy) if gx < gy else (gy, gx)
        a, b = V[x], V[y]
        if gx == gy:
            pos, n = a.pos, _normalize(_lerp(a.normal, b.normal))
        elif e in curved:
            pos, n = odd[e], pn_normal(a.pos, b.pos, _normalize(a.normal), _normalize(b.normal))
        else:
            pos, n = odd[e], _normalize(_lerp(a.normal, b.normal))
        slots, weights = mix_influences(a, b)
        out.append(Vertex(pos, n, _lerp(a.uv0, b.uv0), _lerp(a.uv1, b.uv1), slots, weights))
        split_odd[key] = len(out) - 1
        return split_odd[key]

    idx = []
    for a, b, c in tris:
        ab, bc, ca = edge_vertex(a, b), edge_vertex(b, c), edge_vertex(c, a)
        idx += [a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca]
    if stats is not None:
        for k, v in count.items():
            stats[k] = stats.get(k, 0) + v
    return Mesh(out, idx)
