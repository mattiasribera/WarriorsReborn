"""Cutscene sweep (tools/cutscene_sweep/): mission list, chapter shortcut, scene comparison.

The comparison runs on synthetic --rate-probe files written with ratediff.ProbeWriter: a small
"data" range with a handle table, per-character position records and character slots (a state
word), a camera matrix behind a pointer and a timer-list header. A default (30 Hz) and a
--sim60 run of the same scene are built so that one character walks twice as fast, one
appears earlier, one changes state at half the game time, one follows the same path, and the
camera moves the same. No game data.
"""
import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "ratediff"))
sys.path.insert(0, os.path.join(ROOT, "tools", "cutscene_sweep"))

try:
    import numpy  # noqa: F401
except ImportError:  # the comparison needs numpy (/usr/bin/python3 has it)
    numpy = None

import sweep  # noqa: E402  (no numpy needed)

if numpy is not None:
    import ratediff
    import scenediff

LUA = """
  levelNames = {}
  levelNames = {
    {
      "level100",
      "level100",
      100,
      0,
      0,
      LT_NONE
    },
    {
      "level99",
      "level99",
      99,
      3,
      1,
      LT_NONE
    },
    {
      "level81",
      "level81",
      81,
      5,
      8,
      LT_NONE
    },
    {
      "level60",
      "level60",
      60,
      3,
      25,
      LT_NONE
    }
  }
  for l1, l2 in levelNames do
"""


class SweepPlanTest(unittest.TestCase):
    def test_missions_from_level_names(self):
        ms = sweep.missions_from_lua(LUA)
        self.assertEqual([(m.level, m.chapters, m.mission) for m in ms], [("level99", 3, 1), ("level81", 5, 8)])

    def test_mission_selection(self):
        ms = sweep.missions_from_lua(LUA)
        self.assertEqual([m.level for m in sweep.select(ms, "8")], ["level81"])
        self.assertEqual([m.level for m in sweep.select(ms, "1-8")], ["level99", "level81"])
        jobs = sweep.jobs(sweep.select(ms, "1"), None)
        self.assertEqual([(j.level, j.chapter) for j in jobs], [("level99", 1), ("level99", 2), ("level99", 3)])
        self.assertEqual([j.chapter for j in sweep.jobs(ms, "2")], [2, 2])

    def test_shortcut_pokes(self):
        p = sweep.shortcut_pokes("level81", 3)
        self.assertEqual(p, ["0x44c078:6c6576656c383100", "0x27fd99:b878c04400/e872510a00", "0x335f40:66b80300c3/a1789f5b00"])
        with self.assertRaises(ValueError):
            sweep.shortcut_pokes("level100", 1)   # 8 characters do not fit before .data
        with self.assertRaises(ValueError):
            sweep.shortcut_pokes("level81", 0)

    def test_command(self):
        j = sweep.Job("level99", 1, 2)
        cmd = sweep.command(j, sim60=True, xbrun="xb", xbe="d.xbe", disc="disc", hdd="hdd", frames=9000,
                            probe="out.rp", interval=50)
        self.assertEqual(cmd[:3], ["xb", "d.xbe", "disc"])
        self.assertIn("--sim60", cmd)
        self.assertEqual(cmd[cmd.index("--rate-probe") + 1], "out.rp:50")
        self.assertEqual(cmd.count("--poke"), 3)
        ranges = [cmd[i + 1] for i, a in enumerate(cmd) if a == "--rate-probe-range"]
        self.assertIn("@0x4cf13c+0x1c:64", ranges)
        self.assertIn("@0x4ff92c:0x1000", ranges)   # slot 0 object behind character +0xb4
        self.assertIn("@0x4ff878:0x200".replace("4ff878", "%x" % (0x4FF878 + 0x668 + 0x170)), ranges)
        self.assertLessEqual(len(ranges), 47)
        self.assertNotIn("--sim60", sweep.command(j, sim60=False, xbrun="xb", xbe="d.xbe", disc="disc", hdd="hdd",
                                                    frames=9000, probe="o.rp", interval=50))


