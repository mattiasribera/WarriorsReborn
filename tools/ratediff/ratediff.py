#!/usr/bin/env python3
"""Rate diff: find guest fields that change about 2x (or 0.5x) as fast per game second with
--sim60 as with the default 30 Hz world.

Input: two snapshot files written by `xbrun --rate-probe FILE[:MS]` (runtime/src/rateprobe.h)
from the same scripted input, one without and one with --sim60. Runs are not frame-
deterministic, so values are never compared, only rates: per 4-byte field, the total absolute
change per game second (as int32, float32 and both int16 halves), over a window of Swap numbers
split into chunks of equal game time. A field is flagged when both runs change it often enough,
the overall ratio sim60/default lies in a band around 2 (or 0.5) and most chunks agree.
Flagged fields are grouped (character-pool slots by field offset, other addresses by
adjacency) and annotated with the instructions that reference them, from an objdump of the
XBE's .text, and whether the writer lies inside the world-update closure.

usage: ratediff.py BASE.rp SIM.rp [--from-frame N] [--to-frame N] [--chunks K]
                   [--disasm text.s | --xbe default.xbe] [--closure closure.txt] [--out PREFIX]
Writes PREFIX.md and PREFIX.csv (default ~/.cache/rate-diff/report).
"""
import argparse
import bisect
import collections
import csv
import os
import re
import struct
import subprocess
import sys
import tempfile

import numpy as np

MAGIC, SNAP, VERSION = 0x42525052, 0x50414E53, 1
KIND_NONE, KIND_FULL, KIND_DELTA = 0, 1, 2
FLAG_SIM60 = 1
KINDS = ("i32", "f32", "lo16", "hi16")

# Game-specific layout used for labels and grouping (The Warriors). Fields inside a struct array are grouped by offset across its slots.
STRUCTS = [
    ("char", 0x4FF878, 0x668, 60),     # character pool, 60 slots
    ("charpos", 0x4FEFE8, 32, 60),     # per-character position records
    ("body", 0x5178D8, 0x180, 60),     # body pool behind character +0xb0 (+0x144 health)
    ("handle", 0x5A7570, 8, 0xB00),    # character handle table
    # Per-character slots (slot+0 = character pointer, slot+4 a state compared with 3; slot =
    # 0x599080 + i * 0x2d0 at 0x142914 and others); 16 is a guess from the slots seen in use.
    ("cslot", 0x599080, 0x2D0, 16),
]
KNOWN = {
    0x5825FC: "world step counter (FUN_0016d8d0)",
    0x45CC38: "world time step float (1/30, sim60 1/60)",
    0x4ACA1C: "game clock object pointer",
}


# ---------------------------------------------------------------------------- probe file I/O
class ProbeWriter:
    """Writes the --rate-probe format (same encoding rules as rateprobe.c). Used by tests."""

    def __init__(self, path, interval, flags, ranges):
        self.f = open(path, "wb")
        self.ranges = list(ranges)
        self.prev = [None] * len(self.ranges)
        self.f.write(struct.pack("<5I", MAGIC, VERSION, interval, flags, len(self.ranges)))
        for r in self.ranges:
            self.f.write(struct.pack("<4I", *r))

    def snapshot(self, frame, ms, datas, bases=None):
        self.f.write(struct.pack("<4I", SNAP, frame, ms, len(self.ranges)))
        for k, (addr, size, deref, off) in enumerate(self.ranges):
            data = datas[k]
            base = bases[k] if bases else addr
            if data is None:
                self.f.write(struct.pack("<3I", 0, KIND_NONE, 0))
                self.prev[k] = None
                continue
            cur = np.frombuffer(data, "<u4")
            prev = self.prev[k]
            if prev is not None and prev[0] == base:
                idx = np.nonzero(cur != prev[1])[0]
                if len(idx) * 8 < size:
                    pairs = np.empty((len(idx), 2), "<u4")
                    pairs[:, 0] = idx * 4
                    pairs[:, 1] = cur[idx]
                    self.f.write(struct.pack("<3I", base, KIND_DELTA, len(idx)) + pairs.tobytes())
                    self.prev[k] = (base, cur.copy())
                    continue
            self.f.write(struct.pack("<3I", base, KIND_FULL, size) + bytes(data))
            self.prev[k] = (base, cur.copy())

    def close(self):
        self.f.close()


