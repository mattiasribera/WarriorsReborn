#!/usr/bin/env python3
"""PlayStation-style button icons for the font/icon atlas (texture-pack key 886a4c14094e9597)
and the big font atlas's D-pad sprites (912d0ecc2ada0599).

The game draws its button prompts as characters of the main font atlas. This tool repaints those
cells of a base atlas PNG (the 1x dump or an upscaled pack's file) with PlayStation-style shapes
drawn from geometry by this code (cross, circle, square, triangle, L1/L2/R1/R2, L3/R3, START,
SELECT, and a D-pad with the arrow's direction lit) and writes <out>/886a4c14094e9597.png.

The pulsing D-pad of the Revive prompt and the HUD's D-pad legends (<SDD> <SDL> <SDR> <SDU>) are
sprites of the big font atlas instead (records 257-261 of its own rect table: neutral, then down,
left, right, up lit). The tool decodes that atlas from the same global.pak (or takes --big-base),
upscales it (--big-scale, 4 by default, like the runtime's --sharp-fonts) and writes
<out>/912d0ecc2ada0599.png with those five cells repainted. Both files form one texture pack to
put before the base pack:

    xbrun ... --texture-pack <out> --texture-pack <base pack>

The cell rectangles are read at run time from the user's own System.wad0 paks/global.pak (a table
of 16-byte float records u0, v0, u1, v1; the rect of character code ch is record ch + base). No
game data is stored here: only the character codes and record indices of the button cells.

Themes: black (black button, white symbol), white (white button, black symbol) and color (black
button, face symbols in colour); none draws a rim. Labels use a system TrueType font found at run
time (--font overrides). Needs Python 3 + Pillow.

The runtime draws the same icons itself (xbrun --button-icons ps-black|ps-white|ps-color, see
runtime/src/kbm_glyphs.h) from the same global.pak, so this pack is optional; when given as the
first --texture-pack (without --pad-icons) it is used instead.
"""
import argparse
import os
import struct
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:   # the tables still load (tests); main() needs Pillow
    Image = ImageDraw = ImageFont = None

ATLAS_KEY = "886a4c14094e9597"
TABLE_OFFSET = 0x861F4      # first record in the known global.pak
TABLE_BASE = 0x5E           # record index of character code 0
SUPERSAMPLE = 8

# Character code -> the Xbox control its cell draws (checked on crops of the 1x atlas; the same
# table as runtime/src/kbm_glyphs.c). The game kept the PS2 tag names (<L1> <R2> ...) but its Xbox
# cells draw other controls: <L1> 0xa0 is the right trigger, <R2> 0x94 the left stick click, <L3>
# 0x91 White. The controller-config preset alternates are localized letter badges of the trigger
# and stick-click cells: D (right) 0xa2 trigger, 0xb2 stick; I/G/S (left) 0xa3-0xa5 trigger,
# 0xa6/0xa7/0xb3 stick.
GLYPH_CONTROLS = {
    0x9F: "X", 0x9E: "A", 0x9D: "B", 0x96: "Y",
    0x9C: "BLACK", 0x91: "WHITE", 0x97: "START", 0x93: "BACK",
    0x95: "LT", 0xA0: "RT", 0x94: "LTHUMB", 0x92: "RTHUMB",
    0xA2: "RT", 0xA3: "LT", 0xA4: "LT", 0xA5: "LT",
    0xA6: "LTHUMB", 0xA7: "LTHUMB", 0xB3: "LTHUMB", 0xB2: "RTHUMB",
}
# Xbox control -> the PlayStation button a player presses for it through the runtime's SDL mapping
# (runtime/src/video.c): SDL left/right shoulder (L1/R1) feed White/Black, the SDL triggers (L2/R2)
# the Xbox triggers, the stick clicks L3/R3, back/start SELECT/START.
PS_BUTTON = {
    "A": "cross", "B": "circle", "X": "square", "Y": "triangle",
    "WHITE": "L1", "BLACK": "R1", "LT": "L2", "RT": "R2", "LTHUMB": "L3", "RTHUMB": "R3",
    "START": "START", "BACK": "SELECT",
}
# Character code -> PlayStation art.
CODES = {code: PS_BUTTON[ctl] for code, ctl in GLYPH_CONTROLS.items()}
# The D-pad cells (<DR> <DU> <DL> <DD>: the tag names are mirrored, these are the arrows drawn) ->
# the direction lit on a PlayStation D-pad.
DPAD_CODES = {0x98: "left", 0x99: "up", 0x9A: "right", 0x9B: "down"}
FACE_CODES = (0x9F, 0x9E, 0x9D, 0x96)
# Large face-button sprites (record indices, not character codes): A, Y, X, B.
SPRITES = {79: "cross", 73: "triangle", 65: "square", 6: "circle"}

