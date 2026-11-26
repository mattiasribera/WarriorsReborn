"""Tests of tools/glyphs/make_ps_glyphs.py on a synthetic atlas and synthetic pak bytes (no game
data): the rect table is found, only the listed cells change, each repainted cell is centred,
and the white theme is the black theme with RGB inverted. The D-pad: the main atlas's arrows and
the big font atlas's pulsing D-pad sprites (its own rect table and texture, found in a synthetic
pak) get a D-pad with the drawn direction lit. Skipped without Pillow."""
import importlib.util
import os
import random
import re
import struct
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(ROOT, "tools", "glyphs", "make_ps_glyphs.py")

try:
    from PIL import Image
except ImportError:   # pragma: no cover
    Image = None


def load_module():
    spec = importlib.util.spec_from_file_location("make_ps_glyphs", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SIZE = 256
COUNT = 0x173


def synthetic_rects(mod):
    """Every record a 1x1-texel rect in the top-left corner, except the listed cells: a grid of
    24 x 28 cells (one texel apart) from y = 128."""
    recs = [(0.0, 0.0, 1 / SIZE, 1 / SIZE)] * COUNT
    idx = [c + mod.TABLE_BASE for c in list(mod.CODES) + list(mod.DPAD_CODES)] + list(mod.SPRITES)
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


# What the player presses on a PlayStation-layout pad for each glyph code, through the runtime's
# SDL -> Xbox mapping (L1/R1 = SDL shoulders = White/Black, L2/R2 = SDL triggers, L3/R3 = stick
# clicks). The alternates are the localized letter badges: D = right, I/G/S = left.
EXPECTED_PS = {
    0x9F: "square", 0x9E: "cross", 0x9D: "circle", 0x96: "triangle",
    0x9C: "R1", 0x91: "L1", 0x95: "L2", 0xA0: "R2", 0x94: "L3", 0x92: "R3",
    0x97: "START", 0x93: "SELECT",
    0xA2: "R2", 0xA3: "L2", 0xA4: "L2", 0xA5: "L2",
    0xA6: "L3", 0xA7: "L3", 0xB3: "L3", 0xB2: "R3",
}
# The main atlas's D-pad cells draw these arrows (crops of the 1x atlas; runtime/src/kbm_glyphs.c).
EXPECTED_DPAD = {0x98: "left", 0x99: "up", 0x9A: "right", 0x9B: "down"}
KBM_DIRS = {"IN_LEFT": "left", "IN_UP": "up", "IN_RIGHT": "right", "IN_DOWN": "down"}
XBOX_NAMES = {"IN_A": "A", "IN_B": "B", "IN_X": "X", "IN_Y": "Y", "IN_BLACK": "BLACK", "IN_WHITE": "WHITE",
              "IN_START": "START", "IN_BACK": "BACK", "IN_LT": "LT", "IN_RT": "RT",
              "IN_LTHUMB": "LTHUMB", "IN_RTHUMB": "RTHUMB"}


def read_src(*parts):
    with open(os.path.join(ROOT, *parts), encoding="utf-8") as f:
        return f.read()


class PsGlyphTableTest(unittest.TestCase):
    """The glyph code -> PS icon table (no Pillow needed: the module loads without it)."""

    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()

    def test_ps_icon_per_glyph_code(self):
        self.assertEqual(self.mod.CODES, EXPECTED_PS)

    def test_glyph_controls_match_the_kbm_painter(self):
        """Every button cell runtime/src/kbm_glyphs.c paints keys for (the 12 English cells and the
        8 Spanish/French/Italian trigger and stick-click letter badges) gets the same Xbox control
        here, so a localized prompt is repainted too."""
        src = read_src("runtime", "src", "kbm_glyphs.c")
        pairs = re.findall(r"\{0x([0-9a-f]+) \+ KBM_RECT_BASE, (IN_[A-Z]+),", src)
        kbm = {int(c, 16): XBOX_NAMES[n] for c, n in pairs if n in XBOX_NAMES}
        self.assertEqual(len(kbm), 20)
        self.assertEqual(set(kbm), set(self.mod.GLYPH_CONTROLS))
        for code, ctl in kbm.items():
            self.assertEqual(self.mod.GLYPH_CONTROLS[code], ctl, hex(code))

    def test_dpad_cells_match_the_kbm_painter(self):
        """The main atlas's four D-pad arrows and the big font's five D-pad sprites (neutral, then
        down/left/right/up lit: <SDD> <SDL> <SDR> <SDU> draw code 2 or 3-6 at record code - 1 +
        0x100) are the cells the keyboard/mouse painter repaints too."""
        self.assertEqual(self.mod.DPAD_CODES, EXPECTED_DPAD)
        src = read_src("runtime", "src", "kbm_glyphs.c")
        pairs = re.findall(r"\{0x([0-9a-f]+) \+ KBM_RECT_BASE, (IN_[A-Z]+),", src)
        kbm = {int(c, 16): KBM_DIRS[n] for c, n in pairs if n in KBM_DIRS}
        self.assertEqual(kbm, EXPECTED_DPAD)
        self.assertEqual(self.mod.BIG_SPRITES, {257: None, 258: "down", 259: "left", 260: "right", 261: "up"})
        big = re.findall(r"\{KBM_BIG_DPAD_BASE \+ (\d), (-1|IN_[A-Z]+),", src)
        self.assertEqual({0x100 + int(k): None if c == "-1" else KBM_DIRS[c] for k, c in big}, self.mod.BIG_SPRITES)

    def test_ps_buttons_follow_the_runtime_sdl_mapping(self):
        """runtime/src/video.c: SDL buttons 9/10 (left/right shoulder = L1/R1) feed White/Black,
        axes 4/5 (left/right trigger = L2/R2) feed LT/RT, buttons 7/8 (stick clicks) L3/R3."""
        src = read_src("runtime", "src", "video.c")
        body = re.search(r"bits\[15\] = \{[^\n]*\n(.*?)\};", src, re.S).group(1)
        bits = [t.strip() for t in body.replace("\n", " ").split(",")]
        sdl_ps = {0: "cross", 1: "circle", 2: "square", 3: "triangle", 4: "SELECT", 6: "START",
                  7: "L3", 8: "R3", 9: "L1", 10: "R1"}
        for b, ps in sdl_ps.items():
            self.assertEqual(self.mod.PS_BUTTON[XBOX_NAMES[bits[b]]], ps, f"SDL button {b} = {bits[b]}")
        self.assertIn("p.analog[IN_LT - IN_A] = (uint8_t)(ax[4]", src)
        self.assertIn("p.analog[IN_RT - IN_A] = (uint8_t)(ax[5]", src)
        self.assertEqual((self.mod.PS_BUTTON["LT"], self.mod.PS_BUTTON["RT"]), ("L2", "R2"))


@unittest.skipIf(Image is None, "Pillow is not installed")
class PsGlyphsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()
        cls.recs = synthetic_rects(cls.mod)
        cls.base = synthetic_atlas()
        cls.font = cls.mod.find_font()
        cls.black = cls.mod.make_atlas(cls.base, cls.recs, "black", cls.font)
        cls.white = cls.mod.make_atlas(cls.base, cls.recs, "white", cls.font)
        cls.boxes = [b for b, _, _ in cls.mod.targets(cls.recs, SIZE, SIZE)]

    def test_rect_table_found_at_known_offset_and_by_scan(self):
        for at in (self.mod.TABLE_OFFSET, 0x1234):
            got = self.mod.read_rects(synthetic_pak(self.recs, at))
            self.assertEqual(len(got), COUNT)
            for g, r in zip(got, self.recs):
                for a, b in zip(g, r):
                    self.assertAlmostEqual(a, b, places=6)

    def test_missing_table_is_an_error(self):
        with self.assertRaises(SystemExit):
            self.mod.read_rects(b"\x00" * 4096)

    def test_all_listed_cells_targeted(self):
        self.assertEqual(len(self.boxes), len(self.mod.CODES) + len(self.mod.DPAD_CODES) + len(self.mod.SPRITES))

    def test_dpad_arrows_light_their_direction(self):
        """Each main-atlas D-pad cell is a D-pad with the arrow it stood for lit: the lit arm is the
        light (ink) side in the black theme, the dark side in the white theme."""
        w = h = SIZE
        for code, d in EXPECTED_DPAD.items():
            box = self.mod.cell_box(self.recs[code + self.mod.TABLE_BASE], w, h)
            self.assertEqual(lit_side(self.black.crop(box)), d, hex(code))
            self.assertEqual(lit_side(self.white.crop(box), dark=True), d, hex(code))

    def test_only_listed_rects_change(self):
        inside = set()
        for x0, y0, x1, y1 in self.boxes:
            inside.update((x, y) for y in range(y0, y1) for x in range(x0, x1))
        for out in (self.black, self.white):
            self.assertEqual(out.size, self.base.size)
            a, b = self.base.load(), out.load()
            changed_outside = [(x, y) for y in range(SIZE) for x in range(SIZE) if (x, y) not in inside and a[x, y] != b[x, y]]
            self.assertEqual(changed_outside, [])
            for box in self.boxes:
                self.assertNotEqual(self.base.crop(box).tobytes(), out.crop(box).tobytes())

    def test_cells_are_centred_with_transparent_border(self):
        for box in self.boxes:
            cell = self.black.crop(box)
            cw, ch = cell.size
            bb = cell.getchannel("A").point(lambda v: 255 if v > 32 else 0).getbbox()
            self.assertIsNotNone(bb)
            self.assertLessEqual(abs((bb[0] + bb[2]) / 2 - cw / 2), 1.0, f"{box} {bb}")
            self.assertLessEqual(abs((bb[1] + bb[3]) / 2 - ch / 2), 1.0, f"{box} {bb}")
            self.assertEqual(cell.getpixel((0, 0))[3], 0)   # cleared, not the noise background

    def test_themes_invert(self):
        """Same alpha everywhere; RGB inverted wherever the 3x3 neighbourhood is opaque (at the outer
        edge premultiplied filtering mixes in transparent black, which is not theme-symmetric)."""
        a, b = self.black.load(), self.white.load()
        checked = 0
        for x0, y0, x1, y1 in self.boxes:
            for y in range(y0, y1):
                for x in range(x0, x1):
                    pa, pb = a[x, y], b[x, y]
                    self.assertEqual(pa[3], pb[3], f"{(x, y)}")
                    if x0 < x < x1 - 1 and y0 < y < y1 - 1 and all(
                            a[x + dx, y + dy][3] == 255 for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                        checked += 1
                        for c in range(3):
                            self.assertLessEqual(abs(pa[c] + pb[c] - 255), 3, f"{(x, y)} {pa} {pb}")
        self.assertGreater(checked, 1000)

    def test_black_theme_is_dark_with_light_symbol(self):
        x0, y0, x1, y1 = self.boxes[0]
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        lum = lambda p: sum(p[:3]) / 3
        # A pixel on the square's outline row is light; the button between symbol and rim is dark.
        cell = self.black.crop(self.boxes[0])
        raw = cell.tobytes()
        px = [lum(raw[i:i + 4]) for i in range(0, len(raw), 4) if raw[i + 3] == 255]
        self.assertTrue(any(v < 40 for v in px) and any(v > 215 for v in px))
        self.assertLess(sum(px) / len(px), 128)
        self.assertGreater(lum(self.white.getpixel((cx, cy))), 128)

    def test_buttons_have_no_light_rim(self):
        """Black theme: the first opaque pixels from the top of each badge are the dark button, not a
        light rim (face buttons, shoulder badges, START, the D-pad)."""
        lum = lambda p: sum(p[:3]) / 3
        font = self.mod.find_font()
        for art in ("cross", "circle", "square", "triangle", "L3", "L1", "START", "dpad:"):
            cell = self.mod.draw_cell(art, 64, 64, (0.0, 1.0), "black", font)
            col = [cell.getpixel((32, y)) for y in range(64)]
            top = next(y for y, p in enumerate(col) if p[3] == 255)
            for y in range(top, top + 3):
                self.assertLess(lum(col[y]), 60, f"{art} y={y} {col[y]}")

    def test_cli_writes_pack_and_sheet(self):
        """The main atlas, and the big font atlas from the pak (decoded and upscaled), in the pack."""
        with tempfile.TemporaryDirectory() as tmp:
            pak = os.path.join(tmp, "global.pak")
            with open(pak, "wb") as f:
                f.write(synthetic_pak(self.recs, self.mod.TABLE_OFFSET) + big_pak(big_rects()))
            base = os.path.join(tmp, "base.png")
            self.base.save(base)
            out = os.path.join(tmp, "pack")
            sheet = os.path.join(tmp, "sheet.png")
            self.assertEqual(self.mod.main(["--base", base, "--pak", pak, "--theme", "white", "--out", out, "--sheet", sheet,
                                             "--big-key", big_key(self.mod)]), 0)
            with Image.open(os.path.join(out, self.mod.ATLAS_KEY + ".png")) as got:
                self.assertEqual(got.size, (SIZE, SIZE))
            with Image.open(os.path.join(out, self.mod.BIG_ATLAS_KEY + ".png")) as got:
                self.assertEqual(got.size, (BIG_W * 4, BIG_H * 4))
            self.assertTrue(os.path.isfile(sheet))


# ---- The big font atlas ----

BIG_W, BIG_H, BIG_COUNT = 128, 64, 262


def arm_means(cell):
    """Mean luminance of the opaque texels left / right / above / below the cell's centre."""
    w, h = cell.size
    px = cell.load()
    sums = {"left": [], "right": [], "up": [], "down": []}
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 200:
                continue
            v = (r + g + b) / 3
            dx, dy = x - (w - 1) / 2, y - (h - 1) / 2
            if abs(dx) > abs(dy) + 1:
                sums["left" if dx < 0 else "right"].append(v)
            elif abs(dy) > abs(dx) + 1:
                sums["up" if dy < 0 else "down"].append(v)
    return {k: sum(v) / len(v) for k, v in sums.items() if v}


def lit_side(cell, dark=False):
    """The side (left/up/right/down) of a cell whose opaque texels are the lightest (darkest)."""
    means = arm_means(cell)
    return (min if dark else max)(means, key=means.get)


def big_rects():
    """Every record a 1x1 rect at the origin, except the five D-pad sprites: 24 x 24 cells."""
    recs = [(0.0, 0.0, 1 / BIG_W, 1 / BIG_H)] * BIG_COUNT
    for k in range(5):
        x, y = 2 + k * 25, 20
        recs[257 + k] = (x / BIG_W, y / BIG_H, (x + 24) / BIG_W, (y + 24) / BIG_H)
    return recs


def dxt5_block(rgb, alpha):
    """A 4x4 DXT5 block of one colour (RGB565) and one alpha."""
    r, g, b = rgb
    c = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3
    return bytes([alpha, alpha]) + bytes(6) + struct.pack("<HH", c, c) + bytes(4)


def big_level0():
    """DXT5 level 0 of a BIG_W x BIG_H atlas: opaque grey 4x4 blocks in rows 20..43, else clear."""
    out = b""
    for by in range(BIG_H // 4):
        for bx in range(BIG_W // 4):
            out += dxt5_block((128, 128, 128), 255 if 5 <= by < 11 else 0)
    return out


def big_pak(recs, at=0x4000):
    """A pak with a texture chunk {0x2a, size, 0, id} (level 0 at +0x90) and its font chunk
    {0x4c, size, 0, id}, vtable dword, {count, 0, 0, 0}, records; the table starts at `at`."""
    tid = 0xB7A0C1AF
    level0 = big_level0()
    tex = struct.pack("<IIII", 0x2A, 0x80 + len(level0) * 4 // 3, 0, tid) + bytes(0x80) + level0
    tex += bytes(-len(tex) % 4 + 4 * (len(level0) // 12))   # the mips, 4-aligned
    font = struct.pack("<IIII", 0x4C, 0x24 + 16 * len(recs), 0, tid) + struct.pack("<I", 0x421798)
    font += struct.pack("<IIII", len(recs), 0, 0, 0) + b"".join(struct.pack("<4f", *r) for r in recs)
    head = random.Random(3).randbytes(at - 0x24 - len(tex))
    return head + tex + font + b"\xee" * 64


def big_key(mod):
    return mod.texpack_key(0x0F, BIG_W, BIG_H, big_level0())


@unittest.skipIf(Image is None, "Pillow is not installed")
class BigFontTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_module()
        cls.key = big_key(cls.mod)
        cls.recs = big_rects()
        cls.pak = big_pak(cls.recs)
        cls.font = cls.mod.find_font()

    def test_big_table_found_at_known_offset_and_by_scan(self):
        for at in (self.mod.BIG_TABLE_OFFSET, 0x4000):
            got = self.mod.read_big_rects(big_pak(self.recs, at))
            self.assertEqual(len(got), BIG_COUNT)
            for a, b in zip(got[257], self.recs[257]):
                self.assertAlmostEqual(a, b, places=6)

    def test_main_table_is_not_the_big_one(self):
        main = synthetic_pak(synthetic_rects(self.mod), 0x861F4)
        with self.assertRaises(SystemExit):
            self.mod.read_big_rects(main)

    def test_texture_found_by_its_key_and_decoded(self):
        img = self.mod.big_texture(self.pak, self.key)
        self.assertEqual(img.size, (BIG_W, BIG_H))
        self.assertEqual(img.getpixel((10, 30)), (132, 130, 132, 255))   # RGB565 grey 128
        self.assertEqual(img.getpixel((10, 5))[3], 0)
        with self.assertRaises(SystemExit):
            self.mod.big_texture(self.pak, "0123456789abcdef")

    def test_upscale_keeps_size_ratio_and_ink(self):
        img = self.mod.big_texture(self.pak, self.key)
        up = self.mod.upscale(img, 4)
        self.assertEqual(up.size, (BIG_W * 4, BIG_H * 4))
        self.assertEqual(up.getpixel((40, 120))[3], 255)
        self.assertEqual(up.getpixel((40, 10))[3], 0)

    def test_only_the_dpad_sprites_change_and_light_their_direction(self):
        base = self.mod.upscale(self.mod.big_texture(self.pak, self.key), 4)
        w, h = base.size
        for theme in ("black", "white", "color"):
            out = self.mod.make_big_atlas(base, self.recs, theme)
            boxes = [self.mod.cell_box(self.recs[r], w, h) for r in sorted(self.mod.BIG_SPRITES)]
            inside = set()
            for x0, y0, x1, y1 in boxes:
                inside.update((x, y) for y in range(y0, y1) for x in range(x0, x1))
            a, b = base.load(), out.load()
            self.assertEqual([(x, y) for y in range(h) for x in range(w) if (x, y) not in inside and a[x, y] != b[x, y]], [])
            for r, d in self.mod.BIG_SPRITES.items():
                box = self.mod.cell_box(self.recs[r], w, h)
                cell = out.crop(box)
                self.assertNotEqual(base.crop(box).tobytes(), cell.tobytes())
                if d:
                    self.assertEqual(lit_side(cell, dark=theme == "white"), d, f"{theme} {r}")
            # Neutral: no arm lit, the four arms alike.
            means = arm_means(out.crop(self.mod.cell_box(self.recs[257], w, h)))
            self.assertLess(max(means.values()) - min(means.values()), 12, f"{theme} {means}")

    def test_cli_big_base_png_kept_at_its_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            pak = os.path.join(tmp, "global.pak")
            main_recs = synthetic_rects(self.mod)
            data = bytearray(synthetic_pak(main_recs, self.mod.TABLE_OFFSET))
            data += big_pak(self.recs)
            with open(pak, "wb") as f:
                f.write(bytes(data))
            base = os.path.join(tmp, "base.png")
            synthetic_atlas().save(base)
            bigb = os.path.join(tmp, "big.png")
            Image.new("RGBA", (BIG_W * 2, BIG_H * 2), (90, 90, 90, 255)).save(bigb)
            out = os.path.join(tmp, "pack")
            self.assertEqual(self.mod.main(["--base", base, "--pak", pak, "--out", out, "--big-base", bigb]), 0)
            with Image.open(os.path.join(out, self.mod.BIG_ATLAS_KEY + ".png")) as got:
                self.assertEqual(got.size, (BIG_W * 2, BIG_H * 2))


if __name__ == "__main__":
    unittest.main()
