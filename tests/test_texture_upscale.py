"""Game-independent tests of tools/texture_upscale.py: selection rules, wrap padding and crop,
the separate alpha path and the whole pipeline with a stand-in upscaler (nearest-neighbor 4x,
no GPU, no game data). Skipped when numpy is not installed.
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    import numpy as np
    import texture_upscale as tu
except ImportError:   # numpy missing
    np = None

FAKE = '''#!/usr/bin/env python3
# Stand-in for realesrgan-ncnn-vulkan: nearest-neighbor 4x of every PNG in -i into -o.
import os, sys
sys.path.insert(0, %r)
import numpy as np
import texture_upscale as tu
a = sys.argv
i, o = a[a.index("-i") + 1], a[a.index("-o") + 1]
for n in os.listdir(i):
    img = tu.read_png(os.path.join(i, n))[..., :3]
    tu.write_png(os.path.join(o, n), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1))
'''

FAKE_INFER = '''#!/usr/bin/env python3
# Stand-in for tools/spandrel_infer.py: records argv, nearest-neighbor 4x of every PNG in -i into -o.
import json, os, site, sys
sys.path.insert(0, %r)
sys.path.append(site.getusersitepackages())   # run with -I (as the real venv); numpy may be a user install
import numpy as np
import texture_upscale as tu
a = sys.argv
with open(os.path.join(os.path.dirname(os.path.abspath(a[0])), "argv.json"), "w") as f:
    json.dump(a[1:], f)
i, o = a[a.index("-i") + 1], a[a.index("-o") + 1]
for n in os.listdir(i):
    img = tu.read_png(os.path.join(i, n))[..., :3]
    tu.write_png(os.path.join(o, n), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1))
'''

FAKE_INFER_SCALED = '''#!/usr/bin/env python3
# Stand-in for tools/spandrel_infer.py: appends argv to argv.json; nearest-neighbor upscale by the
# product of the models' scales (file name prefix 1x/2x/4x); honours --skip-existing; an input
# named fail*.png produces no output (a failed texture).
import json, os, site, sys
sys.path.insert(0, %r)
sys.path.append(site.getusersitepackages())
import numpy as np
import texture_upscale as tu
a = sys.argv
with open(os.path.join(os.path.dirname(os.path.abspath(a[0])), "argv.json"), "a") as f:
    f.write(json.dumps(a[1:]) + "\\n")
s = 1
for i, x in enumerate(a):
    if x == "--model":
        s *= int(os.path.basename(a[i + 1])[0])
i, o = a[a.index("-i") + 1], a[a.index("-o") + 1]
for n in os.listdir(i):
    if n.startswith("fail") or ("--skip-existing" in a and os.path.exists(os.path.join(o, n))):
        continue
    img = tu.read_png(os.path.join(i, n))[..., :3]
    tu.write_png(os.path.join(o, n), np.repeat(np.repeat(img, s, axis=0), s, axis=1))
'''


def write_fake(d, text):
    fake = os.path.join(d, "fake_infer.py")
    with open(fake, "w") as f:
        f.write(text % os.path.join(ROOT, "tools"))
    return fake


def read_runs(d):
    with open(os.path.join(d, "argv.json")) as f:
        return [json.loads(line) for line in f]


def run_models(argv):
    return tuple(os.path.basename(argv[i + 1]) for i, x in enumerate(argv) if x == "--model")


def make_dump(d, rows):
    dump = os.path.join(d, "dump")
    os.makedirs(dump)
    with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
        for h, tw, th, lin, alpha in rows:
            w.writerow([h, "0x0F", tw, th, 1, lin, alpha, 1, 0])
    rng = np.random.default_rng(1)
    src = {}
    for h, tw, th, lin, alpha in rows:
        img = rng.integers(0, 256, (th, tw, 4), dtype=np.uint8)
        if not alpha:
            img[..., 3] = 255
        src[h] = img
        tu.write_png(os.path.join(dump, h + ".png"), img)
    return dump, src


@unittest.skipIf(np is None, "numpy not installed")
class TextureUpscaleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/pngraw"], check=True)

    def test_png_round_trip(self):
        img = np.arange(5 * 3 * 4, dtype=np.uint8).reshape(5, 3, 4)
        with tempfile.TemporaryDirectory() as d:
            tu.write_png(os.path.join(d, "x.png"), img)
            self.assertTrue(np.array_equal(tu.read_png(os.path.join(d, "x.png")), img))

    def test_wrap_pad_and_crop(self):
        img = np.arange(4 * 4 * 3, dtype=np.uint8).reshape(4, 4, 3)
        p = tu.wrap_pad(img, 2)
        self.assertEqual(p.shape, (8, 8, 3))
        self.assertTrue(np.array_equal(p[0, 2:6], img[2]))      # top pad = bottom rows
        self.assertTrue(np.array_equal(p[2:6, 0], img[:, 2]))   # left pad = right columns
        big = np.repeat(np.repeat(p, 4, axis=0), 4, axis=1)
        self.assertTrue(np.array_equal(tu.crop(big, 2, 4), np.repeat(np.repeat(img, 4, axis=0), 4, axis=1)))

    def test_fit_and_ui_heuristic(self):
        img = np.full((64, 32, 4), 200, dtype=np.uint8)
        self.assertEqual(tu.fit(img, 16).shape, (16, 8, 4))
        self.assertEqual(tu.fit(img, 64).shape, (64, 32, 4))
        glyphs = np.zeros((16, 16, 4), dtype=np.uint8)
        glyphs[4:8, 4:8] = 255                                  # white glyph on transparent
        self.assertTrue(tu.ui_like(glyphs))
        opaque = np.full((16, 16, 4), 255, dtype=np.uint8)
        self.assertFalse(tu.ui_like(opaque))
        colored = glyphs.copy()
        colored[4:8, 4:8, 1] = 0                                # magenta texels
        self.assertFalse(tu.ui_like(colored))

    def test_pipeline_with_stand_in_upscaler(self):
        with tempfile.TemporaryDirectory() as d:
            dump, out = os.path.join(d, "dump"), os.path.join(d, "pack")
            os.makedirs(dump)
            rows = [("00000000000000a1", 32, 16, 0, 1), ("00000000000000a2", 8, 8, 0, 0),
                    ("00000000000000a3", 32, 32, 1, 0), ("00000000000000a4", 16, 16, 0, 0),
                    ("00000000000000a5", 16, 16, 0, 0)]
            with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
                for h, tw, th, lin, alpha in rows:
                    w.writerow([h, "0x0F", tw, th, 1, lin, alpha, 1, 0])
            rng = np.random.default_rng(1)
            src = {}
            for h, tw, th, lin, alpha in rows:
                img = rng.integers(0, 256, (th, tw, 4), dtype=np.uint8)
                if not alpha:
                    img[..., 3] = 255
                src[h] = img
                tu.write_png(os.path.join(dump, h + ".png"), img)
            fake = os.path.join(d, "fake_esrgan.py")
            with open(fake, "w") as f:
                f.write("#!" + sys.executable + "\n" + FAKE % os.path.join(ROOT, "tools"))
            os.chmod(fake, 0o755)
            with open(os.path.join(d, "reject.json"), "w") as f:
                json.dump({"rejected": ["00000000000000a5"]}, f)
            tu.main(["--dump", dump, "--out", out, "--model", "fake", "--esrgan", fake, "--pad", "4",
                     "--reject", os.path.join(d, "reject.json")])
            pack = os.path.join(out, "fake")
            names = sorted(n for n in os.listdir(pack) if n.endswith(".png"))
            self.assertEqual(names, ["00000000000000a1.png", "00000000000000a4.png"])   # small, linear, rejected out
            up = tu.read_png(os.path.join(pack, "00000000000000a1.png"))
            want = np.repeat(np.repeat(src["00000000000000a1"], 4, axis=0), 4, axis=1)
            self.assertEqual(up.shape, (64, 128, 4))
            self.assertTrue(np.array_equal(up, want))   # color and the separately upscaled alpha
            with open(os.path.join(pack, "selection.csv")) as f:
                status = {r["hash"]: r["status"] for r in csv.DictReader(f)}
            self.assertEqual(status["00000000000000a2"], "skipped:small")
            self.assertEqual(status["00000000000000a3"], "skipped:linear")
            self.assertEqual(status["00000000000000a5"], "skipped:rejected")

    def test_resolve_model(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ("1x_DEDXT.pth", "4x-GameAI-2-0.pth", "4xTextures_GTAV_rgt-s.pth",
                      "4xTextures_GTAV_rgt-s.safetensors", "notes.txt"):
                open(os.path.join(d, n), "w").close()
            self.assertEqual(os.path.basename(tu.resolve_model(d, "1x-DEDXT")), "1x_DEDXT.pth")
            self.assertEqual(os.path.basename(tu.resolve_model(d, "4x-gameai-2-0")), "4x-GameAI-2-0.pth")
            self.assertEqual(os.path.basename(tu.resolve_model(d, "4x-Textures-GTAV-rgt-s")),
                             "4xTextures_GTAV_rgt-s.safetensors")   # safetensors preferred, '-'/'_' ignored
            with self.assertRaises(SystemExit):
                tu.resolve_model(d, "4x-Missing")

    def test_spandrel_backend_chain(self):
        with tempfile.TemporaryDirectory() as d:
            dump, src = make_dump(d, [("00000000000000b1", 32, 16, 0, 1), ("00000000000000b2", 16, 16, 0, 0)])
            models = os.path.join(d, "models")
            os.makedirs(models)
            for n in ("1x_DEDXT.pth", "4x-GameAI-2-0.safetensors"):
                open(os.path.join(models, n), "w").close()
            fake = os.path.join(d, "fake_infer.py")
            with open(fake, "w") as f:
                f.write(FAKE_INFER % os.path.join(ROOT, "tools"))
            out = os.path.join(d, "pack")
            tu.main(["--dump", dump, "--out", out, "--backend", "spandrel", "--chain", "1x-DEDXT,4x-GameAI-2-0",
                     "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4"])
            pack = os.path.join(out, "1x-DEDXT+4x-GameAI-2-0")
            up = tu.read_png(os.path.join(pack, "00000000000000b1.png"))
            self.assertTrue(np.array_equal(up, np.repeat(np.repeat(src["00000000000000b1"], 4, axis=0), 4, axis=1)))
            self.assertTrue(os.path.exists(os.path.join(pack, "00000000000000b2.png")))
            with open(os.path.join(d, "argv.json")) as f:
                argv = json.load(f)
            paths = [argv[i + 1] for i, a in enumerate(argv) if a == "--model"]
            self.assertEqual([os.path.basename(p) for p in paths], ["1x_DEDXT.pth", "4x-GameAI-2-0.safetensors"])
            self.assertEqual([n for n in os.listdir(out) if n.startswith(".")], [])   # work dir cleaned up

    def test_classes_and_presets_loading(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "classes.json")
            with open(p, "w") as f:
                json.dump({"textures": {"AA": {"class": "fx"}, "bb": {"class": "ui_font_hud"}}}, f)
            classes = tu.load_classes(p)
            self.assertEqual(classes, {"aa": "fx", "bb": "ui_font_hud"})
            q = os.path.join(d, "presets.json")
            with open(q, "w") as f:
                json.dump({"fx": {"pad": 2, "chain": ["4x-Other"]}}, f)
            presets = tu.load_presets(q)
            self.assertEqual(presets["fx"]["pad"], 2)
            self.assertEqual(presets["fx"]["alpha"], "separate")          # merged over the table
            self.assertEqual(tu.PRESETS["fx"]["pad"], 8)                   # the table is not modified
            self.assertEqual(tu.preset_for("aa", classes, presets)[0], "fx")
            self.assertEqual(tu.preset_for("zz", classes, presets), (None, tu.DEFAULT_PRESET))
            with open(q, "w") as f:
                json.dump({"fx": {"padding": 2}}, f)
            with self.assertRaises(SystemExit):
                tu.load_presets(q)
        self.assertTrue(set(tu.PRESETS) >= {"ui_font_hud", "environment_tiling", "unknown", "character"})

    def test_old_character_classes_and_presets(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "classes.json")
            with open(p, "w") as f:
                json.dump({"textures": {"aa": {"class": "character_skin_face"}, "bb": "character_hair"}}, f)
            self.assertEqual(tu.load_classes(p), {"aa": "character", "bb": "character"})
            q = os.path.join(d, "presets.json")
            with open(q, "w") as f:
                json.dump({"character_skin_face": {"pad": 3}, "character_cloth_leather": {"pad": 5}}, f)
            presets = tu.load_presets(q)
            self.assertEqual(presets["character_skin"]["pad"], 3)
            self.assertEqual(presets["character"]["pad"], 5)
            self.assertNotIn("character_skin_face", presets)

    def test_region_mask_and_composite(self):
        regions = {"aa": {"size": [16, 16], "regions": {"bbox": {"face": [2, 2, 6, 6], "hands": [10, 12, 14, 16],
                                                                  "torso": [0, 0, 16, 16], "hair": None}}}}
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "classes.json")
            with open(p, "w") as f:
                json.dump({"textures": dict(regions, bb={"class": "fx"})}, f)
            boxes = tu.load_skin_regions(p)
        self.assertEqual(boxes, {"aa": ([16, 16], [[2, 2, 6, 6], [10, 12, 14, 16]])})
        m = tu.region_mask(32, 32, *boxes["aa"])                      # dump twice the WAD size
        self.assertEqual(m.shape, (32, 32))
        self.assertEqual(m[4:12, 4:12].min(), 1.0)
        self.assertEqual(m[20:24, 0:16].max(), 0.0)
        self.assertEqual(m[24:32, 20:28].min(), 1.0)
        base = np.zeros((128, 128, 3), np.uint8)
        skin = np.full((128, 128, 3), 200, np.uint8)
        out = tu.composite(base, skin, m, 4)
        self.assertTrue((out[24:40, 24:40] == 200).all())             # inside the face box: skin preset
        self.assertTrue((out[80:92, 0:64] == 0).all())                # outside: base (cloth) preset
        edge = out[15, 30, 0]                                          # feathered border
        self.assertTrue(0 <= edge <= 200)
        self.assertTrue(tu.PRESETS["ui_font_hud"]["skip"])
        self.assertFalse(tu.binary_alpha(np.array([0, 128, 255], np.uint8)))
        self.assertTrue(tu.binary_alpha(np.array([0, 5, 250, 255], np.uint8)))

    def test_pipeline_with_classes(self):
        with tempfile.TemporaryDirectory() as d:
            dump, src = make_dump(d, [("00000000000000c1", 16, 16, 0, 1), ("00000000000000c2", 16, 16, 0, 1),
                                      ("00000000000000c3", 32, 16, 0, 0), ("00000000000000c4", 16, 16, 0, 0)])
            cut = src["00000000000000c2"]
            cut[..., 3] = np.where(cut[..., 3] >= 128, 255, 0)            # a binary cutout
            tu.write_png(os.path.join(dump, "00000000000000c2.png"), cut)
            classes = os.path.join(d, "classes.json")
            with open(classes, "w") as f:
                json.dump({"textures": {"00000000000000c1": {"class": "ui_font_hud"},
                                        "00000000000000c2": {"class": "decal_alpha"},
                                        "00000000000000c3": {"class": "environment_unique"}}}, f)
            models = os.path.join(d, "models")
            os.makedirs(models)
            for n in ("1x_DEDXT.pth", "4x-GameAI-2-0.pth", "4x-Other.pth"):
                open(os.path.join(models, n), "w").close()
            presets = os.path.join(d, "presets.json")
            with open(presets, "w") as f:
                json.dump({"environment_unique": {"chain": ["4x-Other"]}}, f)
            fake = os.path.join(d, "fake_infer.py")
            with open(fake, "w") as f:
                f.write(FAKE_INFER.replace('"w") as f:\n    json.dump(a[1:], f)', '"a") as f:\n    f.write(json.dumps(a[1:]) + "\\n")')
                        % os.path.join(ROOT, "tools"))
            out = os.path.join(d, "pack")
            tu.main(["--dump", dump, "--out", out, "--backend", "spandrel", "--chain", "1x-DEDXT,4x-GameAI-2-0",
                     "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4",
                     "--classes", classes, "--presets", presets])
            pack = os.path.join(out, "1x-DEDXT+4x-GameAI-2-0")
            names = sorted(n for n in os.listdir(pack) if n.endswith(".png"))
            self.assertEqual(names, ["00000000000000c2.png", "00000000000000c3.png", "00000000000000c4.png"])
            for h in ("00000000000000c2", "00000000000000c3", "00000000000000c4"):
                up = tu.read_png(os.path.join(pack, h + ".png"))
                want = np.repeat(np.repeat(src[h] if h != "00000000000000c2" else cut, 4, axis=0), 4, axis=1)
                self.assertTrue(np.array_equal(up, want), h)               # edge or wrap padding crops back
            with open(os.path.join(d, "argv.json")) as f:
                runs = [json.loads(line) for line in f]
            chains = sorted(tuple(os.path.basename(a[i + 1]) for i, x in enumerate(a) if x == "--model") for a in runs)
            self.assertEqual(chains, [("1x_DEDXT.pth", "4x-GameAI-2-0.pth"), ("4x-Other.pth",)])
            with open(os.path.join(pack, "selection.csv")) as f:
                rows = {r["hash"]: r for r in csv.DictReader(f)}
            self.assertEqual(rows["00000000000000c1"]["status"], "skipped:class:ui_font_hud")
            self.assertEqual(rows["00000000000000c3"]["class"], "environment_unique")
            self.assertEqual(rows["00000000000000c4"]["class"], "")

    def test_character_skin_regions_composited(self):
        # the skin preset has its own chain: skin boxes come from that run, the rest from the main run
        with tempfile.TemporaryDirectory() as d:
            dump, src = make_dump(d, [("00000000000000d1", 32, 32, 0, 0)])
            classes = os.path.join(d, "classes.json")
            with open(classes, "w") as f:
                json.dump({"textures": {"00000000000000d1": {"class": "character", "size": [32, 32],
                                                             "regions": {"bbox": {"face": [0, 0, 16, 16]}}}}}, f)
            models = os.path.join(d, "models")
            os.makedirs(models)
            for n in ("4x-Main.pth", "4x-Skin.pth"):
                open(os.path.join(models, n), "w").close()
            presets = os.path.join(d, "presets.json")
            with open(presets, "w") as f:
                json.dump({"character_skin": {"chain": ["4x-Skin"]}}, f)
            fake = os.path.join(d, "fake_infer.py")
            with open(fake, "w") as f:      # the "skin" model inverts the colours, so its texels show
                f.write(FAKE_INFER.replace("img, 4, axis=0), 4, axis=1)",
                                           "img if not any('Skin' in x for x in a) else 255 - img, 4, axis=0), 4, axis=1)")
                        % os.path.join(ROOT, "tools"))
            out = os.path.join(d, "pack")
            tu.main(["--dump", dump, "--out", out, "--backend", "spandrel", "--chain", "4x-Main",
                     "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4",
                     "--classes", classes, "--presets", presets])
            up = tu.read_png(os.path.join(out, "4x-Main", "00000000000000d1.png"))
            want = np.repeat(np.repeat(src["00000000000000d1"], 4, axis=0), 4, axis=1)
            self.assertTrue(np.array_equal(up[:56, :56, :3], 255 - want[:56, :56, :3]))   # face box: skin run
            self.assertTrue(np.array_equal(up[72:, :, :3], want[72:, :, :3]))              # elsewhere: main run
            self.assertTrue(np.array_equal(up[:, 72:, :3], want[:, 72:, :3]))

    def _classes_dir(self, d, entries):
        """full/<hash>.png images (no manifest.csv) + classes.json; entries: (hash, w, h, format,
        class, alpha). Returns (image dir, classes path, {hash: source RGBA})."""
        full = os.path.join(d, "full")
        os.makedirs(full)
        rng = np.random.default_rng(5)
        src, tex = {}, {}
        for h, w, hh, fmt, cls, alpha in entries:
            img = rng.integers(0, 256, (hh, w, 4), dtype=np.uint8)
            if not alpha:
                img[..., 3] = 255
            src[h] = img
            tu.write_png(os.path.join(full, h + ".png"), img)
            tex[h] = {"class": cls, "size": [w, hh], "format": fmt, "alpha": "blend" if alpha else "opaque"}
        classes = os.path.join(d, "classes.json")
        with open(classes, "w") as f:
            json.dump({"version": 1, "textures": tex}, f)
        return full, classes, src

    def test_classes_source_review_and_keep_original(self):
        with tempfile.TemporaryDirectory() as d:
            full, classes, src = self._classes_dir(d, [
                ("00000000000000e1", 16, 16, "DXT1", "prop", 0),
                ("00000000000000e2", 16, 16, "DXT5", "prop", 1),                 # overridden to ui_font_hud
                ("00000000000000e3", 16, 16, "DXT1", "environment_tiling", 0),   # keep-original
                ("00000000000000e4", 16, 16, "0x12", "ui_screen", 0),            # linear
                ("fail00000000000e", 16, 16, "DXT1", "prop", 0)])                # upscaler gives no output
            review = os.path.join(d, "review.json")
            with open(review, "w") as f:
                json.dump({"overrides": {"00000000000000e2": "ui_font_hud"}, "confirmed": ["00000000000000e1"]}, f)
            keep = os.path.join(d, "keep.json")
            with open(keep, "w") as f:
                json.dump({"note": "x", "hashes": ["00000000000000E3"]}, f)
            models = os.path.join(d, "models")
            os.makedirs(models)
            open(os.path.join(models, "4x-A.pth"), "w").close()
            fake = write_fake(d, FAKE_INFER_SCALED)
            out = os.path.join(d, "pack")
            os.makedirs(os.path.join(out, "v4"))
            tu.write_png(os.path.join(out, "v4", "00000000000000e3.png"), src["00000000000000e3"])   # stale
            rc = tu.main(["--dump", full, "--out", out, "--name", "v4", "--backend", "spandrel", "--chain", "4x-A",
                          "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4",
                          "--classes", classes, "--review", review, "--keep-original", keep])
            self.assertEqual(rc, 2)                                         # one texture failed
            pack = os.path.join(out, "v4")
            names = sorted(n for n in os.listdir(pack) if n.endswith(".png"))
            self.assertEqual(names, ["00000000000000e1.png"])
            up = tu.read_png(os.path.join(pack, "00000000000000e1.png"))
            self.assertTrue(np.array_equal(up, np.repeat(np.repeat(src["00000000000000e1"], 4, 0), 4, 1)))
            with open(os.path.join(pack, "selection.csv")) as f:
                rows = {r["hash"]: r for r in csv.DictReader(f)}
            self.assertEqual(rows["00000000000000e2"]["status"], "skipped:class:ui_font_hud")
            self.assertEqual(rows["00000000000000e2"]["class"], "ui_font_hud")
            self.assertEqual(rows["00000000000000e3"]["status"], "skipped:keep-original")
            self.assertEqual(rows["00000000000000e4"]["status"], "skipped:linear")
            self.assertEqual(rows["fail00000000000e"]["status"], "failed:no-output")
            self.assertEqual(rows["00000000000000e1"]["status"], "upscaled")

    def test_chain8_alpha_chain_and_bleed(self):
        with tempfile.TemporaryDirectory() as d:
            full, classes, src = self._classes_dir(d, [
                ("00000000000000f1", 16, 16, "DXT5", "environment_tiling", 1),   # 16 * 8 <= 256: 8x chain
                ("00000000000000f2", 64, 64, "DXT1", "environment_tiling", 0)])  # 64 * 8 > 256: 4x chain
            models = os.path.join(d, "models")
            os.makedirs(models)
            for n in ("4x-A.pth", "2x-B.pth", "4x-C.pth"):
                open(os.path.join(models, n), "w").close()
            presets = os.path.join(d, "presets.json")
            with open(presets, "w") as f:
                json.dump({"environment_tiling": {"chain": ["4x-A"], "chain8": ["4x-A", "2x-B"],
                                                  "alpha_chain": ["4x-C"], "bleed": True, "anchor": 0}}, f)
            fake = write_fake(d, FAKE_INFER_SCALED)
            out = os.path.join(d, "pack")
            rc = tu.main(["--dump", full, "--out", out, "--name", "v4", "--backend", "spandrel", "--chain", "4x-A",
                          "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4",
                          "--max-size", "256", "--classes", classes, "--presets", presets])
            self.assertEqual(rc, 0)
            self.assertEqual(sorted(run_models(a) for a in read_runs(d)),
                             [("4x-A.pth",), ("4x-A.pth", "2x-B.pth"), ("4x-C.pth",)])
            f1 = tu.read_png(os.path.join(out, "v4", "00000000000000f1.png"))
            self.assertEqual(f1.shape, (128, 128, 4))
            bled = tu.bleed(src["00000000000000f1"])
            self.assertTrue(np.array_equal(f1[..., :3], np.repeat(np.repeat(bled[..., :3], 8, 0), 8, 1)))
            a4 = np.repeat(np.repeat(src["00000000000000f1"][..., 3], 4, 0), 4, 1).astype(np.int16)
            a8 = f1[..., 3].astype(np.int16)
            self.assertLessEqual(np.abs(a8[1::2, 1::2] - a4).max(), 112)   # bilinear 4x -> 8x: <= 7/16 of a step
            f2 = tu.read_png(os.path.join(out, "v4", "00000000000000f2.png"))
            self.assertEqual(f2.shape, (256, 256, 4))
            self.assertTrue((f2[..., 3] == 255).all())

    def test_resume_with_keep_work(self):
        with tempfile.TemporaryDirectory() as d:
            full, classes, src = self._classes_dir(d, [("00000000000000a1", 16, 16, "DXT1", "prop", 0),
                                                        ("00000000000000a2", 16, 16, "DXT5", "prop", 1)])
            models = os.path.join(d, "models")
            os.makedirs(models)
            open(os.path.join(models, "4x-A.pth"), "w").close()
            fake = write_fake(d, FAKE_INFER_SCALED)
            out, work = os.path.join(d, "pack"), os.path.join(d, "work")
            argv = ["--dump", full, "--out", out, "--name", "v4", "--backend", "spandrel", "--chain", "4x-A",
                    "--model-dir", models, "--python", sys.executable, "--infer", fake, "--pad", "4",
                    "--classes", classes, "--keep-work", work]
            self.assertEqual(tu.main(argv), 0)
            first = tu.read_png(os.path.join(out, "v4", "00000000000000a2.png"))
            raw = os.path.join(work, "out", "00000000000000a1.png")
            stamp = os.stat(raw).st_mtime_ns
            os.remove(os.path.join(out, "v4", "00000000000000a2.png"))
            self.assertEqual(tu.main(argv), 0)
            self.assertIn("--skip-existing", read_runs(d)[-1])
            self.assertEqual(os.stat(raw).st_mtime_ns, stamp)                  # finished outputs kept
            self.assertTrue(np.array_equal(tu.read_png(os.path.join(out, "v4", "00000000000000a2.png")), first))

    def test_bleed_refine_and_sharpen_alpha(self):
        img = np.zeros((4, 4, 4), np.uint8)
        img[:, 2:] = (200, 100, 50, 255)
        b = tu.bleed(img)
        self.assertTrue((b[:, :2, :3] == (200, 100, 50)).all())             # color under alpha 0 filled
        self.assertTrue((b[..., 3] == img[..., 3]).all())
        src = np.random.default_rng(6).integers(40, 200, (16, 16, 3), dtype=np.uint8)
        up = np.clip(np.repeat(np.repeat(src, 4, 0), 4, 1).astype(np.int16) + 30, 0, 255).astype(np.uint8)
        fixed = tu.refine(src, up, 1.0, 1.0)
        self.assertLess(abs(float(fixed.mean()) - float(src.mean())), 2.0)   # tint shift anchored away
        a = tu.sharpen_alpha(np.array([0, 64, 128, 192, 255], np.uint8))
        self.assertEqual((int(a[0]), int(a[1]), int(a[3]), int(a[4])), (0, 0, 255, 255))
        self.assertTrue(100 < a[2] < 155)


if __name__ == "__main__":
    unittest.main()