def _read_header(f):
    magic, version, interval, flags, n = struct.unpack("<5I", f.read(20))
    if magic != MAGIC or version != VERSION:
        raise ValueError("not a rate-probe file (version %d)" % version)
    ranges = [struct.unpack("<4I", f.read(16)) for _ in range(n)]
    return {"interval_ms": interval, "flags": flags, "ranges": ranges}


def _snapshots(path, hdr, payload=True):
    with open(path, "rb") as f:
        _read_header(f)
        yield from _records(f, hdr, payload)


def _records(f, hdr, payload):
    while True:
        head = f.read(16)
        if len(head) < 16:
            return
        tag, frame, ms, n = struct.unpack("<4I", head)
        if tag != SNAP or n != len(hdr["ranges"]):
            raise ValueError("corrupt snapshot at byte %d" % (f.tell() - 16))
        recs = []
        for k in range(n):
            base, kind, count = struct.unpack("<3I", f.read(12))
            nbytes = count if kind == KIND_FULL else 8 * count if kind == KIND_DELTA else 0
            if payload:
                raw = f.read(nbytes)
                if kind == KIND_FULL:
                    data = np.frombuffer(raw, "<u4")
                elif kind == KIND_DELTA:
                    data = np.frombuffer(raw, "<u4").reshape(-1, 2)
                else:
                    data = None
            else:
                f.seek(nbytes, 1)
                data = None
            recs.append((base, kind, data))
        yield frame, ms, recs


def read_probe(path, payload=True):
    """Returns (header, iterator of (frame, game_ms, [(base, kind, data)] per range))."""
    with open(path, "rb") as f:
        hdr = _read_header(f)
    return hdr, _snapshots(path, hdr, payload)


# ---------------------------------------------------------------------------- accumulation
class RangeStats:
    def __init__(self, rng, chunks):
        self.addr, self.size, self.deref, self.off = rng
        n = self.size // 4
        self.abs = {k: np.zeros((chunks, n)) for k in KINDS}
        self.cnt = {k: np.zeros((chunks, n), np.int32) for k in KINDS}
        self.dur = np.zeros(chunks)
        self.fbad = np.zeros(n, bool)


class RunStats:
    def __init__(self, path, hdr, chunks):
        self.path, self.hdr = path, hdr
        self.sim60 = bool(hdr["flags"] & FLAG_SIM60)
        self.ranges = [RangeStats(r, chunks) for r in hdr["ranges"]]
        self.snapshots = self.pairs = 0
        self.frames = (None, None)
        self.game_s = 0.0


def _float_bad(vals):
    with np.errstate(invalid="ignore", over="ignore"):
        f = vals.view("<f4").astype(np.float64)
        a = np.abs(f)
        return ~np.isfinite(f) | (a > 1e8) | ((a != 0) & (a < 1e-20))


def _wrap(d, bits):
    m = 1 << bits
    return ((d + (m >> 1)) % m) - (m >> 1)


def _add(rs, k, idx, old, new):
    o, n = old.astype(np.int64), new.astype(np.int64)
    d = {
        "i32": np.abs(_wrap(n - o, 32)).astype(np.float64),
        "lo16": np.abs(_wrap((n & 0xFFFF) - (o & 0xFFFF), 16)).astype(np.float64),
        "hi16": np.abs(_wrap((n >> 16) - (o >> 16), 16)).astype(np.float64),
    }
    with np.errstate(invalid="ignore", over="ignore"):
        fo, fn = old.view("<f4").astype(np.float64), new.view("<f4").astype(np.float64)
        df = np.abs(fn - fo)
    df[~np.isfinite(df) | _float_bad(old) | _float_bad(new)] = 0.0
    d["f32"] = df
    for kind, v in d.items():
        rs.abs[kind][k, idx] += v
        rs.cnt[kind][k, idx] += (v != 0)


