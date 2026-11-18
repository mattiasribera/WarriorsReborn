"""Game-independent tests of tools/texture_basis.py (texture pack -> KTX2 pack): the uncompressed
mipmapped .dds handed to the encoder (every level, RGBA bytes, alpha coverage kept), the codec per
texture class (opaque, alpha, UI copy, _n/_s maps with linear metrics), .dds sources (decoded
levels, --source newer/png, --opaque keep), --like, resumable runs, stale outputs, files copied
as is, and failures reported per texture, with a stand-in basisu (no encoder, no game data). TEXPACK_BASISU=<basisu v2.50> adds one real encode whose header is checked.
Skipped when numpy is not installed.
"""
import os
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import texture_basis as tb
    import texture_compress as tc
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None

# Stand-in for basisu: logs its options and a summary of the source .dds (w h levels, alpha of
# every level's first texel, alpha-test coverage of every level, bit count + masks, trailing bytes),
# then writes a KTX2 header for it: a wrong level count while the file "bad" exists next to the
# script, nothing (exit 3) while "fail" exists. Paths are in the script, not the environment: the
# tool's worker processes may come from a process started by an earlier test.
FAKE = '''#!%s
import os, struct, sys
args = sys.argv[1:]
src = [a for a in args if a.endswith(".dds")][0]
dst = args[args.index("-output_file") + 1]
d = open(src, "rb").read()
h, w = struct.unpack_from("<2I", d, 12)
levels = struct.unpack_from("<I", d, 28)[0]
masks = struct.unpack_from("<5I", d, 88)
at, cov, first = 128, [], []
for l in range(levels):
    lw, lh = max(1, w >> l), max(1, h >> l)
    px = d[at:at + lw * lh * 4]
    first.append(px[3])
    cov.append(sum(1 for i in range(3, len(px), 4) if px[i] >= 128) / (lw * lh))
    at += lw * lh * 4
with open(%r, "a") as f:
    f.write("%%s|%%d %%d %%d|%%s|%%s|%%s|%%d\\n" %% (" ".join(a for a in args if a.startswith("-") and a != "-output_file"),
            w, h, levels, ",".join(str(a) for a in first), ",".join("%%.3f" %% c for c in cov), "%%x %%x %%x %%x %%x" %% masks, len(d) - at))
name = os.path.basename(os.path.dirname(dst))
here = os.path.dirname(os.path.abspath(sys.argv[0]))
if os.path.exists(os.path.join(here, "fail")):
    sys.exit(3)
bad = os.path.exists(os.path.join(here, "bad"))
hdr = b"\\xabKTX 20\\xbb\\r\\n\\x1a\\n" + struct.pack("<8I", 0, 1, w, h, 0, 0, 1, levels - (1 if bad else 0)) + bytes(32)
open(dst, "wb").write(hdr)
'''