# Exact complements, so the white theme is the black theme with RGB inverted. "color" is the black
# theme with the face symbols in the classic colours. No theme draws an ink rim around the badges
# (a theme may opt back in with "rim": True; "face_rim": False then skips it on the face buttons).
THEMES = {
    "black": {"fill": (20, 20, 20), "ink": (245, 245, 245)},
    "white": {"fill": (235, 235, 235), "ink": (10, 10, 10)},
    "color": {"fill": (20, 20, 20), "ink": (245, 245, 245),
              "symbols": {"triangle": (64, 226, 160), "circle": (255, 102, 102),
                          "cross": (124, 178, 232), "square": (255, 105, 248)},
              "face_rim": False},
}

# The big font atlas (1024x512 DXT5): its rect table has no sprite records before the characters
# (header {count, 0, 0, 0}). In the D-pad sprite mode the big font draws code c at record c - 1 +
# 0x100; <SDD> <SDL> <SDR> <SDU> pulse between code 2 (neutral) and 3-6.
BIG_ATLAS_KEY = "912d0ecc2ada0599"
BIG_TABLE_OFFSET = 0x1728C4  # first record in the known global.pak
BIG_SPRITES = {257: None, 258: "down", 259: "left", 260: "right", 261: "up"}
BIG_SCALE = 4
DXT5 = 0x0F                  # Xbox texture format code of the font atlases (texpack hash input)

FONT_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
]
WINDOWS_FONTS = ["arialbd.ttf", "segoeuib.ttf"]


def find_font(override=None):
    """Path of a bold sans TrueType font, or None (Pillow's default font is used then)."""
    if override:
        if not os.path.isfile(override):
            raise SystemExit(f"font not found: {override}")
        return override
    windir = os.environ.get("WINDIR") or os.environ.get("SystemRoot") or r"C:\Windows"
    for path in FONT_CANDIDATES + [os.path.join(windir, "Fonts", n) for n in WINDOWS_FONTS]:
        if os.path.isfile(path):
            return path
    return None


def _table_at(data, rec_off, table_base=TABLE_BASE, min_count=0x100):
    """Records of a table whose 16-byte header {count, base, 0, 0} ends at rec_off, or None when it
    does not fit."""
    hdr = rec_off - 16
    if hdr < 0 or rec_off > len(data):
        return None
    count, base, z0, z1 = struct.unpack_from("<IIII", data, hdr)
    if base != table_base or z0 or z1 or not min_count <= count <= 0x1000 or rec_off + count * 16 > len(data):
        return None
    recs = [struct.unpack_from("<4f", data, rec_off + 16 * i) for i in range(count)]
    for u0, v0, u1, v1 in recs:
        if not (0.0 <= u0 <= u1 <= 1.0 and 0.0 <= v0 <= v1 <= 1.0):
            return None
    return recs


def _find_table(data, known, table_base, min_count):
    """(records, offset of the first record) of a table: the known offset first, else a scan for a
    header {count, table_base, 0, 0} followed by count records in [0, 1]; None when there is none."""
    recs = _table_at(data, known, table_base, min_count)
    if recs:
        return recs, known
    needle = struct.pack("<I", table_base) + bytes(8)
    pos = data.find(needle)
    while pos >= 0:
        if pos % 4 == 0:
            recs = _table_at(data, pos + 12, table_base, min_count)
            if recs:
                return recs, pos + 12
        pos = data.find(needle, pos + 1)
    return None


def read_rects(data):
    """Glyph rect records (u0, v0, u1, v1) from global.pak bytes: the known offset first, else a
    scan for a header {count, base 0x5e, 0, 0} followed by count records in [0, 1]."""
    found = _find_table(data, TABLE_OFFSET, TABLE_BASE, 0x100)
    if not found:
        raise SystemExit("glyph rect table not found in the pak (expected System.wad0 paks/global.pak)")
    return found[0]