def accumulate(path, chunks=8, from_frame=0, to_frame=None, max_gap_ms=None):
    """Streams one probe file into per-chunk, per-field change totals (RunStats)."""
    hdr, it = read_probe(path, payload=False)
    gap = max_gap_ms or 5 * hdr["interval_ms"]
    inwin = lambda fr: fr >= from_frame and (to_frame is None or fr <= to_frame)
    # Pass 1: valid pairs (both in the window, game time advanced by at most the gap) and the
    # chunk boundaries in game time.
    dts, last = [], None
    for frame, ms, _ in it:
        if inwin(frame):
            if last is not None and 0 < ms - last <= gap:
                dts.append(ms - last)
            last = ms
        else:
            last = None
    run = RunStats(path, hdr, chunks)
    total = float(sum(dts))
    run.game_s, run.pairs = total / 1000.0, len(dts)
    cum = np.cumsum(dts) if dts else np.zeros(0)
    pair_chunk = np.minimum((((cum - np.asarray(dts, float) / 2) / total) * chunks).astype(int), chunks - 1) if dts else []
    # Pass 2: field changes per valid pair.
    hdr, it = read_probe(path)
    state = [None] * len(run.ranges)   # (base, values) or None
    last, p, first_fr, last_fr = None, 0, None, None
    for frame, ms, recs in it:
        run.snapshots += 1
        ok = False
        if inwin(frame):
            ok = last is not None and 0 < ms - last <= gap
            last = ms
            first_fr = frame if first_fr is None else first_fr
            last_fr = frame
        else:
            last = None
        k = pair_chunk[p] if ok else -1
        for ri, (base, kind, data) in enumerate(recs):
            rs, st = run.ranges[ri], state[ri]
            same = st is not None and st[0] == base
            if kind == KIND_NONE:
                state[ri] = None
                continue
            if kind == KIND_FULL:
                new = data.copy()
                rs.fbad |= _float_bad(new)
                if ok and same:
                    idx = np.nonzero(new != st[1])[0]
                    _add(rs, k, idx, st[1][idx], new[idx])
                state[ri] = (base, new)
            else:
                if st is None:
                    continue
                idx, vals = data[:, 0] // 4, data[:, 1].copy()
                rs.fbad[idx] |= _float_bad(vals)
                if ok and same:
                    _add(rs, k, idx, st[1][idx], vals)
                st[1][idx] = vals
            if ok and same:
                rs.dur[k] += dts[p] / 1000.0
        if ok:
            p += 1
    run.frames = (first_fr, last_fr)
    return run