class SweepCandidateFixTest(unittest.TestCase):
    """A candidate fix (extra pokes) runs as a tagged --sim60 run and pairs with the base run."""

    def test_tagged_names_and_pokes(self):
        j = sweep.Job("level87", 3, 3)
        self.assertEqual(sweep.run_name(j, True, "age"), "m03_level87_c3_simage")
        self.assertEqual(sweep.run_name(j, False, "age"), "m03_level87_c3_base")   # base runs are shared
        cmd = sweep.command(j, sim60=True, xbrun="xb", xbe="d.xbe", disc="disc", hdd="hdd", frames=10,
                            probe="o.rp", interval=50, extra_pokes=["0x1a888b:bfb4000000/bf5a000000"])
        self.assertEqual(cmd.count("--poke"), 4)
        self.assertEqual(cmd[-1], "--sim60")

    def test_pairs(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("m01_level99_c1_base", "m01_level99_c1_sim", "m01_level99_c1_simage", "m01_level99_c2_base",
                      "m01_level99_c2_simage", "m01_level99_c3_sim"):
                open(os.path.join(d, n + ".done"), "w").close()
            self.assertEqual([p[0] for p in sweep.pairs(d)], ["m01_level99_c1"])
            self.assertEqual([p[0] for p in sweep.pairs(d, "age")], ["m01_level99_c1", "m01_level99_c2"])
            self.assertTrue(sweep.pairs(d, "age")[1][2].endswith("m01_level99_c2_simage.rp"))

    def test_nice_prefix(self):
        self.assertEqual(sweep.niced(["xb", "a"], 19), ["nice", "-n", "19", "xb", "a"])
        self.assertEqual(sweep.niced(["xb"], None), ["xb"])

    def test_scene_filter(self):
        ms = sweep.missions_from_lua(LUA)
        js = sweep.jobs(ms, None, scenes={"m01_level99_c2", "m08_level81_c5"})
        self.assertEqual([(j.level, j.chapter) for j in js], [("level99", 2), ("level81", 5)])


# Synthetic layout: a data range at DATA with handles, positions and slots; the camera behind CAMP.
DATA = 0x10000
HANDLES, POS, POOL, STRIDE, NS = DATA, DATA + 0x100, DATA + 0x400, 0x200, 4
CAMP, TIMERP = 0x9000, 0x9100
SIZE = 0x400 + STRIDE * NS
LAYOUT = None if numpy is None else scenediff.Layout(
    handles=HANDLES, handle_stride=8, pos=POS, pos_stride=32, pool=POOL, pool_stride=STRIDE,
    state_off=0x190, slots=NS, cam_ptr=CAMP, cam_off=0x1C, timer_ptr=TIMERP)


def scene(path, sim):
    """A front end (clock 0..2 s), a clock reset (level start), then a 20 s scene sampled every 50 ms."""
    w = ratediff.ProbeWriter(path, 50, 1 if sim else 0,
                             [(DATA, SIZE, 0, 0), (CAMP, 64, 1, 0x1C), (TIMERP, 0x28, 1, 0)])
    frame = 0
    for ms in range(0, 2000, 50):
        frame += 3
        w.snapshot(frame, ms, [bytes(SIZE), bytes(64), bytes(0x28)], [DATA, 0x20000, 0x30000])
    for ms in range(0, 20000, 50):
        frame += 3
        t = ms / 1000.0
        mem = bytearray(SIZE)
        alive = [True, True, t >= (3.0 if sim else 6.0), True]
        for s in range(NS):
            if alive[s]:
                struct.pack_into("<I", mem, HANDLES - DATA + 8 * s, POOL + STRIDE * s)
        # slot 0: walks 1 unit/s in both; slot 1: 0.5 unit/s at 30 Hz, 1 unit/s with sim60 (2x).
        struct.pack_into("<3f", mem, POS - DATA, t, 0.0, 0.0)
        struct.pack_into("<3f", mem, POS - DATA + 32, (2.0 if sim else 1.0) * 0.5 * t, 5.0, 0.0)
        struct.pack_into("<3f", mem, POS - DATA + 64, 3.0, 3.0, 0.0)
        # slot 3: idle, but its state machine steps every 4 s (2 s with sim60).
        step = 2.0 if sim else 4.0
        struct.pack_into("<I", mem, POOL - DATA + 3 * STRIDE + 0x190, 0x100 + int(t // step))
        struct.pack_into("<3f", mem, POS - DATA + 96, -4.0, 0.0, 0.0)
        # slot 0 is placed (a scripted cut) at 8 s and 14 s at 30 Hz, at 4 s and 7 s with sim60 (2x)
        # in the second run of the schedule test only (see scene2x); here both at 8 and 14 s.
        cut = (4.0, 7.0) if sim == "2x" else (8.0, 14.0)
        if t >= cut[0]:
            k = 1 + (t >= cut[1])
            struct.pack_into("<3f", mem, POS - DATA, 100.0 * k + (t - cut[k - 1]), 0.0, 0.0)
        cam = struct.pack("<16f", *([0.0] * 12 + [t, 1.0, 2.0, 1.0]))
        n = 3 - min(3, int(t // 5))          # three timers fire at 5, 10 and 15 s in both
        timers = struct.pack("<10I", 0, 0, 0, 0x40000, 0x40000 + 0x18 * n, 0, 0, 0, 0, 2000)
        w.snapshot(frame, ms, [bytes(mem), cam, timers], [DATA, 0x20000, 0x30000])
    w.close()


@unittest.skipIf(numpy is None, "needs numpy")
class SceneDiffTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.base = os.path.join(cls.tmp.name, "base.rp")
        cls.sim = os.path.join(cls.tmp.name, "sim.rp")
        scene(cls.base, False)
        scene(cls.sim, True)
        cls.b = scenediff.load_series(cls.base, LAYOUT)
        cls.s = scenediff.load_series(cls.sim, LAYOUT)
        cls.findings = scenediff.compare(cls.b, cls.s)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def kinds(self, slot):
        return {f.kind for f in self.findings if f.slot == slot}

    def test_segment_after_level_start(self):
        self.assertEqual(self.b.t[0], 0)
        self.assertEqual(len(self.b.t), 400)
        self.assertEqual(self.b.start_frame, 3 * 41)
        self.assertTrue(self.b.alive[0, 0])
        self.assertFalse(self.b.alive[0, 2])
        self.assertAlmostEqual(float(self.b.pos[20, 1, 0]), 0.5, places=4)

    def test_motion_ratio(self):
        r = scenediff.motion_ratio(self.b, self.s, 1, 0, 20000)
        self.assertAlmostEqual(r, 2.0, places=2)
        self.assertAlmostEqual(scenediff.motion_ratio(self.b, self.s, 0, 0, 20000), 1.0, places=2)

    def test_flags(self):
        self.assertIn("fast", self.kinds(1))
        self.assertIn("diverge", self.kinds(1))
        self.assertIn("spawn", self.kinds(2))
        self.assertIn("state2x", self.kinds(3))
        self.assertEqual(self.kinds(0), set())
        self.assertNotIn("cam", {f.kind for f in self.findings})
        self.assertNotIn("timers", {f.kind for f in self.findings})

    def test_spawn_ratio(self):
        f = [f for f in self.findings if f.kind == "spawn"][0]
        self.assertAlmostEqual(f.base_ms, 6000, delta=60)
        self.assertAlmostEqual(f.sim_ms, 3000, delta=60)
        self.assertAlmostEqual(f.ratio, 0.5, places=1)

    def test_state_ratio(self):
        f = [f for f in self.findings if f.kind == "state2x"][0]
        self.assertAlmostEqual(f.ratio, 0.5, places=1)

    def test_timer_events_match(self):
        eb, es = scenediff.timer_events(self.b), scenediff.timer_events(self.s)
        self.assertEqual([round(x / 1000) for x in eb], [5, 10, 15])
        self.assertEqual(eb, es)

    def test_schedule_matches(self):
        n, off, ratio = scenediff.schedule(self.b, self.s)
        self.assertEqual(n, 2)
        self.assertAlmostEqual(off, 0, delta=60)
        self.assertNotIn("schedule2x", {f.kind for f in self.findings})

    def test_schedule_2x(self):
        path = os.path.join(self.tmp.name, "sim2x.rp")
        scene(path, "2x")
        s2 = scenediff.load_series(path, LAYOUT)
        n, off, ratio = scenediff.schedule(self.b, s2)
        self.assertEqual(n, 2)
        self.assertAlmostEqual(ratio, 0.5, places=2)
        self.assertIn("schedule2x", {f.kind for f in scenediff.compare(self.b, s2)})

    def test_identical_runs_have_no_findings(self):
        self.assertEqual(scenediff.compare(self.b, self.b), [])

    def test_strided_rows_merge(self):
        rows = [{"field": "0x%08x" % (0x1000 + 0x2c * k), "kind": "i32", "n": 1, "ratio": 1.8, "rate_base": 1,
                 "rate_sim": 2, "examples": ""} for k in range(5)]
        rows.append({"field": "cslot[*]+0x28", "kind": "i32", "n": 7, "ratio": 2.0, "rate_base": 30, "rate_sim": 60,
                     "examples": ""})
        m = scenediff.merge_strided(rows)
        self.assertEqual([(r["field"], r["n"]) for r in m], [("0x00001000 + k*0x2c", 5), ("cslot[*]+0x28", 7)])

    def test_report(self):
        out = os.path.join(self.tmp.name, "rep")
        rows = [{"field": "cslot[*]+0x28", "kind": "i32", "n": 2, "ratio": 2.0, "rate_base": 30, "rate_sim": 60,
                 "examples": ""}]
        scenediff.write_report([("level99 ch1", self.b, self.s, self.findings, rows)], out)
        with open(out + ".md") as f:
            md = f.read()
        self.assertIn("level99 ch1", md)
        self.assertIn("state2x", md)
        self.assertIn("| cslot[*]+0x28 | i32 | 1 |", md)
        self.assertTrue(os.path.exists(out + ".csv"))


if __name__ == "__main__":
    unittest.main()