def _big_table(data):
    found = _find_table(data, BIG_TABLE_OFFSET, 0, max(BIG_SPRITES) + 1)
    if not found:
        raise SystemExit("big font rect table not found in the pak (expected System.wad0 paks/global.pak)")
    return found


def read_big_rects(data):
    """The big font's rect records: header {count, 0, 0, 0} with records up to the D-pad sprites."""
    return _big_table(data)[0]


def texpack_key(code, w, h, level0):
    """The runtime's texture-pack key (runtime/src/texpack.c texpack_hash): FNV-1a 64 of the
    header {code, w, h, 0} and the level-0 bytes, as 16 hex digits."""
    hsh = 0xCBF29CE484222325
    for byte in struct.pack("<4I", code, w, h, 0) + bytes(level0):
        hsh = ((hsh ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return "%016x" % hsh


def decode_dxt5(level0, w, h):
    """RGBA image of DXT5 (BC3) blocks, decoded by Pillow's DDS reader."""
    import io
    hdr = struct.pack("<4sIIIIIII44sII4sIIIIIIIIII", b"DDS ", 124, 0x81007, h, w, len(level0), 0, 1, bytes(44),
                      32, 4, b"DXT5", 0, 0, 0, 0, 0, 0x1000, 0, 0, 0, 0)
    return Image.open(io.BytesIO(hdr + bytes(level0))).convert("RGBA")


def big_texture(data, key=BIG_ATLAS_KEY):
    """The big font atlas decoded from global.pak bytes: the texture chunk {0x2a, size, 0, id} with
    the font chunk's id holds level 0 at +0x90; the size is the one whose texture-pack key matches."""
    _, rec_off = _big_table(data)
    font = rec_off - 0x24
    ctype, _, zero, tid = struct.unpack_from("<IIII", data, font) if font >= 0 else (0, 0, 1, 0)
    if ctype != 0x4C or zero:
        raise SystemExit("big font chunk not found before its rect table (use --big-base)")
    needle = struct.pack("<I", 0x2A)
    pos = data.find(needle)
    while pos >= 0:
        _, size, z, i = struct.unpack_from("<IIII", data, pos) if pos + 16 <= len(data) else (0, 0, 1, 0)
        if pos % 4 == 0 and not z and i == tid:
            for w, h in ((1024, 512), (512, 1024), (2048, 1024), (1024, 1024), (512, 256), (256, 512), (128, 64), (64, 32)):
                start = pos + 0x90
                if start + w * h <= len(data) and w * h <= size and texpack_key(DXT5, w, h, data[start:start + w * h]) == key:
                    return decode_dxt5(data[start:start + w * h], w, h)
        pos = data.find(needle, pos + 1)
    raise SystemExit(f"big font atlas {key} not found in the pak (use --big-base)")


def upscale(img, scale):
    """img scale x larger: colours bicubic (premultiplied); the alpha of near-white letter ink gets a
    steep ramp at half coverage, so letter edges stay crisp (a light version of --sharp-fonts),
    everything else keeps the smooth alpha."""
    if scale == 1:
        return img.copy()
    size = (img.size[0] * scale, img.size[1] * scale)
    up = img.convert("RGBa").resize(size, Image.BICUBIC).convert("RGBA")
    r, g, b, a = up.split()
    ink = Image.merge("RGB", (r, g, b)).convert("L").point(lambda v: 255 if v > 200 else 0)
    sharp = a.point(lambda v: max(0, min(255, (v - 128) * 3 + 128)))
    return Image.merge("RGBA", (r, g, b, Image.composite(sharp, a, ink)))


def cell_box(rec, w, h):
    """Pixel box (x0, y0, x1, y1) covering a normalized rect in a w x h atlas."""
    import math
    u0, v0, u1, v1 = rec
    return (math.floor(u0 * w + 1e-6), math.floor(v0 * h + 1e-6), math.ceil(u1 * w - 1e-6), math.ceil(v1 * h - 1e-6))


def targets(recs, w, h):
    """[(box, art, kind)] for every cell to repaint; kind is 'glyph' or 'sprite'."""
    out = []
    for code, art in list(CODES.items()) + [(c, "dpad:" + d) for c, d in DPAD_CODES.items()]:
        if code + TABLE_BASE < len(recs):
            out.append((cell_box(recs[code + TABLE_BASE], w, h), art, "glyph"))
    for idx, art in SPRITES.items():
        if idx < len(recs):
            out.append((cell_box(recs[idx], w, h), art, "sprite"))
    return [t for t in out if t[0][2] > t[0][0] and t[0][3] > t[0][1]]


def opaque_rows(base, box):
    """(top, bottom) fractions of the cell height covered by base texels with alpha > 128."""
    bb = base.crop(box).getchannel("A").point(lambda a: 255 if a > 128 else 0).getbbox()
    hgt = box[3] - box[1]
    return (bb[1] / hgt, bb[3] / hgt) if bb else None


def glyph_band(base, recs):
    """Vertical band (fractions of the cell height) the face-button glyphs occupy in the base, so
    the new glyphs sit where the old ones did relative to the text baseline."""
    w, h = base.size
    rows = [opaque_rows(base, cell_box(recs[c + TABLE_BASE], w, h)) for c in FACE_CODES if c + TABLE_BASE < len(recs)]
    rows = [r for r in rows if r]
    if not rows:
        return (0.0, 1.0)
    return (min(r[0] for r in rows), max(r[1] for r in rows))


def _fit_font(font_path, text, max_w, max_h):
    size = max(4, int(max_h))
    while size > 4:
        font = ImageFont.truetype(font_path, size) if font_path else ImageFont.load_default(size)
        l, t, r, b = font.getbbox(text)
        if r - l <= max_w and b - t <= max_h:
            return font
        size -= 1
    return ImageFont.truetype(font_path, 4) if font_path else ImageFont.load_default(4)


def _text(d, cx, cy, text, font, color):
    l, t, r, b = font.getbbox(text)
    d.text((cx - (l + r) / 2, cy - (t + b) / 2), text, font=font, fill=color)


DIRECTIONS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0)}


