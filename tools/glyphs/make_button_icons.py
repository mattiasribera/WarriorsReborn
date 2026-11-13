#!/usr/bin/env python3
"""Custom button icons for the game's font/icon atlas (texture-pack key 886a4c14094e9597).

The game draws its button prompts as characters of the main font atlas. This tool repaints those
cells of a base atlas PNG (from --dump-textures, or an upscaled pack's file) with neutral icons
drawn by this code, and writes <out>/886a4c14094e9597.png: a one-file texture pack to load
before the base pack (the launcher's button-icon pack folder, or by hand):

    xbrun ... --texture-pack <out> --texture-pack <base pack>

Face buttons: "position" (default) draws four dots in a diamond with the button's own dot filled
(bottom = A, right = B, left = X, top = Y on the game's layout), so the prompt reads the same on any
controller; "labels" draws a round badge with your own text (--face-labels) and optional colours
(--face-colors). Shoulders, triggers, stick clicks, Start and Back get badges with your own text
(--bumper-labels, --trigger-labels, --stick-labels, --start-label, --back-label).

The cell rectangles are read at run time from your own System.wad0 paks/global.pak (a table of
16-byte float records u0, v0, u1, v1; the rect of character code ch is record ch + base). No game
data is stored here: only the character codes and record indices of the button cells.

Themes: dark (dark badge, light ink) and light (light badge, dark ink). Labels use a system
TrueType font found at run time (--font overrides). Needs Python 3 + Pillow.
"""
import argparse
import math
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

# Character code -> the Xbox control its cell draws (the same table as runtime/src/kbm_glyphs.c).
# The alternates are the controller-config preset's localized letter badges of the trigger and
# stick-click cells: 0xa2 right trigger, 0xb2 right stick; 0xa3-0xa5 left trigger, 0xa6/0xa7/0xb3
# left stick.
GLYPH_CONTROLS = {
    0x9F: "X", 0x9E: "A", 0x9D: "B", 0x96: "Y",
    0x9C: "BLACK", 0x91: "WHITE", 0x97: "START", 0x93: "BACK",
    0x95: "LT", 0xA0: "RT", 0x94: "LTHUMB", 0x92: "RTHUMB",
    0xA2: "RT", 0xA3: "LT", 0xA4: "LT", 0xA5: "LT",
    0xA6: "LTHUMB", 0xA7: "LTHUMB", 0xB3: "LTHUMB", 0xB2: "RTHUMB",
}
# Large face-button sprites (record indices, not character codes): A, Y, X, B.
SPRITES = {79: "A", 73: "Y", 65: "X", 6: "B"}
FACE = ("A", "B", "X", "Y")
# Face button -> its place in the diamond (dx, dy): A bottom, B right, X left, Y top.
FACE_POS = {"A": (0, 1), "B": (1, 0), "X": (-1, 0), "Y": (0, -1)}
# How each control is drawn: the badge shape and which side (left/right) for the label lists.
SHAPE = {"WHITE": ("bumper", 0), "BLACK": ("bumper", 1), "LT": ("trigger", 0), "RT": ("trigger", 1),
         "LTHUMB": ("stick", 0), "RTHUMB": ("stick", 1), "START": ("wide", 0), "BACK": ("wide", 0)}

THEMES = {
    "dark": {"fill": (24, 24, 24), "ink": (240, 240, 240)},
    "light": {"fill": (235, 235, 235), "ink": (16, 16, 16)},
}

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


class Style:
    """What to draw: face mode, labels and colours, theme."""

    def __init__(self, theme="dark", face="position", face_labels=("A", "B", "X", "Y"), face_colors=None,
                 bumper_labels=("L", "R"), trigger_labels=("L", "R"), stick_labels=("L", "R"),
                 start_label="START", back_label="BACK"):
        if theme not in THEMES:
            raise ValueError(f"unknown theme {theme!r}")
        if face not in ("position", "labels"):
            raise ValueError(f"unknown face style {face!r}")
        for name, vals, n in (("face labels", face_labels, 4), ("bumper labels", bumper_labels, 2),
                              ("trigger labels", trigger_labels, 2), ("stick labels", stick_labels, 2)):
            if len(vals) != n or any(not v for v in vals):
                raise ValueError(f"{name}: need {n} non-empty values, got {list(vals)}")
        if face_colors is not None and len(face_colors) != 4:
            raise ValueError(f"face colors: need 4 values, got {list(face_colors)}")
        self.theme, self.face = theme, face
        self.face_labels = dict(zip(FACE, face_labels))
        self.face_colors = dict(zip(FACE, face_colors)) if face_colors else {}
        self.bumper, self.trigger, self.stick = tuple(bumper_labels), tuple(trigger_labels), tuple(stick_labels)
        self.start, self.back = start_label, back_label


