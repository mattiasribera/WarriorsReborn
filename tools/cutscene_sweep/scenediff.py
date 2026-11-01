#!/usr/bin/env python3
"""Scene diff: compare one scripted scene run at 30 Hz with the same scene under --sim60,
keyed by game time (the title ms clock, which restarts at the level start).

Input: two --rate-probe files (runtime/src/rateprobe.h) of the same chapter start (sweep.py).
Per run the segment after the level start (the first clock restart that has characters) is
decoded into time series: per character slot whether its handle exists, its position record,
its state word (character +0x190, the state function), the scene camera's view matrix and the
script timer list length. Runs are close but not frame-identical, so the comparison works on
rates and event times, not on single values:
  - fast / slow: a character's path length in a window of game time is about 2x (0.5x);
  - cam: the same for the camera matrix;
  - spawn / despawn: a slot appears (disappears) at another game time; ratio sim/base given;
  - state2x: the slot's state changes happen at about half (twice) the game time;
  - diverge: the same slot is more than DIVERGE units apart for at least a second;
  - timers: the script timer list fires at other game times.
Optionally the per-field rate diff (tools/ratediff) runs over the same segments and lists
fields of the .data section, the camera and the body objects that change at about 2x.
"""
import collections
import concurrent.futures
import csv
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "ratediff"))
import ratediff  # noqa: E402

Layout = collections.namedtuple(
    "Layout", "handles handle_stride pos pos_stride pool pool_stride state_off slots cam_ptr cam_off timer_ptr")
# The Warriors: handle table 0x5a7570 (8 bytes,
# first dword the character), position records 0x4fefe8 (32 bytes), character pool 0x4ff878
# (0x668 bytes; +0x190 the state function), scene view matrix [[0x4cf13c]+0x1c], the script
# object [0x5a7528] (+0xc/+0x10: begin/end of the timer heap, 0x18 bytes per entry, due at
# the title ms clock, FUN_00292110).
WARRIORS = Layout(0x5A7570, 8, 0x4FEFE8, 32, 0x4FF878, 0x668, 0x190, 32, 0x4CF13C, 0x1C, 0x5A7528)

Finding = collections.namedtuple("Finding", "kind slot base_ms sim_ms ratio detail")

WINDOW_MS = 2000
MIN_MOTION = 0.5        # units per window before a ratio counts
BAND2, BANDH = (1.6, 2.5), (0.4, 0.625)
EVENT_MS = 500          # event time difference that counts
DIVERGE, DIVERGE_MS = 2.0, 1000


class Series:
    pass