def bc1_dds(w, h, rgb):
    """A BC1 .dds of one colour with the full mip chain (every block: both endpoints that colour)."""
    c = (rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3
    dims = tc.level_dims(w, h)
    blocks = struct.pack("<2HI", c, c, 0) * sum(((lw + 3) // 4) * ((lh + 3) // 4) for lw, lh in dims)
    return tc.dds_header(tc.BC1, w, h, len(dims)) + blocks


def solid(w, h, rgba):
    img = np.empty((h, w, 4), dtype=np.uint8)
    img[...] = rgba
    return img


@unittest.skipIf(np is None, "numpy not installed")
class HelperTest(unittest.TestCase):
    def test_rgba_dds_holds_every_level(self):
        img = solid(8, 4, (10, 20, 30, 40))
        img[0, 0] = (1, 2, 3, 4)
        levels = tc.mip_chain(img, "box")
        d = tb.rgba_dds(levels)
        self.assertEqual(d[:4], b"DDS ")
        size, flags, h, w, pitch, depth, mips = struct.unpack_from("<7I", d, 4)
        self.assertEqual((size, h, w, pitch, mips), (124, 4, 8, 32, 4))
        self.assertTrue(flags & 0x20000, "DDSD_MIPMAPCOUNT")
        self.assertEqual(struct.unpack_from("<2I4s5I", d, 76), (32, 0x41, b"\0\0\0\0", 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000))
        self.assertEqual(len(d), 128 + 4 * (32 + 8 + 2 + 1))
        self.assertEqual(d[128:132], bytes((1, 2, 3, 4)), "R in the low byte (RGBA order)")
        self.assertEqual(d[-4:], bytes(levels[-1][0, 0]), "the 1x1 level last")

    def test_rgba_dds_adds_opaque_alpha_to_rgb(self):
        d = tb.rgba_dds([np.full((1, 1, 3), 7, np.uint8)])
        self.assertEqual(d[128:132], bytes((7, 7, 7, 255)))

    def test_ktx2_info(self):
        hdr = tb.KTX2_ID + struct.pack("<8I", 0, 1, 16, 8, 0, 0, 1, 5) + bytes(32)
        self.assertEqual(tb.ktx2_info(hdr), (16, 8, 5, 1, 0))
        with self.assertRaises(ValueError):
            tb.ktx2_info(b"DDS " + bytes(60))

    def test_codec_args(self):
        o = {"etc1s_quality": 90, "effort": 3, "uastc_level": 2, "uastc_rdo": 1.5, "xubc7_rdo": 20, "xubc7_bc7e": -1}
        self.assertEqual(tb.codec_args("etc1s", o, False), ["-etc1s", "-quality", "90", "-effort", "3"])
        self.assertEqual(tb.codec_args("uastc", o, True), ["-uastc", "-uastc_level", "2", "-uastc_rdo_l", "1.5", "-linear"])
        self.assertEqual(tb.codec_args("xubc7", dict(o, xubc7_bc7e=6), False),
                         ["-xubc7", "-effort", "3", "-xubc7_rdo_level", "20", "-xubc7_bc7e_scalar_level", "6"])
        self.assertEqual(tb.codec_args("uastc", dict(o, uastc_rdo=0), False), ["-uastc", "-uastc_level", "2"])

    def test_split_name(self):
        self.assertEqual(tb.split_name("0123456789ABCDEF_N.png"), ("0123456789abcdef", "_n", ".png"))
        self.assertEqual(tb.split_name("0123456789abcdef.ktx2"), ("0123456789abcdef", "", ".ktx2"))
        self.assertIsNone(tb.split_name("0123456789abcdef.emit.dds"))
        self.assertIsNone(tb.split_name("selection.csv"))


@unittest.skipIf(np is None, "numpy not installed")
class ConvertTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)
        tu.PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.pack, self.out = os.path.join(d, "pack"), os.path.join(d, "out")
        os.makedirs(self.pack)
        self.enc = os.path.join(d, "fake_basisu")
        self.log = os.path.join(d, "enc.log")
        self.fail_flag, self.bad_flag = os.path.join(d, "fail"), os.path.join(d, "bad")
        with open(self.enc, "w") as f:
            f.write(FAKE % (sys.executable, self.log))
        os.chmod(self.enc, 0o755)
        self.opaque, self.alpha, self.ui, self.relief, self.dds_only = (
            "00000000000000a1", "00000000000000a2", "00000000000000a3", "00000000000000a4", "00000000000000a5")
        tu.write_png(os.path.join(self.pack, self.opaque + ".png"), solid(8, 4, (10, 20, 30, 255)))
        cut = solid(16, 16, (200, 100, 50, 0))   # a cutout: a third of the texels pass the alpha test
        cut[:, ::3, 3] = 200
        tu.write_png(os.path.join(self.pack, self.alpha + ".png"), cut)
        tu.write_png(os.path.join(self.pack, self.ui + ".png"), solid(4, 4, (1, 2, 3, 255)))
        tu.write_png(os.path.join(self.pack, self.relief + "_n.png"), solid(8, 8, (128, 128, 255, 90)))   # height 90 in alpha
        tu.write_png(os.path.join(self.pack, self.relief + "_s.png"), solid(4, 4, (200, 60, 0, 255)))
        with open(os.path.join(self.pack, self.dds_only + ".dds"), "wb") as f:   # 8 x 8 BC1, no .png
            f.write(bc1_dds(8, 8, (248, 0, 0)))
        with open(os.path.join(self.pack, self.dds_only + ".emit.dds"), "wb") as f:
            f.write(b"mask")
        with open(os.path.join(self.pack, self.opaque + ".ktx2"), "wb") as f:
            f.write(b"an older ktx2 in the input is not copied")
        with open(os.path.join(self.pack, "selection.csv"), "w") as f:
            f.write("hash,format,w,h,status,category\n%s,0x0C,4,4,upscaled,ui\n" % self.ui)

    def tearDown(self):
        self.tmp.cleanup()

    def run_tool(self, *extra):
        return tb.main(["--pack", self.pack, "--out", self.out, "--basisu", self.enc, "-j", "2", "--pngraw", tu.PNGRAW] + list(extra))

    def test_defaults(self):
        self.assertEqual(self.run_tool(), 0)
        with open(os.path.join(self.out, self.dds_only + ".dds"), "rb") as f, \
                open(os.path.join(self.pack, self.dds_only + ".dds"), "rb") as g:
            self.assertEqual(f.read(), g.read(), "--opaque keep (default): an opaque BC1 .dds is copied as is")
        opts = {c[1]: c[0].split() for c in self.calls()}
        self.assertIn("-etc1s", opts["8 4 4"], "keep without a .dds falls back to etc1s")
        for k in ("16 16 5", "4 4 3"):
            self.assertIn("-xubc7_bc7e_scalar_level", opts[k], "XUBC7 over bc7e (level 6 by default)")

    def calls(self):
        with open(self.log) as f:
            return [ln.rstrip("\n").split("|") for ln in f]

    def call_for(self, w, h):
        return [c for c in self.calls() if c[1].startswith("%d %d " % (w, h))]

    def test_convert(self):
        self.assertEqual(self.run_tool("--opaque", "etc1s"), 0)
        names = sorted(os.listdir(self.out))
        self.assertEqual(names, sorted([self.opaque + ".ktx2", self.alpha + ".ktx2", self.ui + ".png", self.relief + "_n.ktx2",
                                        self.relief + "_s.ktx2", self.dds_only + ".ktx2", self.dds_only + ".emit.dds", "selection.csv"]))
        for h in (self.opaque, self.alpha, self.relief + "_n", self.relief + "_s", self.dds_only):
            with open(os.path.join(self.out, h + ".ktx2"), "rb") as f:
                self.assertEqual(f.read(12), tb.KTX2_ID, h)
        calls = self.calls()
        self.assertEqual(len(calls), 5, "UI copied, not encoded")
        (dds,) = [c for c in calls if c[1] == "8 8 4" and "-etc1s" in c[0].split()]
        self.assertEqual(dds[2], "255,255,255,255", "a BC1 .dds source: its own 4 levels, opaque -> --opaque")
        opts = {c[1]: c[0] for c in calls if not (c[1] == "8 8 4" and "-etc1s" in c[0].split())}
        self.assertEqual(opts["8 4 4"].split()[:2], ["-ktx2", "-no_multithreading"])
        self.assertIn("-etc1s", opts["8 4 4"].split(), "opaque -> etc1s (BC1)")
        self.assertNotIn("-linear", opts["8 4 4"].split(), "colour keeps sRGB (perceptual) metrics")
        self.assertIn("-xubc7", opts["16 16 5"].split(), "alpha -> xubc7 (BC7)")
        self.assertIn("-xubc7", opts["8 8 4"].split(), "_n -> the maps codec")
        self.assertIn("-linear", opts["8 8 4"].split(), "maps use linear metrics")
        self.assertIn("-linear", opts["4 4 3"].split())
        for c in calls:
            self.assertEqual(c[4], "20 ff ff00 ff0000 ff000000", "uncompressed 32-bit RGBA source")
            self.assertEqual(c[5], "0", "exactly the levels' bytes")

    def test_mips_keep_alpha_coverage_and_height(self):
        self.assertEqual(self.run_tool(), 0)
        (cut,) = self.call_for(16, 16)
        cov = [float(x) for x in cut[3].split(",")]
        self.assertAlmostEqual(cov[0], 6 / 16, places=3)
        for c in cov[1:3]:
            self.assertLess(abs(c - cov[0]), 0.13, "alpha-test coverage kept down the chain: %s" % cov)
        (n,) = [c for c in self.call_for(8, 8) if "-linear" in c[0].split()]
        self.assertEqual(n[2], "90,90,90,90", "a normal map's height (alpha) is not touched by coverage")
        self.assertEqual(self.run_tool("--alpha-coverage", "none", "--force"), 0)
        cov2 = [float(x) for x in self.call_for(16, 16)[-1][3].split(",")]
        self.assertNotEqual(cov2[1:3], cov[1:3], "--alpha-coverage none leaves the filtered alpha")

    def test_codec_options(self):
        self.assertEqual(self.run_tool("--opaque", "uastc", "--alpha", "etc1s", "--maps", "uastc", "--uastc-rdo", "2"), 0)
        opts = {c[1]: c[0].split() for c in self.calls() if "-linear" in c[0].split() or c[1] != "8 8 4"}
        self.assertEqual(opts["8 4 4"][2:], ["-quiet", "-no_stats", "-uastc", "-uastc_level", "-uastc_rdo_l"])   # values not logged
        self.assertIn("-etc1s", opts["16 16 5"])
        self.assertIn("-uastc", opts["8 8 4"])
        self.assertIn("-linear", opts["8 8 4"])

    def test_resume_and_force(self):
        self.assertEqual(self.run_tool(), 0)
        first = len(self.calls())
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(len(self.calls()), first, "up-to-date outputs are skipped")
        time.sleep(0.05)
        src = os.path.join(self.pack, self.opaque + ".png")
        tu.write_png(src, solid(8, 4, (11, 20, 30, 255)))
        os.utime(src, (time.time() + 5, time.time() + 5))
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(len(self.calls()), first + 1, "a changed source is redone")
        self.assertEqual(self.run_tool("--force"), 0)
        self.assertEqual(len(self.calls()), 2 * first + 1, "--force redoes everything")

    def test_stale_outputs_removed(self):
        os.makedirs(self.out)
        for n in (self.opaque + ".dds", self.ui + ".ktx2"):
            with open(os.path.join(self.out, n), "wb") as f:
                f.write(b"stale")
        self.assertEqual(self.run_tool(), 0)
        self.assertFalse(os.path.exists(os.path.join(self.out, self.opaque + ".dds")), "a .dds would win over the .ktx2")
        self.assertFalse(os.path.exists(os.path.join(self.out, self.ui + ".ktx2")), "a .ktx2 would win over the copied PNG")

    def test_with_png(self):
        self.assertEqual(self.run_tool("--with-png"), 0)
        for h in (self.opaque, self.alpha, self.relief + "_n"):
            self.assertTrue(os.path.exists(os.path.join(self.out, h + ".png")), h)

    def test_failures_reported(self):
        open(self.fail_flag, "w").close()
        self.assertEqual(self.run_tool(), 1)
        self.assertFalse(any(n.endswith(".ktx2") for n in os.listdir(self.out)), "no partial outputs")
        os.remove(self.fail_flag)
        open(self.bad_flag, "w").close()
        self.assertEqual(self.run_tool(), 1, "an output with the wrong level count is an error")
        self.assertFalse(any(n.endswith(".ktx2") for n in os.listdir(self.out)))
        os.remove(self.bad_flag)
        self.assertEqual(self.run_tool(), 0)

    def test_like_reproduces_a_dds_pack(self):
        like = os.path.join(self.tmp.name, "like")
        os.makedirs(like)
        with open(os.path.join(like, self.opaque + ".dds"), "wb") as f:      # the reference made it BC7
            f.write(tc.dds_header(tc.BC7, 8, 4, 4))
        with open(os.path.join(like, self.alpha + ".dds"), "wb") as f:       # at half size, BC1
            f.write(b"DDS " + tc.dds_header(tc.BC1, 8, 8, 4)[4:])
        tu.write_png(os.path.join(like, self.ui + ".png"), solid(4, 4, (1, 2, 3, 255)))
        with open(os.path.join(like, self.relief + "_n.dds"), "wb") as f:   # maps too; _s is not in it
            f.write(tc.dds_header(tc.BC7, 8, 8, 4))
        os.makedirs(self.out)
        with open(os.path.join(self.out, self.relief + "_s.ktx2"), "wb") as f:
            f.write(b"stale")
        self.assertEqual(self.run_tool("--like", like, "--opaque", "etc1s"), 0)
        opts = {c[1]: c[0].split() for c in self.calls()}
        self.assertEqual(sorted(opts), ["8 4 4", "8 8 4"], "the alpha texture at the reference's 8 x 8, _n at its own size")
        self.assertEqual(len(self.calls()), 3)
        self.assertIn("-xubc7", opts["8 4 4"], "the reference's BC7 -> --alpha")
        alpha8 = [c for c in self.calls() if c[1] == "8 8 4" and "-etc1s" in c[0].split()]
        self.assertEqual(len(alpha8), 1, "the reference's BC1 -> --opaque, from the PNG's level 1")
        self.assertTrue(os.path.exists(os.path.join(self.out, self.ui + ".png")), "a PNG in the reference stays a PNG")
        self.assertFalse(os.path.exists(os.path.join(self.out, self.relief + "_s.ktx2")), "not in the reference: not in the output")
        with open(os.path.join(like, self.opaque + ".dds"), "wb") as f:     # a size that is no mip level of the PNG
            f.write(tc.dds_header(tc.BC7, 3, 4, 3))
        self.assertEqual(self.run_tool("--like", like, "--force", "--opaque", "etc1s"), 1)

    def test_source_newer_png_or_dds(self):
        s_png = os.path.join(self.pack, self.relief + "_s.png")
        s_dds = os.path.join(self.pack, self.relief + "_s.dds")
        with open(s_dds, "wb") as f:   # a 4 x 4 .dds rewritten after its .png (material_shine.py does that)
            f.write(tc.dds_header(tc.BC7, 4, 4, 3) + bytes([0x40] + [0] * 15) * 3)   # BC7 mode 6, zero endpoints
        os.utime(s_png, (time.time() - 100, time.time() - 100))
        self.assertEqual(self.run_tool(), 0)
        (s,) = [c for c in self.call_for(4, 4) if "-linear" in c[0].split()]
        self.assertNotEqual(s[2], "255,255,255", "--source newer: the newer .dds (BC7 mode 6 zero block: alpha 0)")
        self.assertEqual(self.run_tool("--source", "png", "--force"), 0)
        (s,) = [c for c in self.call_for(4, 4) if "-linear" in c[0].split()][-1:]
        self.assertEqual(s[2], "255,255,255", "--source png: the .png (alpha 255)")
        self.assertFalse(os.path.exists(os.path.join(self.out, self.relief + "_s.dds")), "no .dds next to the .ktx2")

    def test_opaque_keep_copies_the_bc1_dds(self):
        self.assertEqual(self.run_tool("--opaque", "keep"), 0)   # also the default (test_defaults)
        with open(os.path.join(self.out, self.dds_only + ".dds"), "rb") as f, \
                open(os.path.join(self.pack, self.dds_only + ".dds"), "rb") as g:
            self.assertEqual(f.read(), g.read(), "the BC1 .dds as is")
        self.assertFalse(os.path.exists(os.path.join(self.out, self.dds_only + ".ktx2")))
        self.assertTrue(os.path.exists(os.path.join(self.out, self.opaque + ".ktx2")), "a PNG-only opaque texture: etc1s")
        self.assertIn("-etc1s", [c for c in self.calls() if c[1] == "8 4 4"][0][0].split())

    def test_broken_dds_reported(self):
        with open(os.path.join(self.pack, self.dds_only + ".dds"), "wb") as f:
            f.write(b"DDS broken")
        self.assertEqual(self.run_tool(), 1)

    def test_refuses_same_dir_and_missing_encoder(self):
        with self.assertRaises(SystemExit):
            tb.main(["--pack", self.pack, "--out", self.pack, "--basisu", self.enc, "--pngraw", tu.PNGRAW])
        with self.assertRaises(SystemExit):
            tb.main(["--pack", self.pack, "--out", self.out, "--basisu", os.path.join(self.tmp.name, "nope"), "--pngraw", tu.PNGRAW])


@unittest.skipIf(np is None or not os.environ.get("TEXPACK_BASISU"), "real basisu not given (TEXPACK_BASISU)")
class RealEncoderTest(unittest.TestCase):
    def test_real_encode_keeps_the_chain(self):
        with tempfile.TemporaryDirectory() as d:
            img = solid(32, 16, (90, 160, 30, 255))
            img[:, :8, 3] = 0
            levels = tc.mip_chain(img, "lanczos3", 128)
            for codec in tb.CODECS:
                dst = os.path.join(d, codec + ".ktx2")
                opts = {"etc1s_quality": 100, "effort": 2, "uastc_level": 1, "uastc_rdo": 0, "xubc7_rdo": 0, "xubc7_bc7e": -1}
                tb.encode_ktx2(os.environ["TEXPACK_BASISU"], levels, codec, opts, False, d, dst)
                with open(dst, "rb") as f:
                    self.assertEqual(tb.ktx2_info(f.read(64)), (32, 16, 6, 1, 0), codec)


if __name__ == "__main__":
    unittest.main()
