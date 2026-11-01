"""Rate diff: the runtime probe (runtime/build/rateprobe_test) and the analysis tool
(tools/ratediff/ratediff.py) on synthetic snapshots.

Two synthetic probe files stand for a default (30 Hz world) and a --sim60 run of the same
input. Field 0 is a counter that advances per world update (2x under sim60), field 1 a position
that follows game time (1x), field 2 is noisy (random spikes, scaled so that the totals are exactly
2x, but the chunks of game time disagree), field 3 holds a 16-bit per-update decrement in its low half under a high half
that changes the same way in both runs, and field 4 never changes. A pair of snapshots across a
large gap of game time and an unreadable sample must not count. No game data.
"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "ratediff"))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "rateprobe_test")

try:
    import numpy  # noqa: F401
except ImportError:  # the analysis needs numpy (/usr/bin/python3 has it)
    numpy = None
if numpy is not None:
    import ratediff

BASE = 0x1000
NFIELDS = 5


def fields_bytes(vals):
    """vals: [u32 counter, f32 pos, u32 noisy, (lo16, hi16), u32 const]."""
    lo, hi = vals[3]
    return struct.pack("<IfIHHI", vals[0] & 0xFFFFFFFF, vals[1], vals[2] & 0xFFFFFFFF, lo & 0xFFFF, hi & 0xFFFF, vals[4])


def spikes(seed, count, n, scale_to=None):
    """Random spikes {sample: magnitude}; optionally scaled to a given total."""
    import random
    rng = random.Random(seed)
    out = {rng.randrange(n): rng.randint(1, 1000) for _ in range(count)}
    if scale_to:
        k = scale_to / sum(out.values())
        out = {i: int(round(v * k)) for i, v in out.items()}
    return out


def make_run(path, per_update, noise, n=240, interval=100):
    """One run: n snapshots 100 ms apart (game time); per_update world updates per sample."""
    w = ratediff.ProbeWriter(path, interval, 1 if per_update == 6 else 0, [(BASE, NFIELDS * 4, 0, 0)])
    counter, pos, noisy, lo, hi = 1000, 0.0, 0, 30000, 0
    ms = 5000
    for i in range(n):
        if i == 120:
            ms += 20000          # a gap (loading): this pair must not count
            counter += 99999
        if i == 60:
            w.snapshot(i * 2, ms, [None])   # unreadable sample
        else:
            w.snapshot(i * 2, ms, [fields_bytes([counter, pos, noisy, (lo, hi), 7])])
        ms += interval
        counter += per_update
        pos += 0.5
        lo -= per_update // 3      # 1 or 2 per sample
        hi = (hi + 0x100) & 0xFFFF  # same in both runs
        noisy += noise.get(i, 0)
    w.close()


@unittest.skipIf(numpy is None, "numpy not available")
class RateDiffAnalysisTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.base = os.path.join(cls.tmp.name, "base.rp")
        cls.sim = os.path.join(cls.tmp.name, "sim.rp")
        noise = spikes(1, 24, 240)
        make_run(cls.base, 3, noise)
        make_run(cls.sim, 6, spikes(2, 24, 240, scale_to=2 * sum(noise.values())))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_reader_round_trip(self):
        hdr, snaps = ratediff.read_probe(self.base)
        self.assertEqual(hdr["interval_ms"], 100)
        self.assertEqual(hdr["ranges"], [(BASE, NFIELDS * 4, 0, 0)])
        snaps = list(snaps)
        self.assertEqual(len(snaps), 240)
        self.assertEqual(snaps[60][2][0][1], ratediff.KIND_NONE)

    def findings(self):
        b = ratediff.accumulate(self.base, chunks=6)
        s = ratediff.accumulate(self.sim, chunks=6)
        return {(f["addr"], f["kind"]): f for f in ratediff.compare(b, s, min_changes=10)}

    def test_flags_2x_counter(self):
        f = self.findings()
        self.assertIn((BASE, "i32"), f)
        self.assertAlmostEqual(f[(BASE, "i32")]["ratio"], 2.0, delta=0.05)
        self.assertEqual(f[(BASE, "i32")]["band"], "2x")

    def test_ignores_1x_noise_gap_and_constant(self):
        f = self.findings()
        addrs = {a for a, _ in f}
        self.assertNotIn(BASE + 4, addrs, "1x position flagged")
        self.assertNotIn(BASE + 8, addrs, "noisy field flagged")
        self.assertNotIn(BASE + 16, addrs, "constant field flagged")

    def test_gap_not_counted(self):
        b = ratediff.accumulate(self.base, chunks=6)
        rate = b.ranges[0].abs["i32"][:, 0].sum() / b.ranges[0].dur.sum()
        self.assertAlmostEqual(rate, 30.0, delta=0.5)   # 3 per 100 ms; the +99999 jump left out

    def test_flags_16bit_half(self):
        f = self.findings()
        self.assertIn((BASE + 12, "lo16"), f)
        self.assertNotIn((BASE + 12, "hi16"), f)
        self.assertNotIn((BASE + 12, "i32"), f)

    def test_frame_window(self):
        b = ratediff.accumulate(self.base, chunks=4, from_frame=300, to_frame=400)
        self.assertAlmostEqual(b.ranges[0].dur.sum(), 50 * 0.1, delta=1e-6)   # frames 300..400: 51 samples

    def test_annotation(self):
        disasm = (
            "  12000:\tff 05 00 10 00 00 \tincl   0x1000\n"
            "  12006:\ta1 00 10 00 00    \tmov    0x1000,%eax\n"
            "  30000:\t66 ff 48 0c       \tdecw   0xc(%eax)\n"
            "  30004:\t89 04 8d 00 10 00 00\tmov    %eax,0x1000(,%ecx,4)\n"
        )
        idx = ratediff.DisasmIndex.from_text(disasm)
        closure = ratediff.Closure.from_text("00011ff0 100\n")
        w = idx.refs(0x1000)
        self.assertEqual([(r.addr, r.write) for r in w], [(0x12000, True), (0x12006, False)])
        self.assertTrue(closure.contains(0x12000))
        self.assertFalse(closure.contains(0x30000))
        self.assertEqual([r.addr for r in idx.disp_writes(0xC)], [0x30000])
        self.assertEqual([r.addr for r in idx.indexed_refs(0x1008)], [0x30004])

    def test_report_files(self):
        b = ratediff.accumulate(self.base, chunks=6)
        s = ratediff.accumulate(self.sim, chunks=6)
        out = os.path.join(self.tmp.name, "report")
        md, csv = ratediff.write_report(ratediff.compare(b, s, min_changes=10), b, s, out, None, None)
        with open(md) as fh:
            text = fh.read()
        self.assertIn("0x00001000", text)
        with open(csv) as fh:
            self.assertGreaterEqual(len(fh.read().splitlines()), 3)   # header + 2 findings


class RateProbeBinaryTest(unittest.TestCase):
    def test_rateprobe(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/rateprobe_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([l for l in res.stdout.splitlines() if l.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