def _segments(path):
    """Yields lists of (frame, ms, [range arrays]) split at clock restarts, decoding deltas."""
    hdr, it = ratediff.read_probe(path)
    cur = [None] * len(hdr["ranges"])
    seg, last = [], None
    for frame, ms, recs in it:
        for k, (base, kind, data) in enumerate(recs):
            if kind == ratediff.KIND_FULL:
                cur[k] = data.copy()
            elif kind == ratediff.KIND_DELTA and cur[k] is not None:
                cur[k][data[:, 0] // 4] = data[:, 1]
            else:
                cur[k] = None
        if last is not None and ms < last:
            yield hdr, seg
            seg = []
        last = ms
        seg.append((frame, ms, [None if c is None else c.copy() for c in cur]))
    yield hdr, seg


def _range_index(hdr, addr, deref):
    for k, (a, size, d, off) in enumerate(hdr["ranges"]):
        if a == addr and bool(d) == deref:
            return k
    return None


def load_series(path, layout=WARRIORS):
    """The first segment after a clock restart in which any character handle exists."""
    for hdr, seg in _segments(path):
        data = [k for k, (a, size, d, off) in enumerate(hdr["ranges"]) if not d][0]
        daddr = hdr["ranges"][data][0]
        cam = _range_index(hdr, layout.cam_ptr, True)
        tim = _range_index(hdr, layout.timer_ptr, True)
        S = layout.slots
        hidx = (layout.handles - daddr) // 4 + np.arange(S) * (layout.handle_stride // 4)
        pidx = (layout.pos - daddr) // 4 + np.arange(S)[:, None] * (layout.pos_stride // 4) + np.arange(3)
        sidx = (layout.pool + layout.state_off - daddr) // 4 + np.arange(S) * (layout.pool_stride // 4)
        if not seg or not any(r[data] is not None and r[data][hidx].any() for _, _, r in seg):
            continue
        s = Series()
        s.path, s.frames = path, np.array([f for f, _, _ in seg])
        s.t = np.array([ms for _, ms, _ in seg], dtype=np.int64)
        s.start_frame = int(s.frames[0])
        n = len(seg)
        s.alive = np.zeros((n, S), bool)
        s.pos = np.zeros((n, S, 3), np.float32)
        s.state = np.zeros((n, S), np.uint32)
        s.cam = np.zeros((n, 16), np.float32)
        s.timers = np.zeros(n, np.int64)
        for i, (_, _, r) in enumerate(seg):
            d = r[data]
            if d is None:
                continue
            s.alive[i] = d[hidx] != 0
            s.pos[i] = d[pidx].view(np.float32)
            s.state[i] = d[sidx]
            if cam is not None and r[cam] is not None:
                s.cam[i] = r[cam][:16].view(np.float32)
            if tim is not None and r[tim] is not None:
                b, e = int(r[tim][3]), int(r[tim][4])
                s.timers[i] = (e - b) // 0x18 if e >= b else 0
        s.pos[~np.isfinite(s.pos)] = 0
        s.cam[~np.isfinite(s.cam)] = 0
        return s
    raise ValueError("%s: no level segment with characters" % path)


def _path_len(s, slot, t0, t1):
    m = (s.t >= t0) & (s.t < t1) & s.alive[:, slot]
    q = s.pos[m, slot].astype(np.float64)
    if len(q) < 2:
        return 0.0
    d = np.linalg.norm(np.diff(q, axis=0), axis=1)
    return float(d[d < 20].sum())   # a jump of 20+ units in 50 ms is a placement, not motion


def motion_ratio(b, s, slot, t0, t1):
    a, c = _path_len(b, slot, t0, t1), _path_len(s, slot, t0, t1)
    return c / a if a > 0 else float("inf") if c > 0 else 1.0


def _cam_motion(s, t0, t1):
    m = (s.t >= t0) & (s.t < t1)
    c = s.cam[m].astype(np.float64)
    if len(c) < 2:
        return 0.0
    d = np.abs(np.diff(c, axis=0)).sum(axis=1)
    return float(d[d < 50].sum())   # cuts are jumps


def _in(r, band):
    return band[0] <= r <= band[1]


def _first(mask):
    i = np.flatnonzero(mask)
    return int(i[0]) if len(i) else None


def _events(s, slot):
    """(ms, value) of each state change of a slot while it exists."""
    st, al = s.state[:, slot], s.alive[:, slot]
    ch = np.flatnonzero((st[1:] != st[:-1]) & al[1:] & al[:-1]) + 1
    return [(int(s.t[i]), int(st[i])) for i in ch]


def timer_events(s):
    """Game ms at which the timer list got shorter (timers fired)."""
    d = np.diff(s.timers)
    out = []
    for i in np.flatnonzero(d < 0):
        out += [int(s.t[i + 1])] * int(-d[i])
    return out


JUMP = 2.0              # a placement: more than JUMP units in one sample


def jumps(s, slot):
    """(ms, destination) of each placement (a jump between two samples) of a slot."""
    al = s.alive[:, slot]
    d = np.linalg.norm(np.diff(s.pos[:, slot], axis=0), axis=1)
    idx = np.flatnonzero((d > JUMP) & al[1:] & al[:-1]) + 1
    return [(int(s.t[i]), s.pos[i, slot].copy()) for i in idx]


def match_jumps(b, s, slot, tol=0.5):
    """Pairs placements of a slot that land on the same spot (within tol units), in order."""
    js = jumps(s, slot)
    used = set()
    out = []
    for tb, pb in jumps(b, slot):
        for k, (ts, ps) in enumerate(js):
            if k not in used and float(np.linalg.norm(pb - ps)) <= tol:
                used.add(k)
                out.append((tb, ts))
                break
    return out


def schedule(b, s):
    """Scene clock check from scripted placements: (pairs, median offset ms, fitted ratio sim/base)."""
    pairs = []
    for slot in range(b.alive.shape[1]):
        pairs += match_jumps(b, s, slot)
    pairs = sorted(p for p in pairs if p[0] > EVENT_MS)
    if not pairs:
        return 0, None, None
    tb = np.array([p[0] for p in pairs], float)
    ts = np.array([p[1] for p in pairs], float)
    off = float(np.median(ts - tb))
    ratio = float(np.median(ts / tb))
    return len(pairs), off, ratio


def _match_events(eb, es):
    """Pairs the k-th occurrence of each value in both event lists."""
    seen_b, seen_s, idx = collections.Counter(), collections.Counter(), {}
    for t, v in es:
        idx[(v, seen_s[v])] = t
        seen_s[v] += 1
    out = []
    for t, v in eb:
        k = (v, seen_b[v])
        seen_b[v] += 1
        if k in idx:
            out.append((t, idx[k]))
    return out


def compare(b, s, window=WINDOW_MS):
    out = []
    end = int(min(b.t[-1], s.t[-1]))
    S = b.alive.shape[1]
    for slot in range(S):
        if not (b.alive[:, slot].any() or s.alive[:, slot].any()):
            continue
        # existence: first appearance and disappearance
        for kind, mb, ms_ in (("spawn", b.alive[:, slot], s.alive[:, slot]),
                              ("despawn", ~b.alive[:, slot] & np.maximum.accumulate(b.alive[:, slot]),
                               ~s.alive[:, slot] & np.maximum.accumulate(s.alive[:, slot]))):
            ib, is_ = _first(mb), _first(ms_)
            if ib is None and is_ is None:
                continue
            tb = int(b.t[ib]) if ib is not None else None
            ts = int(s.t[is_]) if is_ is not None else None
            if kind == "spawn" and tb == 0 and ts == 0:
                continue
            if tb is None or ts is None:
                if (tb or ts) < end:
                    out.append(Finding(kind, slot, tb, ts, None, "only in one run"))
            elif abs(ts - tb) > EVENT_MS:
                out.append(Finding(kind, slot, tb, ts, ts / tb if tb else None, ""))
        # motion per window
        hits = {"fast": [], "slow": []}
        moving = 0
        for w0 in range(0, end, window):
            a, c = _path_len(b, slot, w0, w0 + window), _path_len(s, slot, w0, w0 + window)
            if min(a, c) < MIN_MOTION:
                continue
            moving += 1
            r = c / a
            if _in(r, BAND2):
                hits["fast"].append((w0, r))
            elif _in(r, BANDH):
                hits["slow"].append((w0, r))
        for kind, h in hits.items():
            if h:
                out.append(Finding(kind, slot, h[0][0], h[0][0], round(float(np.median([r for _, r in h])), 2),
                                   "%d of %d moving windows" % (len(h), moving)))
        # state changes
        pairs = [(tb, ts) for tb, ts in _match_events(_events(b, slot), _events(s, slot)) if tb > EVENT_MS]
        if pairs:
            r = np.median([ts / tb for tb, ts in pairs])
            off = [abs(ts - tb) for tb, ts in pairs]
            if len(pairs) >= 2 and (_in(r, BANDH) or _in(r, BAND2)) and np.median(off) > EVENT_MS:
                out.append(Finding("state2x", slot, pairs[0][0], pairs[0][1], round(float(r), 2),
                                   "%d changes" % len(pairs)))
        # divergence at equal game time
        both = b.alive[:, slot]
        js = np.clip(np.searchsorted(s.t, b.t), 0, len(s.t) - 1)
        ok = both & s.alive[js, slot] & (np.abs(s.t[js] - b.t) <= 60)
        dist = np.where(ok, np.linalg.norm(b.pos[:, slot] - s.pos[js, slot], axis=1), 0)
        far = dist > DIVERGE
        run = 0
        for i in range(len(far)):
            run = run + 1 if far[i] else 0
            if run and b.t[i] - b.t[i - run + 1] >= DIVERGE_MS:
                t0 = int(b.t[i - run + 1])
                out.append(Finding("diverge", slot, t0, t0, None, "%.1f units at %d ms" % (dist[i], b.t[i])))
                break
    # scene clock from placements
    n, off, ratio = schedule(b, s)
    if n >= 2 and (_in(ratio, BANDH) or _in(ratio, BAND2)):
        out.append(Finding("schedule2x", None, None, None, round(ratio, 2), "%d placements, median offset %d ms" % (n, off)))
    elif n >= 2 and abs(off) > 2 * EVENT_MS:
        out.append(Finding("schedule", None, None, None, round(ratio, 2), "%d placements, median offset %d ms" % (n, off)))
    # camera
    worst, n = None, 0
    for w0 in range(0, end, window):
        a, c = _cam_motion(b, w0, w0 + window), _cam_motion(s, w0, w0 + window)
        if min(a, c) < MIN_MOTION:
            continue
        r = c / a
        if _in(r, BAND2) or _in(r, BANDH):
            n += 1
            worst = worst or (w0, r)
    if n:
        out.append(Finding("cam", None, worst[0], worst[0], round(worst[1], 2), "%d windows" % n))
    # script timers
    eb, es = timer_events(b), timer_events(s)
    pairs = list(zip(eb, es))
    bad = [(x, y) for x, y in pairs if abs(x - y) > EVENT_MS and x < end and y < end]
    if bad:
        out.append(Finding("timers", None, bad[0][0], bad[0][1], round(bad[0][1] / bad[0][0], 2) if bad[0][0] else None,
                           "%d of %d firings off" % (len(bad), len(pairs))))
    return out


def field_label(f, layout=WARRIORS):
    """ratediff finding -> a label: obj<OFF>[slot]+off for an object behind character +OFF."""
    if f["addr"] is None:
        ptr, _, off = f["range"]
        k, r = divmod(ptr - layout.pool, layout.pool_stride)
        if 0 <= k < 60 and r in (0xB0, 0xB4, 0x170):
            return "obj%x[%d]+0x%x" % (r, k, off + f["field"])
        if ptr == layout.cam_ptr:
            return "cam+0x%x" % (off + f["field"])
    return ratediff.label(f)[0]


def rate_rows(pb, ps, b, s):
    """Per-field rate diff over both level segments, grouped by label without the slot index."""
    rb = ratediff.accumulate(pb, 8, b.start_frame, int(b.frames[-1]))
    rs = ratediff.accumulate(ps, 8, s.start_frame, int(s.frames[-1]))
    groups = collections.OrderedDict()
    for f in ratediff.compare(rb, rs):
        lab = field_label(f)
        key = (re.sub(r"\[\d+\]", "[*]", lab), f["kind"])
        groups.setdefault(key, []).append((lab, f))
    rows = []
    for (key, kind), members in groups.items():
        r = np.median([f["ratio"] for _, f in members])
        rows.append({"field": key, "kind": kind, "n": len(members), "ratio": round(float(r), 2),
                     "rate_base": round(float(np.median([f["rate_base"] for _, f in members])), 2),
                     "rate_sim": round(float(np.median([f["rate_sim"] for _, f in members])), 2),
                     "examples": " ".join(l for l, _ in members[:4])})
    return merge_strided(rows)


def merge_strided(rows):
    """Collapses runs of plain-address rows (same kind, ratio within 0.05, constant stride)."""
    out = []
    for r in rows:
        m = re.fullmatch(r"0x([0-9a-f]+)", r["field"])
        p = out[-1] if out else None
        if m and p and p.get("_addr") is not None and p["kind"] == r["kind"] and abs(p["ratio"] - r["ratio"]) <= 0.05:
            a = int(m.group(1), 16)
            stride = a - p["_last"]
            if p["_stride"] in (None, stride) and 0 < stride <= 0x1000:
                p["_stride"], p["_last"], p["n"] = stride, a, p["n"] + 1
                p["field"] = "0x%08x + k*0x%x" % (p["_addr"], stride)
                continue
        r = dict(r)
        r["_addr"] = int(m.group(1), 16) if m else None
        r["_last"], r["_stride"] = r["_addr"], None
        out.append(r)
    for r in out:
        for k in ("_addr", "_last", "_stride"):
            r.pop(k)
    return out


def _fmt_ms(v):
    return "-" if v is None else "%.1f" % (v / 1000.0)


def write_report(scenes, prefix):
    """scenes: [(name, base Series, sim Series, findings, rate rows)]."""
    with open(prefix + ".csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scene", "kind", "slot", "base_s", "sim_s", "ratio", "detail"])
        for name, b, s, fs, _ in scenes:
            for x in fs:
                w.writerow([name, x.kind, "" if x.slot is None else x.slot, _fmt_ms(x.base_ms), _fmt_ms(x.sim_ms),
                            "" if x.ratio is None else x.ratio, x.detail])
    kinds = collections.Counter(x.kind for _, _, _, fs, _ in scenes for x in fs)
    scenes_by_kind = collections.defaultdict(set)
    for name, _, _, fs, _ in scenes:
        for x in fs:
            scenes_by_kind[x.kind].add(name)
    with open(prefix + ".md", "w") as f:
        f.write("# Cutscene sweep report\n\n%d scenes compared (30 Hz vs --sim60, keyed by game time since the level start).\n\n"
                % len(scenes))
        f.write("| kind | findings | scenes |\n|---|---|---|\n")
        for k, n in kinds.most_common():
            f.write("| %s | %d | %d |\n" % (k, n, len(scenes_by_kind[k])))
        fields = collections.defaultdict(set)
        for name, _, _, _, rates in scenes:
            for r in rates:
                fields[(r["field"], r["kind"])].add(name)
        if fields:
            f.write("\nRate-diff fields by number of scenes:\n\n| field | kind | scenes |\n|---|---|---|\n")
            for (fl, kd), ns in sorted(fields.items(), key=lambda x: -len(x[1]))[:25]:
                f.write("| %s | %s | %d |\n" % (fl, kd, len(ns)))
        for name, b, s, fs, rates in scenes:
            f.write("\n## %s\n\n- base %.1f s (%d samples, %d characters), sim60 %.1f s (%d samples, %d characters)\n"
                    % (name, b.t[-1] / 1000.0, len(b.t), int(b.alive.any(0).sum()), s.t[-1] / 1000.0, len(s.t),
                       int(s.alive.any(0).sum())))
            n, off, ratio = schedule(b, s)
            if n:
                f.write("- scene clock from %d matched placements: median offset %+d ms, median time ratio %.3f\n"
                        % (n, off, ratio))
            if fs:
                f.write("\n| kind | slot | base s | sim60 s | ratio | detail |\n|---|---|---|---|---|---|\n")
                for x in fs:
                    f.write("| %s | %s | %s | %s | %s | %s |\n" % (
                        x.kind, "" if x.slot is None else x.slot, _fmt_ms(x.base_ms), _fmt_ms(x.sim_ms),
                        "" if x.ratio is None else x.ratio, x.detail))
            else:
                f.write("- no findings\n")
            if rates:
                f.write("\nFields at about 2x (rate diff):\n\n")
                f.write("| field | kind | slots | ratio | default/s | sim60/s | examples |\n|---|---|---|---|---|---|---|\n")
                for r in rates:
                    f.write("| %(field)s | %(kind)s | %(n)d | %(ratio).2f | %(rate_base)s | %(rate_sim)s | %(examples)s |\n" % r)