def _dpad(d, cx, cy, dia, lit, theme):
    """A PlayStation D-pad: four arrow-shaped keys pointing at a gap in the middle, each with an
    engraved triangle pointing out; the lit key is solid ink with a fill-coloured triangle."""
    fill = THEMES[theme]["fill"] + (255,)
    ink = THEMES[theme]["ink"] + (255,)
    end, w, gap = 0.47 * dia, 0.36 * dia, 0.07 * dia
    rim = 0.06 * dia if THEMES[theme].get("rim") else 0.0
    for name, (dx, dy) in DIRECTIONS.items():
        def at(u, v):   # u outward along the arm, v across it
            return (cx + u * dx - v * dy, cy + u * dy + v * dx)
        hw, tip = w / 2 - rim, gap + rim * 1.4142
        on = name == lit
        d.polygon([at(gap, 0), at(gap + w / 2, -w / 2), at(end, -w / 2), at(end, w / 2), at(gap + w / 2, w / 2)], fill=ink)
        d.polygon([at(tip, 0), at(tip + hw, -hw), at(end - rim, -hw), at(end - rim, hw), at(tip + hw, hw)], fill=ink if on else fill)
        m, t = (tip + hw + end - rim) / 2, 0.09 * dia
        d.polygon([at(m + 0.6 * t, 0), at(m - 0.4 * t, -0.6 * t), at(m - 0.4 * t, 0.6 * t)], fill=fill if on else ink)


