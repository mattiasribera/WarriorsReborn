#!/usr/bin/env python3
"""Turns the solid dark panel of a pack texture transparent: alpha is recovered from a texture
painted on one background colour (glowing sign letters on a black or navy panel), so the panel
disappears in the game while the letters and their glow stay.

Some of the title's signs are opaque textures whose letters glow on a dark panel, drawn alpha
blended with the texture's own alpha (opaque everywhere). On the console the panel is lost in the
night; with a raised Brightness it shows as a dark rectangle hiding what is behind the sign (the
WONDER WHEEL sign covers the wheel's hub and spokes). This tool rewrites such textures of a PNG
pack with an alpha channel, for a small overlay pack or to replace the pack's own files.

Per texel, with the colour c and the background b (0..1): each channel's rise above b relative to
the most it could rise (1 - b); alpha is the largest of the three, and the colour is
b + (c - b) / alpha, so the texture composited over b is the original again. The content is light
on a dark panel: texels darker than b (the panel's own unevenness) count as panel. Alpha under
--floor (compression noise in the panel) becomes 0 and the rest is stretched back to 0..1. An
alpha the texture already has is multiplied in. Transparent texels get their visible neighbours'
colour, so filtering and mips do not pull the panel's colour into the glow's edges.

  texture_unblend.py --pack PACK --out OUT HASH [HASH ...] [--background auto|RRGGBB] [--floor F]

--pack: the folder of <hash>.png (e.g. texpack-v4, or a --dump-textures folder). --out: written
<hash>.png files (made if missing; the pack is not changed). --background auto takes the median of
the texture's outermost texels. Then run tools/texture_compress.py --pack OUT --out OUT-bc for a BC
pack (textures with alpha become BC7). Exit 1 when a hash has no PNG in --pack.

PNG decoding uses the pngraw helper of tools/texture_upscale.py (pass --pngraw PATH when it is not
found); numpy is required.
"""
import argparse
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texture_upscale as tu  # noqa: E402  (read_png, write_png, bleed)

DEFAULT_FLOOR = 0.04


def border_color(img):
    """Median RGB of the outermost texels, as a tuple of ints."""
    rgb = img[..., :3]
    edge = np.concatenate([rgb[0], rgb[-1], rgb[1:-1, 0], rgb[1:-1, -1]])
    return tuple(int(v) for v in np.median(edge, axis=0))


def unblend(img, background, floor=DEFAULT_FLOOR):
    """img: H x W x 4 uint8 RGBA; background: (r, g, b) 0..255. Returns the RGBA with the
    background turned transparent (see the module doc)."""
    c = img[..., :3].astype(np.float32) / 255.0
    b = np.array(background, np.float32) / 255.0
    d = c - b
    a = np.clip((d / np.maximum(1.0 - b, 1e-6)).max(axis=-1), 0.0, 1.0)
    col = np.where(a[..., None] > 0, b + d / np.maximum(a, 1e-6)[..., None], c)
    a_out = np.clip((a - floor) / (1.0 - floor), 0.0, 1.0) if floor > 0 else a
    out = np.empty_like(img)
    out[..., :3] = np.clip(col * 255.0 + 0.5, 0, 255).astype(np.uint8)
    out[..., 3] = (a_out * img[..., 3].astype(np.float32) + 0.5).astype(np.uint8)
    return tu.bleed(out, alpha_min=1)


def parse_color(s):
    if s == "auto":
        return None
    if not re.fullmatch(r"[0-9a-fA-F]{6}", s):
        raise argparse.ArgumentTypeError("expected auto or RRGGBB, got %r" % s)
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def parse_hash(s):
    if not re.fullmatch(r"[0-9a-fA-F]{16}", s):
        raise argparse.ArgumentTypeError("expected a 16-digit hex texture hash, got %r" % s)
    return s.lower()


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hashes", nargs="+", type=parse_hash, metavar="HASH", help="texture hash (16 hex digits)")
    ap.add_argument("--pack", required=True, help="folder of <hash>.png to read")
    ap.add_argument("--out", required=True, help="folder for the written <hash>.png")
    ap.add_argument("--background", type=parse_color, default=None, metavar="auto|RRGGBB",
                    help="the panel colour (default auto: median of the texture's border)")
    ap.add_argument("--floor", type=float, default=DEFAULT_FLOOR,
                    help="alpha under this (0..1) becomes 0: compression noise in the panel (default %(default)s)")
    ap.add_argument("--pngraw", help="path of the pngraw PNG decoder")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.pngraw:
        tu.PNGRAW = args.pngraw
    if not os.path.isfile(tu.PNGRAW):
        print(tu.missing_pngraw(tu.PNGRAW), file=sys.stderr)
        return 1
    missing = [h for h in args.hashes if not os.path.isfile(os.path.join(args.pack, h + ".png"))]
    if missing:
        print("no PNG in %s for: %s" % (args.pack, " ".join(missing)), file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    for h in args.hashes:
        img = tu.read_png(os.path.join(args.pack, h + ".png"))
        bg = args.background or border_color(img)
        out = unblend(img, bg, args.floor)
        tu.write_png(os.path.join(args.out, h + ".png"), out)
        print("%s: background %02x%02x%02x, %.1f%% of the texels transparent"
              % (h, bg[0], bg[1], bg[2], 100.0 * float((out[..., 3] == 0).mean())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