def _one(item):
    name, pb, ps, rates = item
    try:
        b, s = load_series(pb), load_series(ps)
    except (ValueError, OSError) as e:
        return name, None, None, [Finding("unreadable", None, None, None, None, str(e))], []
    fs = compare(b, s)
    return name, b, s, fs, rate_rows(pb, ps, b, s) if rates else []


def sweep_report(pairs, prefix, rates=True, disasm=None, closure=None, jobs=4):
    items = [(n, b, s, rates) for n, b, s in pairs]
    with concurrent.futures.ProcessPoolExecutor(jobs) as ex:
        res = list(ex.map(_one, items))
    ok = [r for r in res if r[1] is not None]
    bad = [r for r in res if r[1] is None]
    write_report(ok, prefix)
    if bad:
        with open(prefix + ".md", "a") as f:
            f.write("\n## Not compared\n\n")
            for name, _, _, fs, _ in bad:
                f.write("- %s: %s\n" % (name, fs[0].detail))
    return res


def main(argv=None):
    import argparse
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("base")
    p.add_argument("sim")
    p.add_argument("--out", default=os.path.join(os.path.expanduser("~"), ".cache", "cutscene-sweep", "scene"))
    p.add_argument("--no-rates", action="store_true")
    a = p.parse_args(argv)
    res = _one((os.path.basename(a.base), a.base, a.sim, not a.no_rates))
    write_report([res], a.out)
    print("%d findings; report %s.md" % (len(res[3]), a.out))


if __name__ == "__main__":
    main()
