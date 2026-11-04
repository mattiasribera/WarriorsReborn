"""Character vertex format, content hash and .wmesh files (runtime/src/meshpack.h).

Stream 0, 0x20 bytes per vertex: float3 position, NORMPACKED3 normal (x:11, y:11, z:10 signed
normalized), float2 uv0, float2 uv1. Stream 1, 18 bytes: SHORT3 palette offsets (slot * 3; -3 for
an unused influence) and float3 weights. Triangle-list u16 indices.

The character payload (a 0x47 chunk with head {1, 0x76}) holds the three buffers back to back from
+0x80; a resource record at +0x80+tail gives the stream 1 and index buffer offsets.
"""
import struct
from collections import namedtuple

STRIDE0 = 0x20
STRIDE1 = 18
MAX_VERTS = 65535
HEAD_CHAR = (1, 0x76)
MAGIC = b"WMP1"
FNV_OFF = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3

# One vertex: pos (3 floats), normal (3 floats), uv0, uv1 (2 floats each), slots (3 ints, -1 =
# unused), weights (3 floats).
Vertex = namedtuple("Vertex", "pos normal uv0 uv1 slots weights")
Mesh = namedtuple("Mesh", "vertices indices")


def fnv1a64(data, h=FNV_OFF):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def content_hash(s0, s1, idx_bytes):
    """The runtime's hash of a draw: header {count, 0x20, 18, 0}, the indices, then the stream 0
    and stream 1 bytes of the vertices from the lowest to the highest index."""
    n = len(idx_bytes) // 2
    idx = struct.unpack("<%dH" % n, idx_bytes)
    lo, hi = min(idx), max(idx)
    h = fnv1a64(struct.pack("<4I", n, STRIDE0, STRIDE1, 0))
    h = fnv1a64(idx_bytes, h)
    h = fnv1a64(s0[lo * STRIDE0:(hi + 1) * STRIDE0], h)
    return fnv1a64(s1[lo * STRIDE1:(hi + 1) * STRIDE1], h)


def character_buffers(payload):
    """(stream 0, stream 1, index bytes) of a character payload, as the game hands them to D3D."""
    if len(payload) < 0x80 or struct.unpack_from("<II", payload, 0) != HEAD_CHAR:
        raise ValueError("not a skinned character payload")
    tail = struct.unpack_from("<I", payload, 12)[0]
    rec = 0x80 + tail
    if rec + 36 > len(payload):
        raise ValueError("resource record out of range")
    _c0, d0, _l0, _c1, d1, _l1, _c2, d2, _l2 = struct.unpack_from("<9I", payload, rec)
    if d0 or not d1 <= d2 <= tail:
        raise ValueError("unexpected resource offsets")
    nv = d1 // STRIDE0
    if d1 + nv * STRIDE1 > d2 or (tail - d2) % 6:
        raise ValueError("stream sizes don't match")
    base = 0x80
    return (bytes(payload[base:base + nv * STRIDE0]), bytes(payload[base + d1:base + d1 + nv * STRIDE1]),
            bytes(payload[base + d2:base + tail]))


def _sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def unpack_normal(u):
    return (_sext(u, 11) / 1023.0, _sext(u >> 11, 11) / 1023.0, _sext(u >> 22, 10) / 511.0)


def pack_normal(n):
    def q(v, scale, bits):
        i = int(round(max(-1.0, min(1.0, v)) * scale))
        return i & ((1 << bits) - 1)
    return q(n[0], 1023, 11) | q(n[1], 1023, 11) << 11 | q(n[2], 511, 10) << 22


def decode(s0, s1, idx_bytes):
    nv = len(s0) // STRIDE0
    verts = []
    for i in range(nv):
        x, y, z, n, u0, v0, u1, v1 = struct.unpack_from("<3fI4f", s0, i * STRIDE0)
        raw = struct.unpack_from("<3h3f", s1, i * STRIDE1)
        slots = tuple(s // 3 if s >= 0 else -1 for s in raw[:3])
        verts.append(Vertex((x, y, z), unpack_normal(n), (u0, v0), (u1, v1), slots, tuple(raw[3:])))
    return Mesh(verts, list(struct.unpack("<%dH" % (len(idx_bytes) // 2), idx_bytes)))


def encode(mesh):
    """(stream 0, stream 1, index bytes) of a Mesh."""
    if len(mesh.vertices) > MAX_VERTS:
        raise ValueError("%d vertices: the u16 index limit is %d" % (len(mesh.vertices), MAX_VERTS))
    s0, s1 = bytearray(), bytearray()
    for v in mesh.vertices:
        s0 += struct.pack("<3fI4f", *v.pos, pack_normal(v.normal), *v.uv0, *v.uv1)
        s1 += struct.pack("<3h3f", *(s * 3 if s >= 0 else -3 for s in v.slots), *v.weights)
    return bytes(s0), bytes(s1), struct.pack("<%dH" % len(mesh.indices), *mesh.indices)


def write_wmesh(path, mesh):
    s0, s1, ib = encode(mesh)
    with open(path, "wb") as f:
        f.write(MAGIC + struct.pack("<4I", len(mesh.vertices), len(mesh.indices), STRIDE0, STRIDE1) + s0 + s1 + ib)


def read_wmesh(data):
    if data[:4] != MAGIC:
        raise ValueError("not a .wmesh file")
    nv, ni, st0, st1 = struct.unpack_from("<4I", data, 4)
    if (st0, st1) != (STRIDE0, STRIDE1) or len(data) != 20 + nv * (STRIDE0 + STRIDE1) + ni * 2:
        raise ValueError("bad .wmesh layout")
    o = 20
    s0 = data[o:o + nv * STRIDE0]
    s1 = data[o + nv * STRIDE0:o + nv * (STRIDE0 + STRIDE1)]
    return decode(s0, s1, data[o + nv * (STRIDE0 + STRIDE1):])
