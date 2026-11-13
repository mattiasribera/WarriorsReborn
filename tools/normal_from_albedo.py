#!/usr/bin/env python3
"""Generates stopgap relief and specular maps (<hash>_n.png, <hash>_s.png) for a PNG texture pack.

STOPGAP: these maps are guessed from the colour texture alone (bright = raised), which is only a
rough stand-in for real relief. Replace them with maps baked from sculpted geometry (Blender,
Substance, ...) as those become available: the runtime reads any <hash>_n / <hash>_s file the same
way, whatever made it.

For every <hash>.png in --pack (and its selection.csv / manifest.csv categories, see
tools/texture_upscale.py) it writes into --out (never into the pack itself):
  - <hash>_n.png: a tangent-space normal map in the OpenGL convention the runtime expects (+Y =
    green points up the image; runtime/src/normal_maps.h). Height is the texel luminance, high-passed
    (minus a wide blur, --flatten-radius, scaled to the texture size) so large areas of one colour
    stay flat and only local detail (mortar, cracks, seams, folds) gets relief; Sobel gradients of
    that height, scaled by --strength and by the texture's size relative to 256 px (an upscaled
    texture gets the same tilt as the original), make the normal. Transparent texels (alpha below
    128) are flat. Alpha is 255. At most --max-size (default 512) on its longer side: the maps are
    uncompressed RGBA in the renderer's texture cache next to the pack (a full 4x pack of 1024-2048
    maps overflowed the default 1 GiB cache and made it drop and reload textures); --dds is 4x smaller.
  - <hash>_s.png (unless --no-specular): a conservative specular map: R = intensity, at most
    --spec-max (default 0.2), higher on bright, unsaturated texels; G = gloss, --gloss (default
    0.3: a broad, dull sheen). B = 0, alpha 255. Half the normal map's size (it is smooth).
UI and font textures (category ui or font, the ui-like heuristic, or a --skip-list hash) are skipped,
as tools/texture_compress.py keeps them as PNG; so are textures smaller than 8 x 8.

Select a subset with --hash H (repeatable) or --hashes FILE (one hash per line, # comments). With
--dds the maps are also written as BC7 <hash>_n.dds / <hash>_s.dds through tools/texture_compress.py
(bc7enc required: --encoder or $TEXPACK_BC_ENCODER); the runtime prefers a .dds when the device
samples BC blocks. Pass the output directory to the game as one more --texture-pack together with
--normal-maps (and --char-lighting / --world-lighting, whose lights shade the relief).

PNG decoding uses the pngraw helper of tools/texture_upscale.py (or --pngraw PATH); numpy only.
"""
import argparse
import os
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texture_compress as tc  # noqa: E402  (pack_hashes, read_categories, classify, mip_chain, encode, dds_header)
import texture_upscale as tu  # noqa: E402  (read_png, write_png, ui_like, PNGRAW)

MIN_SIZE = 8
REF_SIZE = 256.0   # --strength is calibrated for a texture of this size


def luminance(rgba):
    """H x W float32 in [0, 1] (Rec. 601 weights on the stored values)."""
    c = rgba[..., :3].astype(np.float32) / 255.0
    return c[..., 0] * 0.299 + c[..., 1] * 0.587 + c[..., 2] * 0.114


def box_blur(a, r, axis):
    """Box filter of radius r (width 2r + 1) along axis, wrapping (textures tile)."""
    if r < 1:
        return a
    n = a.shape[axis]
    idx = np.arange(-r, n + r) % n
    p = np.take(a, idx, axis=axis)
    c = np.cumsum(p, axis=axis, dtype=np.float64)
    zero = np.zeros_like(np.take(c, [0], axis=axis))
    c = np.concatenate([zero, c], axis=axis)
    hi = np.take(c, np.arange(2 * r + 1, n + 2 * r + 1), axis=axis)
    lo = np.take(c, np.arange(0, n), axis=axis)
    return ((hi - lo) / (2 * r + 1)).astype(np.float32)


def blur(a, r):
    """Three box passes per axis: close to a gaussian of sigma ~ r."""
    for axis in (0, 1):
        for _ in range(3):
            a = box_blur(a, r, axis)
    return a


def high_pass(h, radius):
    """h minus its wide blur: flat areas -> 0, local detail kept."""
    return h - blur(h, max(1, int(round(radius))))


