#!/usr/bin/env python3
"""Adds height to the normal maps of a relief pack (the runtime's --parallax reads it from their alpha).

The relief pack's <hash>_n maps (tools/relief_from_albedo_ai.py, DeepBump's estimate) hold the slope of the relief
at every texel. This tool integrates those slopes into a height field (Frankot-Chellappa least squares, the same
integration the relief tool uses for its cavities), keeps its local relief (the bricks over the mortar, the cobbles
over the gaps, the planks over the seams; the broad swell of the whole texture is removed) and writes it into the
maps' alpha, which the game otherwise ignores, keeping R, G and B byte for byte:
  - alpha 255 = the surface, lower = deeper, 0 = the deepest point (--parallax-depth at run time). The relief is
    measured down from its local top (the bricks, cobbles or planks around a groove, within --detail of the
    texture's size) in units of --full-depth (0.012 of the texture's size reaches alpha 0), so mortar, gaps and
    seams read deep, weak relief (plaster, asphalt grain) stays shallow and the slow drift of the whole texture is
    ignored; a flat texture stays 255 everywhere and draws exactly as without parallax.

Only surfaces that read as masonry, paving or planks get a height: the pack classes environment_tiling and
environment_unique (selection.csv / manifest.csv, column class or category, as the relief tool reads them). With
--materials (the material_shine.csv that tools/material_shine.py writes) the class prop also qualifies when its
material is brick, concrete, asphalt, wood or dirt, and any texture whose material is something else (metal,
glass, car, chrome, fence, sign, paper, fabric, none) is left flat. Characters, UI, fx, signs and decals never get
one. Textures with see-through texels (more than --cutout of them, alpha below 128: fences, foliage, decals) are
skipped, and texels whose specular map (<hash>_s) carries metal or glass shine (B, from material_shine.py) stay at
the surface.

The maps are rewritten where they are (--relief) or into --out (a pack of its own, given to the game before the
relief pack). A map whose .dds sits next to its .png gets a new BC7 .dds too (bc7enc: --encoder or
$TEXPACK_BC_ENCODER), since the game reads the .dds first. Resumable: a map whose alpha already holds a height is
skipped (--force redoes it). No model and no GPU: about 0.1 s per texture plus its BC7 encode. PNG decoding uses
the pngraw helper (--pngraw). Nothing leaves the machine.
"""
import argparse
import concurrent.futures
import csv
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import normal_from_albedo as nfa  # noqa: E402  (blur, write_dds, read_hash_list)
import relief_from_albedo_ai as rai  # noqa: E402  (read_classes, MIN_Z)
import texture_compress as tc  # noqa: E402  (find_encoder, ENCODER_ENV)
import texture_upscale as tu  # noqa: E402  (read_png, write_png, fit, PNGRAW)

CLASSES = ("environment_tiling", "environment_unique")
MATERIAL_CLASSES = ("prop",)            # with --materials, when the material is one of MATERIALS
MATERIALS = ("brick", "concrete", "asphalt", "wood", "dirt")
FLAT = 250                              # alpha at or above this reads as the surface (runtime NORMAL_MAPS_HEIGHT_FLAT)
DEFAULTS = {"detail": 0.015625, "full": 0.012, "shine": 0.2, "cutout": 0.01}


def slopes_from_normals(nmap):
    """(sx, sy) of an RGBA OpenGL normal map: n ~ (sx, sy, 1), +y up the image."""
    n = nmap[..., :3].astype(np.float32) / 127.5 - 1.0
    z = np.maximum(n[..., 2], rai.MIN_Z)
    return n[..., 0] / z, n[..., 1] / z


def integrate(sx, sy):
    """Height in texels (wrapping, mean 0) whose normals are (sx, sy, 1): Frankot-Chellappa least squares in the
    Fourier domain, as relief_from_albedo_ai.height_from_slopes but not rescaled. dz/dcol = -sx, dz/drow = +sy."""
    h, w = sx.shape
    p, q = -sx.astype(np.float64), sy.astype(np.float64)
    wx = 2 * np.pi * np.fft.fftfreq(w)[None, :]
    wy = 2 * np.pi * np.fft.fftfreq(h)[:, None]
    den = wx ** 2 + wy ** 2
    den[0, 0] = 1.0
    zf = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / den
    zf[0, 0] = 0.0
    return np.real(np.fft.ifft2(zf)).astype(np.float32)


def envelope(z, r):
    """Upper envelope of z (wrapping): the maximum over a (2r + 1)^2 square, then blurred over r."""
    m = z.copy()
    for axis in (0, 1):
        src = m.copy()
        for k in range(1, r + 1):
            m = np.maximum(m, np.maximum(np.roll(src, k, axis), np.roll(src, -k, axis)))
    return nfa.blur(m, r)


