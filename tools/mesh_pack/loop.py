"""Loop subdivision of a skinned character mesh (wmesh.Mesh), attributes kept per corner.

The mesh's vertices are split where UVs or normals differ (a seam), so positions are first welded
into a geometric topology; positions are smoothed on that topology and every other attribute is
interpolated per split vertex, so UVs stay exact on both sides of a seam.

Creased edges (open boundaries, non-manifold edges, and depending on `crease`: hard-normal edges or
UV seams) take the boundary rules: a creased edge point is the edge midpoint, a vertex on exactly
two creased edges moves along them (3/4, 1/8, 1/8), a vertex on one or more than two creased edges
stays put. Bone slots and weights of a new vertex are the average of its two parents' influences,
reduced to the three largest (the vertex format holds three) and renormalized.
"""
import math

from .wmesh import Mesh, Vertex

CREASE_MODES = ("none", "normals", "uv")
HARD_COS = math.cos(math.radians(30))


def _lerp(a, b):
    return tuple((x + y) * 0.5 for x, y in zip(a, b))


def _normalize(n):
    d = math.sqrt(sum(c * c for c in n))
    return tuple(c / d for c in n) if d > 1e-12 else (0.0, 0.0, 1.0)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def mix_influences(a, b):
    """Average of two vertices' (slots, weights), the three largest kept, renormalized."""
    acc = {}
    for v in (a, b):
        for s, w in zip(v.slots, v.weights):
            if s >= 0 and w > 0:
                acc[s] = acc.get(s, 0.0) + w * 0.5
    top = sorted(acc.items(), key=lambda kv: (-kv[1], kv[0]))[:3]
    total = sum(w for _, w in top)
    if total <= 0:
        return a.slots, a.weights
    top += [(-1, 0.0)] * (3 - len(top))
    return tuple(s for s, _ in top), tuple(w / total for _, w in top)


def weld(vertices):
    """Geometric vertex id per split vertex (identical positions share one id) and the count."""
    ids, gid = {}, []
    for v in vertices:
        gid.append(ids.setdefault(tuple(v.pos), len(ids)))
    return gid, len(ids)


def edge_map(gid, tris):
    """Geometric edge (low id, high id) -> [(face, split a, split b, opposite geometric vertex)], where
    split a / split b are that face's split vertices on the low / high end."""
    edges = {}
    for f, (a, b, c) in enumerate(tris):
        for x, y, z in ((a, b, c), (b, c, a), (c, a, b)):
            gx, gy = gid[x], gid[y]
            if gx == gy:
                continue
            key = (gx, gy) if gx < gy else (gy, gx)
            sx, sy = (x, y) if gx < gy else (y, x)
            edges.setdefault(key, []).append((f, sx, sy, gid[z]))
    return edges


def creased(V, uses, crease):
    """A two-face edge is creased when its sides disagree: UVs ("uv") or normals past the hard-edge
    angle ("normals"); "none" never creases."""
    (_, a0, b0, _), (_, a1, b1, _) = uses
    if crease == "uv":
        return V[a0].uv0 != V[a1].uv0 or V[b0].uv0 != V[b1].uv0
    if crease == "normals":
        return _dot(V[a0].normal, V[a1].normal) < HARD_COS or _dot(V[b0].normal, V[b1].normal) < HARD_COS
    return False


