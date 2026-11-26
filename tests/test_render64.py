"""render64: the renderer call stream's wire format.

* runtime/src/render_wire_gen.h is generated from renderer.h by runtime/tools/render64/gen_wire.py
  and must be current (a renderer.h change regenerates it, or recordings stop matching the code);
* render_wire_test round-trips every codec in the i386 build and in the x86_64 build, and the
  records one ABI writes are read by the other (the renderer process reads what the game writes).
No game data, no GPU.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
GEN = os.path.join(RUNTIME, "tools", "render64", "gen_wire.py")
T32 = os.path.join(RUNTIME, "build", "render_wire_test")
T64 = os.path.join(RUNTIME, "build64", "render_wire_test")
GEN_H = os.path.join(RUNTIME, "src", "render_wire_gen.h")


class Render64Test(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", RUNTIME, "build/render_wire_test", "build64/render_wire_test"], check=True)

    def run_ok(self, *args):
        res = subprocess.run(list(args), capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("ALL OK", res.stdout)
        self.assertNotIn("FAIL", res.stdout)

    def test_generated_header_is_current(self):
        res = subprocess.run(["python3", GEN, "--check"], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_new_renderer_calls_are_on_the_wire(self):
        with open(GEN_H) as f:
            gen = f.read()
        # recorded since the PoC: frame generation, dynres, TAA sharpness, glossy relief
        for rid in ("RW_ID_FRAME_GEN,", "RW_ID_FRAME_GEN_HUD_END,", "RW_ID_FRAME_GEN_PRESENT,", "RW_ID_TAA_RENDER_SIZE,",
                    "RW_ID_TAA_SHARPNESS,", "RW_ID_SET_RELIEF_GLOSSY,"):
            self.assertIn(rid, gen)
        # queries (not recorded, counted): FSR 3.1, frame-gen state, wet glass, readbacks
        for q in ("renderer_taa_fsr3", "renderer_frame_gen_on", "renderer_frame_gen_pacing", "renderer_frame_gen_readback",
                  "renderer_gpu_passes", "renderer_wet_glass_on", "renderer_texture_lod_bias"):
            self.assertIn(f'"{q}",', gen)
            self.assertNotIn(f"RW_ID_{q[9:].upper()},", gen)

    def test_begin_record_carries_the_frame_time(self):
        with open(GEN_H) as f:
            gen = f.read()
        wrap = gen[gen.index("void rr_renderer_begin(const uint32_t *base, int pitch_bytes) {"):]
        wrap = wrap[:wrap.index("\n}\n")]
        self.assertIn("rw_f64(rw_w, renderer_frame_time());", wrap)
        replay = gen[gen.index("case RW_ID_BEGIN: {"):]
        replay = replay[:replay.index("return 0;")]
        self.assertLess(replay.index("renderer_set_frame_time(a_frame_time);"), replay.index("renderer_begin(a_base, a_pitch_bytes);"))

    def test_make_test_runs_the_coverage_check(self):
        res = subprocess.run(["make", "-n", "-C", RUNTIME, "test"], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("gen_wire.py --check", res.stdout)

    def test_round_trip_both_abis(self):
        self.run_ok(T32)
        self.run_ok(T64)

    def test_across_abis(self):
        with tempfile.TemporaryDirectory() as d:
            w32, w64 = os.path.join(d, "w32.bin"), os.path.join(d, "w64.bin")
            self.run_ok(T32, "--write", w32)
            self.run_ok(T64, "--read", w32)
            self.run_ok(T64, "--write", w64)
            self.run_ok(T32, "--read", w64)


if __name__ == "__main__":
    unittest.main()