def height_map(nmap, detail=DEFAULTS["detail"], full=DEFAULTS["full"]):
    """H x W float height (0 = deepest, 1 = surface) of a normal map: its slopes integrated (wrapping), measured
    down from their local top (the upper envelope over `detail` x the texture's size: the bricks, cobbles and
    planks around a groove are the surface, and the drift of the whole texture is ignored) in units of `full` x
    the texture's size (0 that deep and deeper). Weak relief (plaster, asphalt grain) so stays shallow; a flat map
    is 1 everywhere."""
    sx, sy = slopes_from_normals(nmap)
    if float(np.abs(sx).max()) < 1e-3 and float(np.abs(sy).max()) < 1e-3:
        return np.ones(sx.shape, dtype=np.float32)
    z = integrate(sx, sy)
    size = max(z.shape)
    depth = np.maximum(envelope(z, max(1, int(round(detail * size)))) - z, 0.0)
    return np.clip(1.0 - depth / (full * size), 0.0, 1.0).astype(np.float32)


def flatten_shiny(height, spec, threshold=DEFAULTS["shine"]):
    """Texels whose specular map's B (material shine) is metal or glass (from `threshold` up, fully at twice it)
    rise to the surface; spec is the RGBA specular map (any size: resized to the height's)."""
    if spec is None or spec.shape[2] < 3 or not spec[..., 2].any():
        return height
    if spec.shape[:2] != height.shape:
        ys = (np.arange(height.shape[0]) * spec.shape[0] // height.shape[0]).astype(int)
        xs = (np.arange(height.shape[1]) * spec.shape[1] // height.shape[1]).astype(int)
        spec = spec[ys][:, xs]
    b = spec[..., 2].astype(np.float32) / 255.0
    t = np.clip((b - threshold) / max(threshold, 1e-6), 0.0, 1.0)
    keep = 1.0 - t * t * (3.0 - 2.0 * t)
    return (1.0 - (1.0 - height) * keep).astype(np.float32)


def with_height(nmap, height, colour_alpha=None):
    """The normal map with alpha = height (255 = surface); RGB untouched. Transparent colour texels stay at 255."""
    out = nmap.copy()
    a = np.clip(np.round(height * 255.0), 0, 255).astype(np.uint8)
    if colour_alpha is not None:
        if colour_alpha.shape != a.shape:
            ys = (np.arange(a.shape[0]) * colour_alpha.shape[0] // a.shape[0]).astype(int)
            xs = (np.arange(a.shape[1]) * colour_alpha.shape[1] // a.shape[1]).astype(int)
            colour_alpha = colour_alpha[ys][:, xs]
        a[colour_alpha < 128] = 255
    out[..., 3] = a
    return out


def has_height(nmap):
    return nmap.shape[2] == 4 and int(nmap[..., 3].min()) < FLAT


def wanted(cls, material):
    """None when a texture gets a height, else the reason it does not ("class" or "material")."""
    if material is not None:
        if material not in MATERIALS:
            return "material"
        return None if cls in CLASSES + MATERIAL_CLASSES else "class"
    return None if cls in CLASSES else "class"


def read_materials(path):
    """hash -> material from a material_shine.csv (columns hash, material)."""
    out = {}
    if path:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                h = (r.get("hash") or "").strip().lower()
                if h:
                    out[h] = (r.get("material") or "").strip().lower()
    return out


def process(h, opts):
    """One texture: "made", "exists", "class", "material", "cutout", "flat", "no map" or "error: ..." (never raises)."""
    try:
        src = os.path.join(opts["relief"], h + "_n.png")
        if not os.path.isfile(src):
            return "no map"
        why = wanted(opts["classes"].get(h), opts["materials"].get(h) if opts["materials"] else None)
        if why:
            return why
        out_png = os.path.join(opts["out"], h + "_n.png")
        if not opts["force"] and os.path.isfile(out_png) and has_height(tu.read_png(out_png)):
            return "exists"
        colour_alpha = None
        colour = opts["colours"].get(h)
        if colour:
            rgba = tu.read_png(colour)
            if rgba.shape[2] == 4:
                clear = rgba[..., 3] < 128
                if clear.mean() > opts["cutout"]:
                    return "cutout"
                colour_alpha = rgba[..., 3] if clear.any() else None
        nmap = tu.read_png(src)
        if nmap.shape[2] == 3:
            nmap = np.concatenate([nmap, np.full(nmap.shape[:2] + (1,), 255, np.uint8)], axis=-1)
        height = height_map(nmap, opts["detail"], opts["full"])
        spec_path = os.path.join(opts["relief"], h + "_s.png")
        if os.path.isfile(spec_path):
            height = flatten_shiny(height, tu.read_png(spec_path), opts["shine"])
        img = with_height(nmap, height, colour_alpha)
        if not has_height(img):
            return "flat"
        part = out_png + ".part"
        tu.write_png(part, img)
        os.replace(part, out_png)
        if opts["encoder"] and os.path.isfile(os.path.join(opts["relief"], h + "_n.dds")):
            nfa.write_dds(os.path.join(opts["out"], h + "_n.dds"), img, opts["encoder"])
        return "made"
    except Exception as e:   # reported per texture; the run goes on
        return "error: %s: %s" % (type(e).__name__, e)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--relief", required=True, help="relief pack with <hash>_n.png (and _s.png) maps")
    ap.add_argument("--pack", help="the PNG pack the maps were made from (classes, see-through texels); default: classes from --relief only")
    ap.add_argument("--out", help="write the maps here (a pack of its own) instead of over --relief")
    ap.add_argument("--materials", help="material_shine.csv (tools/material_shine.py): props of masonry or wood qualify, metal and glass do not")
    ap.add_argument("--manifest", help="extra CSV with hash,class (or category) columns")
    ap.add_argument("--hash", action="append", default=[], help="only this texture hash (repeatable)")
    ap.add_argument("--hashes", help="file with the texture hashes to process (one per line, # comments)")
    ap.add_argument("--detail", type=float, default=DEFAULTS["detail"],
                    help="reach of the local top grooves are measured from (fraction of the texture size; default %(default)s)")
    ap.add_argument("--full-depth", dest="full", type=float, default=DEFAULTS["full"],
                    help="relief this deep (fraction of the texture size) becomes the deepest point, alpha 0; shallower "
                         "relief stays shallower (default %(default)s)")
    ap.add_argument("--cutout", type=float, default=DEFAULTS["cutout"],
                    help="skip textures with more than this share of see-through texels (default %(default)s)")
    ap.add_argument("--shine", type=float, default=DEFAULTS["shine"],
                    help="specular B (material shine) from which texels stay at the surface (default %(default)s)")
    ap.add_argument("--force", action="store_true", help="redo maps that already hold a height")
    ap.add_argument("--encoder", help="bc7enc executable for the .dds maps (default: $%s, then bc7enc on PATH)" % tc.ENCODER_ENV)
    ap.add_argument("--no-dds", action="store_true", help="write no .dds (a .dds next to a rewritten .png then holds no height)")
    ap.add_argument("--jobs", type=int, default=4, help="textures processed at once (default %(default)s)")
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    if not os.path.isdir(args.relief):
        sys.exit("cannot read the relief pack %s" % args.relief)
    if args.pack and not os.path.isdir(args.pack):
        sys.exit("cannot read the pack %s" % args.pack)
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    if not (0 < args.detail < 1 and 0 < args.full <= 1 and 0 <= args.cutout <= 1 and 0 < args.shine <= 1 and args.jobs >= 1):
        sys.exit("out of range: 0 < --detail < 1, 0 < --full-depth <= 1, 0 <= --cutout <= 1, 0 < --shine <= 1, --jobs >= 1")
    out = args.out or args.relief
    if args.pack and os.path.realpath(out) == os.path.realpath(args.pack):
        sys.exit("--out must differ from --pack")
    encoder = None
    if not args.no_dds:
        encoder = tc.find_encoder(args.encoder)
        names = os.listdir(args.relief)
        if not encoder and any(n.endswith("_n.dds") for n in names):
            sys.exit("the relief pack has .dds maps: pass --encoder PATH (bc7enc), set %s, or --no-dds" % tc.ENCODER_ENV)
    tu.PNGRAW = args.pngraw
    os.makedirs(out, exist_ok=True)
    hashes = sorted({n[:16].lower() for n in os.listdir(args.relief) if n.lower().endswith("_n.png") and len(n) == 22})
    pick = [h.lower() for h in args.hash] + (nfa.read_hash_list(args.hashes) if args.hashes else [])
    if pick:
        hashes = [h for h in dict.fromkeys(pick) if h in set(hashes)]
    classes = rai.read_classes(args.pack, args.manifest) if args.pack else rai.read_classes(args.relief, args.manifest)
    colours = {}
    if args.pack:
        for n in os.listdir(args.pack):
            if n.lower().endswith(".png") and len(n) == 20:
                colours[n[:16].lower()] = os.path.join(args.pack, n)
    opts = dict(relief=args.relief, out=out, encoder=encoder, classes=classes, colours=colours,
                materials=read_materials(args.materials), force=args.force, detail=args.detail, full=args.full,
                shine=args.shine, cutout=args.cutout)
    print("parallax height: %d normal maps from %s into %s" % (len(hashes), args.relief, out), flush=True)
    t0 = time.time()
    counts = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(process, h, opts): h for h in hashes}
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            r = fut.result()
            key = "failed" if r.startswith("error") else r
            counts[key] = counts.get(key, 0) + 1
            if key == "failed":
                print("  %s: %s" % (futures[fut], r), file=sys.stderr, flush=True)
            if i % 100 == 0 or i == len(hashes):
                print("  %d/%d made %d (%.0f s)" % (i, len(hashes), counts.get("made", 0), time.time() - t0), flush=True)
    print("done in %.1f s: %d with height, %d already there, %d skipped by class, %d by material, %d see-through, "
          "%d flat, %d failed" % (time.time() - t0, counts.get("made", 0), counts.get("exists", 0), counts.get("class", 0),
                                  counts.get("material", 0), counts.get("cutout", 0), counts.get("flat", 0),
                                  counts.get("failed", 0)))
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