def subdivide(mesh, crease="normals"):
    """One level of Loop subdivision; 4x the triangles."""
    if crease not in CREASE_MODES:
        raise ValueError("crease must be one of %s" % ", ".join(CREASE_MODES))
    V, I = mesh.vertices, mesh.indices
    gid, ng = weld(V)
    gpos = [None] * ng
    for i, v in enumerate(V):
        gpos[gid[i]] = v.pos
    tris = [tuple(I[t:t + 3]) for t in range(0, len(I), 3)]
    edges = edge_map(gid, tris)
    crease_set = {e for e, uses in edges.items() if len(uses) != 2 or creased(V, uses, crease)}
    nbrs = [set() for _ in range(ng)]
    cnb = [[] for _ in range(ng)]
    for (ga, gb) in edges:
        nbrs[ga].add(gb)
        nbrs[gb].add(ga)
        if (ga, gb) in crease_set:
            cnb[ga].append(gb)
            cnb[gb].append(ga)

    # Even (original) geometric vertices.
    even = []
    for g in range(ng):
        p = gpos[g]
        if len(cnb[g]) == 2:
            a, b = gpos[cnb[g][0]], gpos[cnb[g][1]]
            even.append(tuple(0.75 * p[k] + 0.125 * (a[k] + b[k]) for k in range(3)))
        elif cnb[g]:
            even.append(p)
        else:
            n = len(nbrs[g])
            if n < 3:
                even.append(p)
                continue
            beta = (0.625 - (0.375 + 0.25 * math.cos(2 * math.pi / n)) ** 2) / n
            s = [sum(gpos[q][k] for q in nbrs[g]) for k in range(3)]
            even.append(tuple((1 - n * beta) * p[k] + beta * s[k] for k in range(3)))

    # Odd (edge) points.
    odd = {}
    for e, uses in edges.items():
        a, b = gpos[e[0]], gpos[e[1]]
        if e in crease_set:
            odd[e] = _lerp(a, b)
        else:
            c, d = gpos[uses[0][3]], gpos[uses[1][3]]
            odd[e] = tuple(0.375 * (a[k] + b[k]) + 0.125 * (c[k] + d[k]) for k in range(3))

    out = [v._replace(pos=even[gid[i]]) for i, v in enumerate(V)]
    split_odd = {}   # (split a, split b) of one face's edge -> new split vertex

    def edge_vertex(x, y):
        key = (x, y) if x < y else (y, x)
        if key in split_odd:
            return split_odd[key]
        gx, gy = gid[x], gid[y]
        pos = odd[(gx, gy) if gx < gy else (gy, gx)] if gx != gy else V[x].pos
        a, b = V[x], V[y]
        slots, weights = mix_influences(a, b)
        out.append(Vertex(pos, _normalize(_lerp(a.normal, b.normal)), _lerp(a.uv0, b.uv0), _lerp(a.uv1, b.uv1), slots, weights))
        split_odd[key] = len(out) - 1
        return split_odd[key]

    idx = []
    for a, b, c in tris:
        ab, bc, ca = edge_vertex(a, b), edge_vertex(b, c), edge_vertex(c, a)
        idx += [a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca]
    return Mesh(out, idx)


def smooth_normals(mesh):
    """Area-weighted normals recomputed on the subdivided surface. Split vertices whose original
    normals agree (within the hard-edge angle) share one normal; hard edges stay hard."""
    V, I = mesh.vertices, mesh.indices
    gid, ng = weld(V)
    acc = [[0.0, 0.0, 0.0] for _ in V]
    for t in range(0, len(I), 3):
        a, b, c = (V[i].pos for i in I[t:t + 3])
        u = [b[k] - a[k] for k in range(3)]
        w = [c[k] - a[k] for k in range(3)]
        n = (u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0])
        for i in I[t:t + 3]:
            for k in range(3):
                acc[i][k] += n[k]
    groups = {}
    for i in range(len(V)):
        groups.setdefault(gid[i], []).append(i)
    out = list(V)
    for members in groups.values():
        for i in members:
            s = [0.0, 0.0, 0.0]
            for j in members:
                if _dot(V[i].normal, V[j].normal) >= HARD_COS:
                    for k in range(3):
                        s[k] += acc[j][k]
            n = _normalize(s)
            # Keep the original winding/orientation convention: flip if it opposes the old normal.
            if _dot(n, V[i].normal) < 0:
                n = tuple(-c for c in n)
            out[i] = V[i]._replace(normal=n)
    return Mesh(out, I)