def draw_cell(art, cw, ch, band, theme, font_path):
    """RGBA image cw x ch with the art, supersampled and LANCZOS-downsampled (premultiplied)."""
    s = SUPERSAMPLE
    W, H = cw * s, ch * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fill = THEMES[theme]["fill"] + (255,)
    ink = THEMES[theme]["ink"] + (255,)
    margin = max(1.0, 0.04 * cw) * s
    top, bottom = band[0] * H, band[1] * H
    cx, cy = W / 2, (top + bottom) / 2
    avail_w, avail_h = W - 2 * margin, bottom - top - 2 * margin * 0.5
    if art.startswith("dpad:"):   # "dpad:" (neutral) or "dpad:<direction>" (that key lit)
        _dpad(d, cx, cy, min(avail_w, avail_h), art[5:] or None, theme)
    elif art in ("cross", "circle", "square", "triangle", "L3", "R3"):
        dia = min(avail_w, avail_h)
        r = dia / 2
        pad = 0.075 * dia   # label inset, kept with or without a rim
        rim = pad if THEMES[theme].get("rim") else 0.0
        face = art in ("cross", "circle", "square", "triangle")
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ink if THEMES[theme].get("face_rim", True) or not face else fill)
        d.ellipse((cx - r + rim, cy - r + rim, cx + r - rim, cy + r - rim), fill=fill)
        st = 0.10 * dia
        ink = THEMES[theme].get("symbols", {}).get(art, ink[:3]) + (255,)   # symbol colour, rim already drawn
        if art == "cross":
            a, t = 0.20 * dia, st / 2
            for sx in (1, -1):   # bar from (-a, -a*sx) to (a, a*sx), thickness st
                nx, ny = t * 0.7071, -t * 0.7071 * sx
                d.polygon([(cx - a + nx, cy - a * sx + ny), (cx + a + nx, cy + a * sx + ny),
                           (cx + a - nx, cy + a * sx - ny), (cx - a - nx, cy - a * sx - ny)], fill=ink)
        elif art == "circle":
            ro, ri = 0.24 * dia, 0.24 * dia - st
            d.ellipse((cx - ro, cy - ro, cx + ro, cy + ro), fill=ink)
            d.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=fill)
        elif art == "square":
            ho, hi = 0.20 * dia, 0.20 * dia - st
            d.rectangle((cx - ho, cy - ho, cx + ho, cy + ho), fill=ink)
            d.rectangle((cx - hi, cy - hi, cx + hi, cy + hi), fill=fill)
        elif art == "triangle":
            ro, tst = 0.30 * dia, 0.075 * dia
            ri = ro - 2 * tst   # an equilateral inset by tst has a circumradius 2 * tst smaller
            ty = cy + 0.03 * dia   # optical centring: the centroid sits a little low
            for rad, col in ((ro, ink), (ri, fill)):
                d.polygon([(cx, ty - rad), (cx + rad * 0.8660, ty + rad / 2), (cx - rad * 0.8660, ty + rad / 2)], fill=col)
        else:   # L3 / R3: label in a round badge
            font = _fit_font(font_path, art, 1.4 * (r - pad), 0.8 * (r - pad))
            _text(d, cx, cy, art, font, ink)
    else:
        if art in ("START", "SELECT"):
            bw, bh = avail_w, 0.48 * avail_h
        else:   # L1 L2 R1 R2: rounded-rect shoulder badge
            bw, bh = min(avail_w, 1.15 * avail_h), 0.72 * avail_h
        rad = 0.32 * bh
        pad = 0.075 * min(bw, avail_h)
        rim = pad if THEMES[theme].get("rim") else 0.0
        box = (cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
        d.rounded_rectangle(box, radius=rad, fill=ink if rim else fill)
        if rim:
            d.rounded_rectangle((box[0] + rim, box[1] + rim, box[2] - rim, box[3] - rim), radius=max(1, rad - rim), fill=fill)
        font = _fit_font(font_path, art, bw - 3.2 * pad, 0.55 * bh)
        _text(d, cx, cy, art, font, ink)
    # Premultiplied: a box reduction to 2x, then LANCZOS to 1x (less ringing than one LANCZOS step).
    pre = img.convert("RGBa").reduce(s // 2)
    return pre.resize((cw, ch), Image.LANCZOS).convert("RGBA")


def make_atlas(base, recs, theme, font_path):
    """Copy of base with every target cell cleared to transparent and repainted."""
    out = base.copy()
    w, h = out.size
    band = glyph_band(base, recs)
    for box, art, kind in targets(recs, w, h):
        cw, ch = box[2] - box[0], box[3] - box[1]
        if kind == "sprite":
            rows = opaque_rows(base, box) or (0.0, 1.0)
            cell_band = rows
        else:
            cell_band = band
        out.paste(Image.new("RGBA", (cw, ch), (0, 0, 0, 0)), box[:2])
        out.paste(draw_cell(art, cw, ch, cell_band, theme, font_path), box[:2])
    return out


def big_targets(recs, w, h):
    """[(box, art)] for the big font's D-pad sprites."""
    out = [(cell_box(recs[r], w, h), "dpad:" + (d or "")) for r, d in BIG_SPRITES.items() if r < len(recs)]
    return [t for t in out if t[0][2] > t[0][0] and t[0][3] > t[0][1]]


def make_big_atlas(base, recs, theme):
    """Copy of the big font atlas with the D-pad sprites cleared and repainted in their own rows."""
    out = base.copy()
    w, h = out.size
    for box, art in big_targets(recs, w, h):
        cw, ch = box[2] - box[0], box[3] - box[1]
        out.paste(Image.new("RGBA", (cw, ch), (0, 0, 0, 0)), box[:2])
        out.paste(draw_cell(art, cw, ch, opaque_rows(base, box) or (0.0, 1.0), theme, None), box[:2])
    return out


def contact_sheet(cells, path, height=96):
    """Every repainted cell [(atlas, box)] over dark, mid and light backgrounds (review only)."""
    tiles = []
    for atlas, box in cells:
        cell = atlas.crop(box)
        scale = max(1, height // max(1, cell.size[1]))
        tiles.append(cell.resize((cell.size[0] * scale, cell.size[1] * scale), Image.NEAREST))
    cw, chh, pad = max(t.size[0] for t in tiles), max(t.size[1] for t in tiles), 4
    sheet = Image.new("RGBA", (len(tiles) * (cw + pad) + pad, 3 * (chh + pad) + pad), (128, 0, 128, 255))
    for row, bg in enumerate(((16, 16, 16, 255), (110, 110, 110, 255), (225, 225, 225, 255))):
        for i, cell in enumerate(tiles):
            tile = Image.new("RGBA", cell.size, bg)
            tile.alpha_composite(cell)
            sheet.paste(tile, (pad + i * (cw + pad), pad + row * (chh + pad)))
    sheet.save(path)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True, help="base atlas PNG (1x dump or an upscaled pack's %s.png)" % ATLAS_KEY)
    ap.add_argument("--pak", required=True, help="your System.wad0 paks/global.pak (glyph rects)")
    ap.add_argument("--theme", choices=sorted(THEMES), default="black")
    ap.add_argument("--out", required=True, help="output pack directory (gets %s.png)" % ATLAS_KEY)
    ap.add_argument("--sheet", help="optional contact sheet PNG for review")
    ap.add_argument("--font", help="TrueType font for the labels (default: a system bold sans)")
    ap.add_argument("--big-base", help="big font atlas PNG (%s.png) to paint on instead of the one decoded from --pak" % BIG_ATLAS_KEY)
    ap.add_argument("--big-scale", type=int, default=BIG_SCALE, help="upscale of the big font atlas decoded from --pak (default 4)")
    ap.add_argument("--big-key", default=BIG_ATLAS_KEY, help=argparse.SUPPRESS)   # tests: a synthetic atlas's key
    a = ap.parse_args(argv)
    if Image is None:
        raise SystemExit("make_ps_glyphs.py needs Pillow (pip install Pillow)")
    with open(a.pak, "rb") as f:
        pak = f.read()
    recs = read_rects(pak)
    big_recs = read_big_rects(pak)
    if a.big_base:
        big_base = Image.open(a.big_base).convert("RGBA")
    else:
        big_base = upscale(big_texture(pak, a.big_key), max(1, a.big_scale))
    base = Image.open(a.base).convert("RGBA")
    font_path = find_font(a.font)
    if not font_path:
        print("warning: no system bold font found; using Pillow's default font (try --font)", file=sys.stderr)
    atlas = make_atlas(base, recs, a.theme, font_path)
    os.makedirs(a.out, exist_ok=True)
    out_path = os.path.join(a.out, ATLAS_KEY + ".png")
    atlas.save(out_path)
    n = len(targets(recs, *base.size))
    print(f"{out_path}: {base.size[0]}x{base.size[1]}, {n} cells repainted ({a.theme}), font {font_path or 'default'}")
    big = make_big_atlas(big_base, big_recs, a.theme)
    big_path = os.path.join(a.out, BIG_ATLAS_KEY + ".png")
    big.save(big_path)
    print(f"{big_path}: {big.size[0]}x{big.size[1]}, {len(big_targets(big_recs, *big.size))} D-pad sprites repainted"
          f" ({'from ' + a.big_base if a.big_base else 'decoded from the pak, %dx' % max(1, a.big_scale)})")
    if a.sheet:
        cells = [(atlas, box) for box, _, _ in targets(recs, *atlas.size)]
        cells += [(big, box) for box, _ in big_targets(big_recs, *big.size)]
        contact_sheet(cells, a.sheet)
        print(f"contact sheet: {a.sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