# ---------------------------------------------------------------------------- comparison
def compare(b, s, min_changes=20, band2=(1.6, 2.5), bandh=(0.4, 0.625), agree=0.75, min_chunks=3):
    """Fields whose sim60/default rate ratio is about 2 or 0.5, consistently across chunks."""
    out = []
    sranges = {tuple((r.addr, r.size, r.deref, r.off)): r for r in s.ranges}
    for rb in b.ranges:
        rs = sranges.get((rb.addr, rb.size, rb.deref, rb.off))
        if rs is None or rb.dur.sum() <= 0 or rs.dur.sum() <= 0:
            continue
        isfloat = ~(rb.fbad | rs.fbad)
        flagged32 = np.zeros(rb.size // 4, bool)
        for kind in ("i32", "f32", "lo16", "hi16"):
            ab, as_ = rb.abs[kind], rs.abs[kind]
            cb, cs = rb.cnt[kind].sum(0), rs.cnt[kind].sum(0)
            rate_b, rate_s = ab.sum(0) / rb.dur.sum(), as_.sum(0) / rs.dur.sum()
            ok = (cb >= min_changes) & (cs >= min_changes) & (rate_b > 0)
            ok &= isfloat if kind == "f32" else ~isfloat
            if kind in ("lo16", "hi16"):
                ok &= ~flagged32
            with np.errstate(divide="ignore", invalid="ignore"):
                ratio = np.where(ok, rate_s / np.where(rate_b > 0, rate_b, 1), 0)
            cand = ok & (((ratio >= band2[0]) & (ratio <= band2[1])) | ((ratio >= bandh[0]) & (ratio <= bandh[1])))
            for i in np.nonzero(cand)[0]:
                band = "2x" if ratio[i] >= 1 else "0.5x"
                lo, hi = band2 if band == "2x" else bandh
                with np.errstate(divide="ignore", invalid="ignore"):
                    crb = ab[:, i] / np.where(rb.dur > 0, rb.dur, np.nan)
                    crs = as_[:, i] / np.where(rs.dur > 0, rs.dur, np.nan)
                    cr = crs / crb
                active = (rb.cnt[kind][:, i] > 0) | (rs.cnt[kind][:, i] > 0)
                both = (rb.cnt[kind][:, i] > 0) & (rs.cnt[kind][:, i] > 0) & np.isfinite(cr)
                inband = both & (cr >= lo) & (cr <= hi)
                if both.sum() < min_chunks or inband.sum() < agree * active.sum():
                    continue
                if kind in ("i32", "f32"):
                    flagged32[i] = True
                lg = np.log2(cr[both])
                field = 4 * int(i) + (2 if kind == "hi16" else 0)
                out.append({
                    "addr": (rb.addr + field) if not rb.deref else None,
                    "range": (rb.addr, rb.deref, rb.off), "field": field, "kind": kind, "band": band,
                    "ratio": float(ratio[i]), "rate_base": float(rate_b[i]), "rate_sim": float(rate_s[i]),
                    "agree": float(inband.sum()) / max(1, active.sum()), "chunks": int(both.sum()),
                    "spread": float(lg.std()) if len(lg) else 0.0,
                    "changes_base": int(cb[i]), "changes_sim": int(cs[i]),
                })
    return out


# ---------------------------------------------------------------------------- disassembly
Ref = collections.namedtuple("Ref", "addr write text")
_LINE = re.compile(r"^\s*([0-9a-f]+):\t[0-9a-f ]+\t(.+)$")
_MEM = re.compile(r"^\*?(?:%([a-z]s):)?(-?0x[0-9a-f]+|-?\d+)?(?:\((%\w+)?(?:,(%\w+))?(?:,(\d))?\))?$")
_PREFIX = {"lock", "rep", "repz", "repe", "repnz", "repne", "data16", "addr16", "notrack", "bnd"}
_READS = re.compile(r"^(cmp|test|bt|push|call|jmp|j[a-z]+|loop\w*|lea|fld|fild|fbld|fcom|fucom|ficom|fadd|fsub|fmul|"
                    r"fdiv|fiadd|fisub|fimul|fidiv|fldcw|fldenv|frstor|ucomis|comis|mul|div|idiv|imul|prefetch|nop|"
                    r"cvt|movzb|movzw|movsb|movsw|movsx|movzx|bound|ptest|pcmp|cmov)")


def _split_ops(s):
    ops, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            ops.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        ops.append(cur.strip())
    return ops


class DisasmIndex:
    """Memory operands of an objdump (AT&T) listing: absolute ones by address, register-based
    ones by displacement, and register-indexed ones with an absolute-looking displacement."""

    def __init__(self):
        self.absolute = collections.defaultdict(list)
        self.disp = collections.defaultdict(list)
        self.indexed = []   # (disp, Ref), sorted after load

    @classmethod
    def from_text(cls, text):
        idx = cls()
        for line in text.splitlines():
            idx._line(line)
        idx.indexed.sort(key=lambda t: t[0])
        return idx

    @classmethod
    def from_file(cls, path):
        idx = cls()
        with open(path, errors="replace") as f:
            for line in f:
                idx._line(line.rstrip("\n"))
        idx.indexed.sort(key=lambda t: t[0])
        return idx

    def _line(self, line):
        m = _LINE.match(line)
        if not m:
            return
        addr, ins = int(m.group(1), 16), m.group(2).split("#")[0].strip()
        parts = ins.split(None, 1)
        while parts and parts[0] in _PREFIX:
            parts = parts[1].split(None, 1) if len(parts) > 1 else []
        if not parts or len(parts) < 2:
            return
        mn, ops = parts[0], _split_ops(parts[1])
        branch = mn.startswith(("j", "call", "loop"))
        for pos, op in enumerate(ops):
            if op.startswith(("$", "%")) or (branch and not op.startswith("*")):
                continue
            mm = _MEM.match(op)
            if not mm or (mm.group(2) is None and mm.group(3) is None and mm.group(4) is None):
                continue
            if mm.group(1) in ("fs", "gs"):
                continue
            disp = int(mm.group(2), 0) & 0xFFFFFFFF if mm.group(2) else 0
            last = pos == len(ops) - 1
            write = (last and not _READS.match(mn)) or mn.startswith(("xchg", "xadd", "cmpxchg"))
            ref = Ref(addr, write, "%x: %s" % (addr, ins))
            if mm.group(3) is None and mm.group(4) is None:
                self.absolute[disp].append(ref)
            elif mm.group(3) != "%esp":   # stack locals are never guest globals or object fields
                self.disp[disp].append(ref)
                if disp >= 0x1000:
                    self.indexed.append((disp, ref))

    def refs(self, addr, size=4):
        """Instructions with an absolute operand inside addr..addr+size."""
        out = []
        for a in range(addr - 3, addr + size):   # a 4-byte access at addr-3.. overlaps the field
            if a >= addr or a + 4 > addr:
                out.extend(self.absolute.get(a, []))
        return sorted(set(out))

    def disp_refs(self, disp):
        return sorted(self.disp.get(disp, []))

    def disp_writes(self, disp):
        return [r for r in self.disp_refs(disp) if r.write]

    def indexed_refs(self, addr, span=0x1000):
        """Register-indexed operands whose displacement lies within span below addr (arrays)."""
        lo = bisect.bisect_left(self.indexed, addr - span + 1, key=lambda t: t[0]) if sys.version_info >= (3, 10) else 0
        return sorted(r for d, r in self.indexed[lo:] if addr - span < d <= addr)


class Closure:
    """Functions of the world-update closure: 'start_hex size_decimal' per line."""

    def __init__(self, funcs):
        self.funcs = sorted(funcs)
        self.starts = [f[0] for f in self.funcs]

    @classmethod
    def from_text(cls, text):
        funcs = []
        for line in text.splitlines():
            p = line.split()
            if len(p) >= 2:
                funcs.append((int(p[0], 16), int(p[1])))
        return cls(funcs)

    def func_of(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and addr < self.funcs[i][0] + self.funcs[i][1]:
            return self.funcs[i][0]
        return None

    def contains(self, addr):
        return self.func_of(addr) is not None


def disasm_from_xbe(xbe, out):
    """objdump of the XBE's .text section at its virtual address (AT&T syntax)."""
    data = open(xbe, "rb").read()
    base, = struct.unpack_from("<I", data, 0x104)
    nsec, hdrs = struct.unpack_from("<II", data, 0x11C)
    for i in range(nsec):
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", data, hdrs - base + 56 * i)
        name = data[name_va - base:data.index(b"\0", name_va - base)]
        if name == b".text":
            with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as t:
                t.write(data[raw:raw + rsize])
            with open(out, "w") as f:
                subprocess.run(["objdump", "-D", "-b", "binary", "-mi386", "--adjust-vma=0x%x" % va, t.name],
                               stdout=f, check=True)
            os.unlink(t.name)
            return out
    raise ValueError("no .text section in %s" % xbe)


# ---------------------------------------------------------------------------- report
def label(f):
    if f["addr"] is None:
        r = f["range"]
        return "[0x%x]+0x%x" % (r[0], r[2] + f["field"]), None, None
    a = f["addr"]
    for name, base, stride, count in STRUCTS:
        if base <= a < base + stride * count:
            i, off = divmod(a - base, stride)
            return "%s[%d]+0x%x" % (name, i, off), name, off
    return "0x%08x" % a, None, None


def group(findings):
    groups = collections.OrderedDict()
    for f in sorted(findings, key=lambda f: (f["addr"] or 0, f["kind"])):
        text, sname, off = label(f)
        if sname:
            key = (sname, off, f["kind"])
        else:
            key = None
            last = next(reversed(groups)) if groups else None
            if last and last[0] == "addr" and f["addr"] is not None and f["addr"] - groups[last][-1]["addr"] <= 16:
                key = last
            key = key or ("addr", f["addr"] if f["addr"] is not None else text, f["kind"])
        groups.setdefault(key, []).append(f)
    return groups


def _writers(key, members, idx, closure, limit=6):
    if idx is None:
        return [], 0
    refs = []
    if key[0] == "addr":
        for f in members:
            if f["addr"] is not None:
                refs += idx.refs(f["addr"], 2 if f["kind"] in ("lo16", "hi16") else 4)
                refs += idx.indexed_refs(f["addr"])
            else:
                refs += idx.disp_refs(f["range"][2] + f["field"])
    else:
        sname, off = key[0], key[1]
        base = next(b for n, b, _, _ in STRUCTS if n == sname)
        refs += idx.disp_refs(off) + idx.disp_refs(base + off)
        for f in members[:2]:
            refs += idx.refs(f["addr"])
    refs = sorted(set(refs))
    inside = lambda r: closure is not None and closure.contains(r.addr)
    refs.sort(key=lambda r: (not r.write, not inside(r), r.addr))
    shown = [("%s%s%s" % ("W " if r.write else "R ", "*" if inside(r) else "", r.text)) for r in refs[:limit]]
    return shown, len(refs)


def write_report(findings, b, s, prefix, idx, closure, top=80):
    os.makedirs(os.path.dirname(os.path.abspath(prefix)), exist_ok=True)
    md, cv = prefix + ".md", prefix + ".csv"
    with open(cv, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["label", "addr", "field", "kind", "band", "ratio", "rate_base", "rate_sim", "agree", "chunks",
                    "spread", "changes_base", "changes_sim"])
        for f in sorted(findings, key=lambda f: (f["addr"] or 0, f["kind"])):
            w.writerow([label(f)[0], "0x%08x" % f["addr"] if f["addr"] is not None else "", f["field"], f["kind"],
                        f["band"], "%.3f" % f["ratio"], "%.4g" % f["rate_base"], "%.4g" % f["rate_sim"],
                        "%.2f" % f["agree"], f["chunks"], "%.3f" % f["spread"], f["changes_base"], f["changes_sim"]])
    groups = group(findings)

    def score(item):
        key, mem = item
        r = float(np.median([m["ratio"] for m in mem]))
        target = 1.0 if r >= 1 else -1.0
        return (-min(m["agree"] for m in mem), abs(np.log2(r) - target), -len(mem))

    ranked = sorted(groups.items(), key=score)
    with open(md, "w") as fh:
        fh.write("# Rate diff report\n\n")
        for name, r in (("default", b), ("sim60", s)):
            fh.write("- %s: `%s` (flags sim60=%d), %d snapshots, window frames %s-%s, %.1f s of game time in %d pairs\n"
                     % (name, r.path, r.sim60, r.snapshots, r.frames[0], r.frames[1], r.game_s, r.pairs))
        n2 = sum(1 for f in findings if f["band"] == "2x")
        fh.write("- flagged fields: %d at about 2x, %d at about 0.5x, in %d groups\n" % (n2, len(findings) - n2, len(groups)))
        fh.write("- writers: `W` write, `R` read; `*` inside the world-update closure\n\n")
        fh.write("| # | location | kind | n | ratio | agree | rate default/s | rate sim60/s | changes | name | references |\n")
        fh.write("|---|---|---|---|---|---|---|---|---|---|---|\n")
        for rank, (key, mem) in enumerate(ranked[:top], 1):
            first = mem[0]
            loc = label(first)[0] if key[0] == "addr" else "%s[*]+0x%x" % (key[0], key[1])
            if key[0] == "addr" and len(mem) > 1:
                loc += " .. +0x%x" % ((mem[-1]["addr"] or 0) - (first["addr"] or 0))
            ratio = float(np.median([m["ratio"] for m in mem]))
            shown, total = _writers(key, mem, idx, closure)
            name = KNOWN.get(first["addr"], "") if first["addr"] is not None else ""
            refs = "<br>".join("`%s`" % t for t in shown) + (" (+%d)" % (total - len(shown)) if total > len(shown) else "")
            fh.write("| %d | %s | %s | %d | %.2f | %.2f | %.4g | %.4g | %d/%d | %s | %s |\n" % (
                rank, loc, first["kind"], len(mem), ratio, min(m["agree"] for m in mem),
                float(np.median([m["rate_base"] for m in mem])), float(np.median([m["rate_sim"] for m in mem])),
                sum(m["changes_base"] for m in mem), sum(m["changes_sim"] for m in mem), name, refs or "-"))
    return md, cv


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base")
    ap.add_argument("sim")
    ap.add_argument("--from-frame", type=int, default=0)
    ap.add_argument("--to-frame", type=int)
    ap.add_argument("--chunks", type=int, default=8)
    ap.add_argument("--min-changes", type=int, default=20)
    ap.add_argument("--agree", type=float, default=0.75)
    ap.add_argument("--disasm", help="objdump listing of .text (default: generated from --xbe)")
    ap.add_argument("--xbe")
    ap.add_argument("--closure", help="world-update closure: 'start_hex size' per line")
    ap.add_argument("--out", default=os.path.expanduser("~/.cache/rate-diff/report"))
    ap.add_argument("--top", type=int, default=80)
    a = ap.parse_args(argv)
    b = accumulate(a.base, a.chunks, a.from_frame, a.to_frame)
    s = accumulate(a.sim, a.chunks, a.from_frame, a.to_frame)
    if b.sim60 or not s.sim60:
        print("warning: expected BASE without and SIM with --sim60 (flags %d/%d)" % (b.sim60, s.sim60))
    findings = compare(b, s, a.min_changes, agree=a.agree)
    disasm = a.disasm
    if not disasm and a.xbe:
        disasm = disasm_from_xbe(a.xbe, a.out + ".text.s")
    idx = DisasmIndex.from_file(disasm) if disasm else None
    closure = Closure.from_text(open(a.closure).read()) if a.closure else None
    md, cv = write_report(findings, b, s, a.out, idx, closure, a.top)
    print("%d flagged fields; report %s, %s" % (len(findings), md, cv))


if __name__ == "__main__":
    main()