def parse_list(text):
    return [v.strip() for v in text.split(",")]


def parse_color(text):
    """'#rrggbb' or 'rrggbb' -> (r, g, b)."""
    t = text.strip().lstrip("#")
    if len(t) != 6 or any(c not in "0123456789abcdefABCDEF" for c in t):
        raise ValueError(f"not a #rrggbb colour: {text!r}")
    return tuple(int(t[i:i + 2], 16) for i in (0, 2, 4))


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


def _table_at(data, rec_off):
    """Records of a table whose 16-byte header ends at rec_off, or None when it does not fit."""
    hdr = rec_off - 16
    if hdr < 0 or rec_off > len(data):
        return None
    count, base, z0, z1 = struct.unpack_from("<IIII", data, hdr)
    if base != TABLE_BASE or z0 or z1 or not 0x100 <= count <= 0x1000 or rec_off + count * 16 > len(data):
        return None
    recs = [struct.unpack_from("<4f", data, rec_off + 16 * i) for i in range(count)]
    for u0, v0, u1, v1 in recs:
        if not (0.0 <= u0 <= u1 <= 1.0 and 0.0 <= v0 <= v1 <= 1.0):
            return None
    return recs


def read_rects(data):
    """Glyph rect records (u0, v0, u1, v1) from global.pak bytes: the known offset first, else a
    scan for a header {count, base 0x5e, 0, 0} followed by count records in [0, 1]."""
    recs = _table_at(data, TABLE_OFFSET)
    if recs:
        return recs
    needle = struct.pack("<I", TABLE_BASE) + bytes(8)
    pos = data.find(needle)
    while pos >= 0:
        if pos % 4 == 0:
            recs = _table_at(data, pos + 12)
            if recs:
                return recs
        pos = data.find(needle, pos + 1)
    raise SystemExit("glyph rect table not found in the pak (expected System.wad0 paks/global.pak)")


def cell_box(rec, w, h):
    """Pixel box (x0, y0, x1, y1) covering a normalized rect in a w x h atlas."""
    u0, v0, u1, v1 = rec
    return (math.floor(u0 * w + 1e-6), math.floor(v0 * h + 1e-6), math.ceil(u1 * w - 1e-6), math.ceil(v1 * h - 1e-6))


def targets(recs, w, h):
    """[(box, control, kind)] for every cell to repaint; kind is 'glyph' or 'sprite'."""
    out = []
    for code, ctl in GLYPH_CONTROLS.items():
        if code + TABLE_BASE < len(recs):
            out.append((cell_box(recs[code + TABLE_BASE], w, h), ctl, "glyph"))
    for idx, ctl in SPRITES.items():
        if idx < len(recs):
            out.append((cell_box(recs[idx], w, h), ctl, "sprite"))
    return [t for t in out if t[0][2] > t[0][0] and t[0][3] > t[0][1]]


def opaque_rows(base, box):
    """(top, bottom) fractions of the cell height covered by base texels with alpha > 128."""
    bb = base.crop(box).getchannel("A").point(lambda a: 255 if a > 128 else 0).getbbox()
    hgt = box[3] - box[1]
    return (bb[1] / hgt, bb[3] / hgt) if bb else None


def glyph_band(base, recs):
    """Vertical band (fractions of the cell height) the face-button glyphs occupy in the base, so
    the new icons sit where the old ones did relative to the text baseline."""
    w, h = base.size
    codes = [c for c, ctl in GLYPH_CONTROLS.items() if ctl in FACE]
    rows = [opaque_rows(base, cell_box(recs[c + TABLE_BASE], w, h)) for c in codes if c + TABLE_BASE < len(recs)]
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