def sobel(h):
    """d/dx (columns, right) and d/drow (rows, down) of h, per pixel, wrapping."""
    def sh(dy, dx):
        return np.roll(np.roll(h, -dy, axis=0), -dx, axis=1)
    gx = (sh(-1, 1) + 2 * sh(0, 1) + sh(1, 1) - sh(-1, -1) - 2 * sh(0, -1) - sh(1, -1)) / 8.0
    gy = (sh(1, -1) + 2 * sh(1, 0) + sh(1, 1) - sh(-1, -1) - 2 * sh(-1, 0) - sh(-1, 1)) / 8.0
    return gx, gy


def normal_map(rgba, strength=2.0, flatten_radius=1.0 / 32):
    """H x W x 4 uint8 tangent-space normal map (OpenGL: +Y up the image) of an RGBA texture."""
    h, w = rgba.shape[:2]
    lum = luminance(rgba)
    hp = high_pass(lum, flatten_radius * max(w, h))
    gx, gy = sobel(hp)
    k = strength * max(w, h) / REF_SIZE
    # The surface z = height: n ~ (-dz/du, -dz/dv_up, 1) with v_up = -row, so +row gradients tilt up.
    n = np.stack([-gx * k, gy * k, np.ones_like(gx)], axis=-1)
    if rgba.shape[2] == 4:
        n[rgba[..., 3] < 128] = (0.0, 0.0, 1.0)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    out = np.empty((h, w, 4), dtype=np.uint8)
    out[..., :3] = np.clip(np.round(n * 127.5 + 127.5), 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def specular_map(rgba, spec_max=0.2, gloss=0.3):
    """H x W x 4 uint8: R intensity (bright, unsaturated texels up to spec_max), G gloss, B 0."""
    c = rgba[..., :3].astype(np.float32) / 255.0
    lum = luminance(rgba)
    sat = c.max(axis=-1) - c.min(axis=-1)
    inten = spec_max * (0.25 + 0.75 * lum) * (1.0 - sat)
    if rgba.shape[2] == 4:
        inten[rgba[..., 3] < 128] = 0.0
    out = np.zeros(rgba.shape[:2] + (4,), dtype=np.uint8)
    out[..., 0] = np.clip(np.round(inten * 255), 0, 255).astype(np.uint8)
    out[..., 1] = int(np.clip(gloss, 0, 1) * 255 + 0.5)
    out[..., 3] = 255
    return out


def skip_reason(rgba, category, skipped, ui_heuristic=True):
    """Why a texture gets no maps (None: it gets them)."""
    if min(rgba.shape[:2]) < MIN_SIZE:
        return "small"
    if tc.classify(rgba, category, tc.BC1, ui_heuristic, skipped) == "png":
        return "ui"
    return None


def write_dds(path, img, encoder, quality=6):
    levels = tc.mip_chain(img)
    with tempfile.TemporaryDirectory(prefix=".work-", dir=os.path.dirname(path) or ".") as work:
        payload = tc.encode(encoder, tc.BC7, levels, quality, work)
        tmp = os.path.join(work, "out.dds")
        with open(tmp, "wb") as f:
            f.write(tc.dds_header(tc.BC7, img.shape[1], img.shape[0], len(levels)) + payload)
        os.replace(tmp, path)


def process(h, src, opts):
    """One texture: "made", "ui", "small" or "error: ..." (never raises)."""
    try:
        rgba = tu.read_png(src)
        why = skip_reason(rgba, opts["categories"].get(h), h in opts["skip"], opts["ui_heuristic"])
        if why:
            return why
        if opts["max_size"]:
            rgba = tu.fit(rgba, opts["max_size"])   # power-of-two box downscale (the tilt is size-independent)
        maps = [("n", normal_map(rgba, opts["strength"], opts["flatten_radius"]))]
        if opts["specular"]:
            spec = tu.fit(rgba, max(MIN_SIZE, max(rgba.shape[:2]) // 2))   # smooth: half the normal map's size
            maps.append(("s", specular_map(spec, opts["spec_max"], opts["gloss"])))
        for suffix, img in maps:
            tu.write_png(os.path.join(opts["out"], "%s_%s.png" % (h, suffix)), img)
            if opts["encoder"]:
                write_dds(os.path.join(opts["out"], "%s_%s.dds" % (h, suffix)), img, opts["encoder"])
        return "made"
    except Exception as e:   # reported per texture; the run goes on
        return "error: %s: %s" % (type(e).__name__, e)


def read_hash_list(path):
    with open(path) as f:
        return [ln.split("#")[0].strip().lower() for ln in f if ln.split("#")[0].strip()]


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="input PNG pack directory (<hash>.png files)")
    ap.add_argument("--out", required=True, help="output directory for <hash>_n.png / <hash>_s.png (created; not the pack)")
    ap.add_argument("--hash", action="append", default=[], help="only this texture hash (repeatable)")
    ap.add_argument("--hashes", help="file with the texture hashes to process (one per line, # comments)")
    ap.add_argument("--strength", type=float, default=2.0, help="relief strength (default %(default)s)")
    ap.add_argument("--flatten-radius", type=float, default=1.0 / 32,
                    help="high-pass blur radius as a fraction of the texture size: larger keeps broader shapes (default 1/32)")
    ap.add_argument("--max-size", type=int, default=512,
                    help="largest side of the normal maps, halved until it fits (0 = the texture's size; default %(default)s); "
                         "specular maps are half that. Uncompressed maps share the texture cache with the pack (--texpack-budget)")
    ap.add_argument("--no-specular", dest="specular", action="store_false", help="write no <hash>_s.png")
    ap.add_argument("--spec-max", type=float, default=0.2, help="highest specular intensity, 0-1 (default %(default)s)")
    ap.add_argument("--gloss", type=float, default=0.3, help="gloss of the specular maps, 0-1 (default %(default)s)")
    ap.add_argument("--manifest", help="extra CSV with hash,category columns (the pack's selection.csv / manifest.csv are read too)")
    ap.add_argument("--skip-list", help="file with hashes to skip (one per line, # comments)")
    ap.add_argument("--no-ui-heuristic", action="store_true", help="only categories and --skip-list mark UI textures")
    ap.add_argument("--dds", action="store_true", help="also write BC7 .dds maps (needs bc7enc)")
    ap.add_argument("--encoder", help="bc7enc executable for --dds (default: $%s, then bc7enc on PATH)" % tc.ENCODER_ENV)
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    if not os.path.isdir(args.pack):
        sys.exit("cannot read the pack %s" % args.pack)
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    if os.path.realpath(args.out) == os.path.realpath(args.pack):
        sys.exit("--out must differ from --pack (the maps go into a pack of their own)")
    if args.max_size < 0 or 0 < args.max_size < MIN_SIZE:
        sys.exit("--max-size: 0 or at least %d" % MIN_SIZE)
    if not (0 <= args.spec_max <= 1 and 0 <= args.gloss <= 1 and args.strength >= 0 and args.flatten_radius > 0):
        sys.exit("out of range: --strength >= 0, --flatten-radius > 0, --spec-max and --gloss 0-1")
    encoder = None
    if args.dds:
        encoder = tc.find_encoder(args.encoder)
        if not encoder:
            sys.exit("--dds: BC encoder not found (bc7enc; pass --encoder PATH or set %s)" % tc.ENCODER_ENV)
    tu.PNGRAW = args.pngraw
    os.makedirs(args.out, exist_ok=True)
    names = tc.pack_hashes(args.pack)
    wanted = [h.lower() for h in args.hash] + (read_hash_list(args.hashes) if args.hashes else [])
    if wanted:
        missing = [h for h in wanted if h not in names]
        for h in missing:
            print("  %s: not in the pack" % h, file=sys.stderr)
        names = {h: names[h] for h in dict.fromkeys(wanted) if h in names}
    opts = {"out": args.out, "strength": args.strength, "flatten_radius": args.flatten_radius, "specular": args.specular,
            "spec_max": args.spec_max, "gloss": args.gloss, "categories": tc.read_categories(args.pack, args.manifest),
            "skip": tc.load_skip_list(args.skip_list), "ui_heuristic": not args.no_ui_heuristic, "encoder": encoder,
            "max_size": args.max_size}
    print("normal maps (stopgap, from albedo): %d textures from %s into %s" % (len(names), args.pack, args.out))
    t0 = time.time()
    counts = {}
    for i, (h, src) in enumerate(names.items(), 1):
        r = process(h, src, opts)
        key = "failed" if r.startswith("error") else r
        counts[key] = counts.get(key, 0) + 1
        if key == "failed":
            print("  %s: %s" % (h, r), file=sys.stderr)
        if i % 100 == 0:
            print("  %d/%d (%.0f s)" % (i, len(names), time.time() - t0), flush=True)
    print("done in %.1f s: %d with maps, %d UI/font skipped, %d too small, %d failed" % (
        time.time() - t0, counts.get("made", 0), counts.get("ui", 0), counts.get("small", 0), counts.get("failed", 0)))
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
