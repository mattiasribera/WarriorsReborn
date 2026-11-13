"""Tests of tools/glyphs/make_button_icons.py on a synthetic atlas and synthetic pak bytes (no game
data): the rect table is found, only the button cells change, the options are validated, the
position style marks a different dot per face button, and the script names no console brand.
Skipped without Pillow where an image is needed."""
import codecs
import importlib.util
import os
import random
import struct
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tools", "glyphs", "make_button_icons.py")

try:
    from PIL import Image
except ImportError:   # pragma: no cover
    Image = None


def load_module():
    spec = importlib.util.spec_from_file_location("make_button_icons", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SIZE = 256
COUNT = 0x173


def synthetic_rects(mod):
    """Every record a 1x1-texel rect in the top-left corner, except the button cells: a grid of
    24 x 28 cells from y = 128."""
    recs = [(0.0, 0.0, 1 / SIZE, 1 / SIZE)] * COUNT
    idx = [c + mod.TABLE_BASE for c in mod.GLYPH_CONTROLS] + list(mod.SPRITES)
    for k, i in enumerate(idx):
        x, y = 4 + (k % 9) * 27, 128 + (k // 9) * 30
        recs[i] = (x / SIZE, y / SIZE, (x + 24) / SIZE, (y + 28) / SIZE)
    return recs


def synthetic_pak(recs, at):
    head = bytes(random.Random(1).randrange(256) for _ in range(at - 16))
    body = struct.pack("<IIII", COUNT, 0x5E, 0, 0) + b"".join(struct.pack("<4f", *r) for r in recs)
    return head + body + b"\xee" * 64


def synthetic_atlas():
    rnd = random.Random(7)
    img = Image.new("RGBA", (SIZE, SIZE))
    img.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), 255) for _ in range(SIZE * SIZE)])
    return img


class TableTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def test_every_control_has_a_shape(self):
        for ctl in set(self.mod.GLYPH_CONTROLS.values()) | set(self.mod.SPRITES.values()):
            self.assertTrue(ctl in self.mod.FACE or ctl in self.mod.SHAPE, ctl)

    def test_face_positions_are_distinct(self):
        self.assertEqual(len(set(self.mod.FACE_POS.values())), 4)

    def test_table_found_at_known_offset_and_by_scan(self):
        recs = synthetic_rects(self.mod)
        for at in (self.mod.TABLE_OFFSET, 0x2000):
            got = self.mod.read_rects(synthetic_pak(recs, at))
            self.assertEqual(len(got), COUNT)
            self.assertAlmostEqual(got[0x9E + self.mod.TABLE_BASE][0], recs[0x9E + self.mod.TABLE_BASE][0], places=5)

    def test_missing_table_exits(self):
        with self.assertRaises(SystemExit):
            self.mod.read_rects(b"\x00" * 4096)

    def test_style_validation(self):
        Style = self.mod.Style
        with self.assertRaises(ValueError):
            Style(face_labels=("1", "2", "3"))
        with self.assertRaises(ValueError):
            Style(bumper_labels=("L", ""))
        with self.assertRaises(ValueError):
            Style(theme="neon")
        with self.assertRaises(ValueError):
            Style(face="shapes")
        with self.assertRaises(ValueError):
            self.mod.parse_color("#12345")
        self.assertEqual(self.mod.parse_color("#ff8000"), (255, 128, 0))
        s = Style(face="labels", face_labels=("1", "2", "3", "4"))
        self.assertEqual(s.face_labels, {"A": "1", "B": "2", "X": "3", "Y": "4"})

    def test_bad_option_is_a_clean_exit(self):
        with self.assertRaises(SystemExit):
            self.mod.main(["--base", "x.png", "--pak", "x.pak", "--out", "o", "--face-labels", "1,2"])

    def test_no_console_brand_in_the_script(self):
        with open(SCRIPT, encoding="utf-8") as f:
            text = f.read().lower()
        # brand and shape words, rot13 so this test does not itself contain them
        for word in (codecs.decode(w, "rot13") for w in ("cynlfgngvba", "qhnyfubpx", "fbal", "gevnatyr", "pvepyr", "fdhner", "pebff", "avagraqb")):
            self.assertNotIn(word, text, word)


@unittest.skipIf(Image is None, "Pillow not installed")
class AtlasTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()
        cls.recs = synthetic_rects(cls.mod)
        cls.base = synthetic_atlas()

    def test_only_button_cells_change(self):
        out = self.mod.make_atlas(self.base, self.recs, self.mod.Style(), None)
        boxes = [b for b, _, _ in self.mod.targets(self.recs, SIZE, SIZE)]
        inside = lambda x, y: any(b[0] <= x < b[2] and b[1] <= y < b[3] for b in boxes)
        bp, op = self.base.load(), out.load()
        changed_outside = sum(1 for y in range(SIZE) for x in range(SIZE) if not inside(x, y) and bp[x, y] != op[x, y])
        self.assertEqual(changed_outside, 0)
        for b in boxes:
            self.assertNotEqual(self.base.crop(b).tobytes(), out.crop(b).tobytes(), b)

    def test_position_style_differs_per_face_button(self):
        cells = {ctl: self.mod.draw_cell(ctl, 48, 48, (0.0, 1.0), self.mod.Style(), None).tobytes()
                 for ctl in self.mod.FACE}
        self.assertEqual(len(set(cells.values())), 4)

    def test_themes_and_labels_change_the_art(self):
        dark = self.mod.draw_cell("WHITE", 48, 48, (0.0, 1.0), self.mod.Style(), None).tobytes()
        light = self.mod.draw_cell("WHITE", 48, 48, (0.0, 1.0), self.mod.Style(theme="light"), None).tobytes()
        other = self.mod.draw_cell("WHITE", 48, 48, (0.0, 1.0), self.mod.Style(bumper_labels=("LB", "RB")), None).tobytes()
        self.assertNotEqual(dark, light)
        self.assertNotEqual(dark, other)

    def test_main_writes_the_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = os.path.join(tmp, "base.png")
            pak = os.path.join(tmp, "global.pak")
            out = os.path.join(tmp, "pack")
            self.base.save(base)
            with open(pak, "wb") as f:
                f.write(synthetic_pak(self.recs, self.mod.TABLE_OFFSET))
            rc = self.mod.main(["--base", base, "--pak", pak, "--out", out, "--face", "labels",
                                "--face-labels", "1,2,3,4", "--face-colors", "#40e0a0,#ff6666,#7cb2e8,#ff69f8",
                                "--sheet", os.path.join(tmp, "sheet.png")])
            self.assertEqual(rc, 0)
            img = Image.open(os.path.join(out, self.mod.ATLAS_KEY + ".png"))
            self.assertEqual(img.size, (SIZE, SIZE))
            self.assertTrue(os.path.isfile(os.path.join(tmp, "sheet.png")))


if __name__ == "__main__":
    unittest.main()