def _round_badge(d, cx, cy, r, rim, ink, fill):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=ink)
    d.ellipse((cx - r + rim, cy - r + rim, cx + r - rim, cy + r - rim), fill=fill)


def draw_cell(ctl, cw, ch, band, style, font_path):
    """RGBA image cw x ch with the icon, supersampled and downsampled (premultiplied)."""
    s = SUPERSAMPLE
    W, H = cw * s, ch * s
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fill = THEMES[style.theme]["fill"] + (255,)
    ink = THEMES[style.theme]["ink"] + (255,)
    margin = max(1.0, 0.04 * cw) * s
    top, bottom = band[0] * H, band[1] * H
    cx, cy = W / 2, (top + bottom) / 2
    avail_w, avail_h = W - 2 * margin, bottom - top - margin
    if ctl in FACE:
        dia = min(avail_w, avail_h)
        r, rim = dia / 2, 0.075 * dia
        accent = style.face_colors.get(ctl)
        _round_badge(d, cx, cy, r, rim, ink, fill)
        mark = accent + (255,) if accent else ink
        if style.face == "position":
            step, dot = 0.22 * dia, 0.085 * dia
            for btn, (dx, dy) in FACE_POS.items():
                px, py = cx + dx * step, cy + dy * step
                if btn == ctl:
                    d.ellipse((px - 1.45 * dot, py - 1.45 * dot, px + 1.45 * dot, py + 1.45 * dot), fill=mark)
                else:
                    d.ellipse((px - dot, py - dot, px + dot, py + dot), fill=ink)
                    d.ellipse((px - 0.45 * dot, py - 0.45 * dot, px + 0.45 * dot, py + 0.45 * dot), fill=fill)
        else:
            text = style.face_labels[ctl]
            font = _fit_font(font_path, text, 1.25 * (r - rim), 0.95 * (r - rim))
            _text(d, cx, cy, text, font, mark)
    else:
        shape, side = SHAPE[ctl]
        if shape == "stick":
            dia = min(avail_w, avail_h)
            r, rim = dia / 2, 0.075 * dia
            _round_badge(d, cx, cy, r, rim, ink, fill)
            ri = r - 2.6 * rim   # inner ring: a stick cap
            d.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), outline=ink, width=max(1, int(0.6 * rim)))
            text = style.stick[side]
            font = _fit_font(font_path, text, 1.2 * (ri - rim), 0.9 * (ri - rim))
            _text(d, cx, cy, text, font, ink)
        else:
            if shape == "wide":
                text = style.start if ctl == "START" else style.back
                bw, bh = avail_w, 0.48 * avail_h
            elif shape == "bumper":
                text = style.bumper[side]
                bw, bh = min(avail_w, 1.25 * avail_h), 0.56 * avail_h
            else:   # trigger: taller badge
                text = style.trigger[side]
                bw, bh = min(avail_w, 0.78 * avail_h), 0.92 * avail_h
            rad = 0.32 * min(bw, bh) if shape != "trigger" else 0.28 * bw
            rim = 0.075 * min(bw, avail_h)
            box = (cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2)
            d.rounded_rectangle(box, radius=rad, fill=ink)
            d.rounded_rectangle((box[0] + rim, box[1] + rim, box[2] - rim, box[3] - rim), radius=max(1, rad - rim), fill=fill)
            font = _fit_font(font_path, text, bw - 3.2 * rim, 0.55 * min(bh, bw))
            _text(d, cx, cy, text, font, ink)
    # Premultiplied: a box reduction to 2x, then LANCZOS to 1x (less ringing than one LANCZOS step).
    pre = img.convert("RGBa").reduce(s // 2)
    return pre.resize((cw, ch), Image.LANCZOS).convert("RGBA")


def make_atlas(base, recs, style, font_path):
    """Copy of base with every target cell cleared to transparent and repainted."""
    out = base.copy()
    w, h = out.size
    band = glyph_band(base, recs)
    for box, ctl, kind in targets(recs, w, h):
        cw, ch = box[2] - box[0], box[3] - box[1]
        cell_band = (opaque_rows(base, box) or (0.0, 1.0)) if kind == "sprite" else band
        out.paste(Image.new("RGBA", (cw, ch), (0, 0, 0, 0)), box[:2])
        out.paste(draw_cell(ctl, cw, ch, cell_band, style, font_path), box[:2])
    return out


def contact_sheet(atlas, recs, path, scale=None):
    """Every repainted cell over dark, mid and light backgrounds (review only)."""
    w, h = atlas.size
    cells = targets(recs, w, h)
    if scale is None:
        scale = max(1, 96 // max(b[3] - b[1] for b, _, _ in cells))
    cw = max(b[2] - b[0] for b, _, _ in cells) * scale
    chh = max(b[3] - b[1] for b, _, _ in cells) * scale
    pad = 4
    sheet = Image.new("RGBA", (len(cells) * (cw + pad) + pad, 3 * (chh + pad) + pad), (128, 0, 128, 255))
    for row, bg in enumerate(((16, 16, 16, 255), (110, 110, 110, 255), (225, 225, 225, 255))):
        for i, (box, _, _) in enumerate(cells):
            cell = atlas.crop(box)
            cell = cell.resize((cell.size[0] * scale, cell.size[1] * scale), Image.NEAREST)
            tile = Image.new("RGBA", cell.size, bg)
            tile.alpha_composite(cell)
            sheet.paste(tile, (pad + i * (cw + pad), pad + row * (chh + pad)))
    sheet.save(path)


def style_from_args(a):
    try:
        return Style(theme=a.theme, face=a.face, face_labels=parse_list(a.face_labels),
                     face_colors=[parse_color(c) for c in parse_list(a.face_colors)] if a.face_colors else None,
                     bumper_labels=parse_list(a.bumper_labels), trigger_labels=parse_list(a.trigger_labels),
                     stick_labels=parse_list(a.stick_labels), start_label=a.start_label, back_label=a.back_label)
    except ValueError as e:
        raise SystemExit(f"make_button_icons.py: {e}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--base", required=True, help="base atlas PNG (from --dump-textures, or an upscaled pack's %s.png)" % ATLAS_KEY)
    ap.add_argument("--pak", required=True, help="your System.wad0 paks/global.pak (glyph rects)")
    ap.add_argument("--out", required=True, help="output pack directory (gets %s.png)" % ATLAS_KEY)
    ap.add_argument("--theme", choices=sorted(THEMES), default="dark")
    ap.add_argument("--face", choices=("position", "labels"), default="position",
                    help="face buttons: position dots (default) or your own labels")
    ap.add_argument("--face-labels", default="A,B,X,Y", help="with --face labels: text for the bottom,right,left,top buttons")
    ap.add_argument("--face-colors", help="optional #rrggbb,#rrggbb,#rrggbb,#rrggbb for the bottom,right,left,top buttons")
    ap.add_argument("--bumper-labels", default="L,R", help="left,right shoulder buttons (default L,R)")
    ap.add_argument("--trigger-labels", default="L,R", help="left,right triggers (default L,R)")
    ap.add_argument("--stick-labels", default="L,R", help="left,right stick clicks (default L,R)")
    ap.add_argument("--start-label", default="START")
    ap.add_argument("--back-label", default="BACK")
    ap.add_argument("--sheet", help="optional contact sheet PNG for review")
    ap.add_argument("--font", help="TrueType font for the labels (default: a system bold sans)")
    a = ap.parse_args(argv)
    style = style_from_args(a)
    if Image is None:
        raise SystemExit("make_button_icons.py needs Pillow (pip install Pillow)")
    with open(a.pak, "rb") as f:
        recs = read_rects(f.read())
    base = Image.open(a.base).convert("RGBA")
    font_path = find_font(a.font)
    if not font_path:
        print("warning: no system bold font found; using Pillow's default font (try --font)", file=sys.stderr)
    atlas = make_atlas(base, recs, style, font_path)
    os.makedirs(a.out, exist_ok=True)
    out_path = os.path.join(a.out, ATLAS_KEY + ".png")
    atlas.save(out_path)
    n = len(targets(recs, *base.size))
    print(f"{out_path}: {base.size[0]}x{base.size[1]}, {n} cells repainted ({a.theme}, face {a.face}), font {font_path or 'default'}")
    if a.sheet:
        contact_sheet(atlas, recs, a.sheet)
        print(f"contact sheet: {a.sheet}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
