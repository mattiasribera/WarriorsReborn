#!/usr/bin/env python3
"""Converts a PNG texture pack into a BC-compressed pack of <hash>.dds files (BC1 / BC7).

Reads every <hash>.png in --pack (and, when present, the pack's selection.csv / manifest.csv, see
tools/texture_upscale.py) and writes into --out, per texture:
  - UI/font textures (category ui or font, the ui-like heuristic of texture_upscale.py, or a
    --skip-list hash): the PNG is copied as is, so --out is a complete pack;
  - any texel alpha below 255: <hash>.dds as BC7;
  - opaque: <hash>.dds as BC1 (--opaque bc7 forces BC7).
Each .dds has a DX10 header (DXGI BC1_UNORM / BC7_UNORM) and the complete mip chain down to 1x1,
the layout the runtime's texture-pack loader requires (runtime/src/texpack.h). The runtime keeps
using a pack's .png when the device has no BC support, so --with-png also copies every PNG.

Mips are made here, in display (gamma) space like the runtime's own box-filtered PNG mips, with
premultiplied alpha and a Lanczos-3 filter (--mip-filter box reproduces the runtime's 2x2 box
exactly). --alpha-coverage (default all: every BC7 texture) keeps the share of texels passing an
alpha test (--alpha-ref) equal to level 0 on every level, so cutouts (fences, foliage) do not thin
out in the distance; --alpha-coverage none turns it off.

The block encoder is bc7enc from bc7enc_rdo (https://github.com/richgel999/bc7enc_rdo, MIT or
public domain; its optional bc7e.ispc is Apache-2.0); it is not part of this repository. Pass
its path with --encoder or the TEXPACK_BC_ENCODER environment variable. All levels of a texture
are encoded in one call from a block-aligned atlas and cut back into levels (BC blocks are
independent of each other). Outputs newer than their input are skipped (--force redoes them).

PNG decoding uses the pngraw helper of tools/texture_upscale.py (pngraw.exe on Windows, next to the
tools or in runtime/build), or --pngraw PATH; numpy is required.
"""
import argparse
import csv
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ProcessPoolExecutor

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import texture_upscale as tu  # noqa: E402  (read_png, write_png, ui_like)

BC1, BC7 = "bc1", "bc7"
BLOCK_BYTES = {BC1: 8, BC7: 16}
DXGI = {BC1: 71, BC7: 98}   # DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC7_UNORM
UI_CATEGORIES = ("ui", "font")
ENCODER_ENV = "TEXPACK_BC_ENCODER"


# ---- mip and block math ----

def level_dims(w, h):
    """Every level of the full chain: [(w, h), (w >> 1, h >> 1) ..., (1, 1)], sides at least 1."""
    dims = [(w, h)]
    while w > 1 or h > 1:
        w, h = max(1, w >> 1), max(1, h >> 1)
        dims.append((w, h))
    return dims


def level_bytes(fmt, w, h):
    """BC blocks of one level: a level smaller than 4x4 still takes a whole block."""
    return ((w + 3) // 4) * ((h + 3) // 4) * BLOCK_BYTES[fmt]


def align4(n):
    return (n + 3) & ~3


def atlas_layout(dims):
    """Block-aligned places of the levels in one image: level 0 on top, the smaller levels side by
    side below it. Returns (atlas w, atlas h, [(x, y) per level]); every x, y is a multiple of 4."""
    pos = [(0, 0)]
    x, row = 0, align4(dims[0][1])
    for w, _ in dims[1:]:
        pos.append((x, row))
        x += align4(w)
    below = align4(dims[1][1]) if len(dims) > 1 else 0
    return max(align4(dims[0][0]), x), row + below, pos


def build_atlas(levels):
    """levels: H x W x C uint8 arrays. Each level is edge-padded to whole blocks in its place."""
    dims = [(l.shape[1], l.shape[0]) for l in levels]
    aw, ah, pos = atlas_layout(dims)
    atlas = np.zeros((ah, aw, levels[0].shape[2]), dtype=np.uint8)
    for lvl, (x, y) in zip(levels, pos):
        h, w = lvl.shape[:2]
        atlas[y:y + align4(h), x:x + align4(w)] = np.pad(lvl, ((0, align4(h) - h), (0, align4(w) - w), (0, 0)), mode="edge")
    return atlas, pos


def slice_levels(blocks, fmt, atlas_w, dims, pos):
    """blocks: the atlas's block payload (row-major). Returns the levels' blocks, concatenated."""
    bpb, bw = BLOCK_BYTES[fmt], atlas_w // 4
    out = []
    for (w, h), (x, y) in zip(dims, pos):
        n = (w + 3) // 4
        for by in range((h + 3) // 4):
            start = ((y // 4 + by) * bw + x // 4) * bpb
            out.append(blocks[start:start + n * bpb])
    return b"".join(out)


# ---- DDS ----

def dds_header(fmt, w, h, levels):
    """'DDS ' + DDS_HEADER (124 bytes) + DDS_HEADER_DXT10 (20 bytes)."""
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000   # CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE
    caps = 0x1000 | (0x400008 if levels > 1 else 0)         # TEXTURE (+ MIPMAP COMPLEX)
    hdr = struct.pack("<7I", 124, flags, h, w, level_bytes(fmt, w, h), 0, levels) + b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 0x4, b"DX10", 0, 0, 0, 0, 0)   # pixel format: FOURCC DX10
    hdr += struct.pack("<5I", caps, 0, 0, 0, 0)
    dx10 = struct.pack("<5I", DXGI[fmt], 3, 0, 1, 0)   # TEXTURE2D, array size 1
    return b"DDS " + hdr + dx10


def dds_payload(data):
    """(DXGI format or FourCC, w, h, payload) of a DDS file an encoder wrote."""
    if data[:4] != b"DDS " or len(data) < 128:
        raise ValueError("not a DDS file")
    h, w = struct.unpack_from("<2I", data, 12)
    fourcc = data[84:88]
    if fourcc == b"DX10":
        return struct.unpack_from("<I", data, 128)[0], w, h, data[148:]
    return fourcc.decode("latin-1"), w, h, data[128:]


# ---- mips ----

def _lanczos3(x):
    x = np.abs(x)
    out = np.sinc(x) * np.sinc(x / 3.0)
    out[x >= 3.0] = 0.0
    return out


def _weights(n_in, n_out):
    """(indices, weights): n_out x taps, Lanczos-3 widened by the scale, edges clamped."""
    scale = n_in / n_out
    support = 3.0 * scale
    taps = int(np.ceil(2 * support)) + 2
    centers = (np.arange(n_out) + 0.5) * scale
    first = np.floor(centers - support).astype(np.int64)
    idx = first[:, None] + np.arange(taps)[None, :]
    wts = _lanczos3((idx + 0.5 - centers[:, None]) / scale)
    wts /= wts.sum(axis=1, keepdims=True)
    return np.clip(idx, 0, n_in - 1), wts.astype(np.float32)


def _resample(img, w, h):
    """img: H x W x C float32 -> h x w x C (separable Lanczos-3)."""
    if img.shape[0] != h:
        idx, wts = _weights(img.shape[0], h)
        out = np.zeros((h,) + img.shape[1:], dtype=np.float32)
        for t in range(idx.shape[1]):
            out += wts[:, t, None, None] * img[idx[:, t]]
        img = out
    if img.shape[1] != w:
        idx, wts = _weights(img.shape[1], w)
        out = np.zeros((img.shape[0], w, img.shape[2]), dtype=np.float32)
        for t in range(idx.shape[1]):
            out += wts[None, :, t, None] * img[:, idx[:, t]]
        img = out
    return img


def _box(prev, w, h):
    """The runtime's texpack_mips step: 2x2 box average, rounded, odd edges clamped."""
    sh, sw = prev.shape[:2]
    x0 = np.minimum(2 * np.arange(w), sw - 1)
    x1 = np.minimum(2 * np.arange(w) + 1, sw - 1)
    y0 = np.minimum(2 * np.arange(h), sh - 1)
    y1 = np.minimum(2 * np.arange(h) + 1, sh - 1)
    p = prev.astype(np.uint32)
    s = p[y0][:, x0] + p[y0][:, x1] + p[y1][:, x0] + p[y1][:, x1]
    return ((s + 2) // 4).astype(np.uint8)


def _lanczos_step(prev, w, h):
    """One level from the previous one, alpha premultiplied (texels with alpha 0 keep the
    unweighted color, so cutout edges do not darken)."""
    f = prev.astype(np.float32)
    if prev.shape[2] == 3 or prev[..., 3].min() == 255:
        out = _resample(f, w, h)
    else:
        a = f[..., 3:4] / 255.0
        pm = _resample(np.concatenate([f[..., :3] * a, f[..., 3:4]], axis=2), w, h)
        straight = _resample(f[..., :3], w, h)
        a2 = pm[..., 3:4] / 255.0   # unclipped, so color = premultiplied / alpha stays exact
        color = np.where(a2 > 1.0 / 512, pm[..., :3] / np.maximum(a2, 1e-6), straight)
        out = np.concatenate([color, pm[..., 3:4]], axis=2)
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def coverage(alpha, ref):
    return float((alpha >= ref).mean())


def keep_coverage(level, target, ref):
    """Scales the level's alpha so the share of texels with alpha >= ref matches target."""
    a = level[..., 3].astype(np.float32)
    lo, hi = 0.0, 8.0
    for _ in range(20):
        mid = (lo + hi) / 2
        if coverage(np.clip(a * mid, 0, 255), ref) < target:
            lo = mid
        else:
            hi = mid
    out = level.copy()
    out[..., 3] = np.clip(np.rint(a * hi), 0, 255).astype(np.uint8)
    return out


def mip_chain(img, mip_filter="lanczos3", coverage_ref=None):
    """img: H x W x C uint8 -> [level 0 ... 1x1]. coverage_ref: alpha-test reference to keep the
    level 0 coverage of (None: off; RGBA only)."""
    levels = [img]
    target = coverage(img[..., 3], coverage_ref) if coverage_ref is not None else None
    for w, h in level_dims(img.shape[1], img.shape[0])[1:]:
        prev = levels[-1]
        nxt = _box(prev, w, h) if mip_filter == "box" else _lanczos_step(levels[-1], w, h)
        if target is not None:
            nxt = keep_coverage(nxt, target, coverage_ref)
        levels.append(nxt)
    return levels


# ---- pack and classification ----

def read_categories(pack, extra=None):
    """hash -> category from selection.csv / manifest.csv (any file with hash + category)."""
    cats = {}
    for path in [os.path.join(pack, "selection.csv"), os.path.join(pack, "manifest.csv")] + ([extra] if extra else []):
        if not os.path.exists(path):
            continue
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                h, c = (r.get("hash") or "").lower(), (r.get("category") or "").strip().lower()
                if h and c and h not in cats:
                    cats[h] = c
    return cats


def load_skip_list(path):
    if not path:
        return set()
    with open(path) as f:
        return {ln.split("#")[0].strip().lower() for ln in f if ln.split("#")[0].strip()}


def classify(rgba, category, opaque_fmt, ui_heuristic=True, skipped=False):
    """'png' (copy), BC7 or opaque_fmt."""
    if skipped or category in UI_CATEGORIES:
        return "png"
    has_alpha = rgba.shape[2] == 4 and int(rgba[..., 3].min()) < 255
    if has_alpha and ui_heuristic and tu.ui_like(rgba):
        return "png"
    return BC7 if has_alpha else opaque_fmt


def up_to_date(src, dst):
    return os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src)


def find_encoder(path):
    path = path or os.environ.get(ENCODER_ENV) or shutil.which("bc7enc")
    if path and os.path.isfile(path) and os.access(path, os.X_OK):
        return path
    return None


def encoder_args(fmt, quality):
    if fmt == BC1:
        return ["-1", "-b"]   # -b: no 3-color transparent black (the runtime samples BC1 alpha)
    return ["-u%d" % quality]


def encode(encoder, fmt, levels, quality, work):
    """Block payload of every level (one encoder call over the atlas)."""
    dims = [(l.shape[1], l.shape[0]) for l in levels]
    atlas, pos = build_atlas(levels)
    src, dst = os.path.join(work, "atlas.png"), os.path.join(work, "atlas.dds")
    tu.write_png(src, atlas if fmt == BC7 else atlas[..., :3])
    env = dict(os.environ, OMP_NUM_THREADS="1")
    res = subprocess.run([encoder, "-q", "-g"] + encoder_args(fmt, quality) + [src, dst], capture_output=True, env=env)
    if res.returncode or not os.path.exists(dst):
        raise RuntimeError("encoder failed (%d): %s" % (res.returncode, (res.stdout + res.stderr).decode(errors="replace")[-300:]))
    with open(dst, "rb") as f:
        kind, w, h, blocks = dds_payload(f.read())
    want = level_bytes(fmt, atlas.shape[1], atlas.shape[0])
    if (w, h) != (atlas.shape[1], atlas.shape[0]) or len(blocks) != want or kind not in (DXGI[fmt], "DXT1" if fmt == BC1 else None):
        raise RuntimeError("encoder output %r %dx%d with %d bytes, expected %dx%d with %d" % (
            kind, w, h, len(blocks), atlas.shape[1], atlas.shape[0], want))
    return slice_levels(blocks, fmt, atlas.shape[1], dims, pos)


def convert_one(job):
    """One texture; returns a result dict (never raises)."""
    h, src, out, opts = job
    t0 = time.time()
    res = {"hash": h, "kind": None, "in": os.path.getsize(src), "out": 0, "rgba": 0, "s": 0.0, "error": None}
    try:
        tu.PNGRAW = opts["pngraw"]
        rgba = tu.read_png(src)
        kind = classify(rgba, opts["categories"].get(h), opts["opaque"], opts["ui_heuristic"], h in opts["skip"])
        res["kind"] = kind
        res["rgba"] = sum(w * hh * 4 for w, hh in level_dims(rgba.shape[1], rgba.shape[0]))
        dds, png = os.path.join(out, h + ".dds"), os.path.join(out, h + ".png")
        if kind == "png" or opts["with_png"]:
            if opts["force"] or not up_to_date(src, png):
                shutil.copyfile(src, png)
            res["out"] += os.path.getsize(png)
        if kind == "png":
            if os.path.exists(dds):
                os.remove(dds)   # a stale .dds would win over the copied PNG
            return res
        if not opts["force"] and up_to_date(src, dds):
            res["kind"] += "-up-to-date"
            res["out"] += os.path.getsize(dds)
            return res
        ref = opts["coverage_ref"] if (opts["coverage"] == "all" or (
            opts["coverage"] == "cutout" and opts["categories"].get(h) == "cutout")) and kind == BC7 else None
        levels = mip_chain(rgba, opts["mip_filter"], ref)
        with tempfile.TemporaryDirectory(prefix=".work-", dir=out) as work:
            payload = encode(opts["encoder"], kind, levels, opts["quality"], work)
            tmp = os.path.join(work, "out.dds")
            with open(tmp, "wb") as f:
                f.write(dds_header(kind, rgba.shape[1], rgba.shape[0], len(levels)) + payload)
            os.replace(tmp, dds)
        res["out"] += os.path.getsize(dds)
    except Exception as e:   # reported per texture; the run goes on
        res["error"] = "%s: %s" % (type(e).__name__, e)
    res["s"] = time.time() - t0
    return res


def pack_hashes(pack):
    names = {}
    for n in sorted(os.listdir(pack)):
        stem, ext = os.path.splitext(n)
        if ext.lower() == ".png" and len(stem) == 16 and all(c in "0123456789abcdefABCDEF" for c in stem):
            names[stem.lower()] = os.path.join(pack, n)
    return names


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="input PNG pack directory (<hash>.png files)")
    ap.add_argument("--out", required=True, help="output pack directory (created)")
    ap.add_argument("--encoder", help="bc7enc executable (default: $%s, then bc7enc on PATH)" % ENCODER_ENV)
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1, help="parallel textures (one encoder thread each)")
    ap.add_argument("--opaque", choices=(BC1, BC7), default=BC1, help="format of opaque textures (default bc1)")
    ap.add_argument("--quality", type=int, default=6, choices=range(7), help="BC7 encoder quality level (bc7enc -u, default 6)")
    ap.add_argument("--mip-filter", choices=("lanczos3", "box"), default="lanczos3", help="mip filter (box = the runtime's own)")
    ap.add_argument("--alpha-coverage", choices=("all", "cutout", "none", "off"), default="all",
                    help="keep alpha-test coverage on every level: all = every BC7 texture (default), "
                         "cutout = textures of category cutout, none (or off) = never")
    ap.add_argument("--alpha-ref", type=int, default=128, help="alpha-test reference for --alpha-coverage (default 128)")
    ap.add_argument("--manifest", help="extra CSV with hash,category columns (the pack's selection.csv / manifest.csv are read too)")
    ap.add_argument("--skip-list", help="file with hashes kept as PNG (one per line, # comments)")
    ap.add_argument("--no-ui-heuristic", action="store_true", help="only categories and --skip-list keep PNGs")
    ap.add_argument("--with-png", action="store_true", help="also copy every PNG (fallback for devices without BC support)")
    ap.add_argument("--force", action="store_true", help="redo outputs that are up to date")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)

    if not os.path.isdir(args.pack):
        sys.exit("cannot read the pack %s" % args.pack)
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    encoder = find_encoder(args.encoder)
    if not encoder:
        sys.exit("BC encoder not found: build bc7enc from https://github.com/richgel999/bc7enc_rdo (cmake; see its README) "
                 "and pass --encoder PATH or set %s" % ENCODER_ENV)
    if os.path.realpath(args.out) == os.path.realpath(args.pack):
        sys.exit("--out must differ from --pack")
    os.makedirs(args.out, exist_ok=True)
    opts = {"encoder": encoder, "pngraw": args.pngraw, "opaque": args.opaque, "quality": args.quality,
            "mip_filter": args.mip_filter, "coverage": args.alpha_coverage, "coverage_ref": args.alpha_ref,
            "categories": read_categories(args.pack, args.manifest), "skip": load_skip_list(args.skip_list),
            "ui_heuristic": not args.no_ui_heuristic, "with_png": args.with_png, "force": args.force}
    names = pack_hashes(args.pack)
    jobs = [(h, p, args.out, opts) for h, p in names.items()]
    print("textures: %d PNGs in %s, %d workers, encoder %s" % (len(jobs), args.pack, args.jobs, encoder))
    t0 = time.time()
    results = []
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for i, r in enumerate(ex.map(convert_one, jobs, chunksize=1), 1):
            results.append(r)
            if r["error"]:
                print("  %s: %s" % (r["hash"], r["error"]), file=sys.stderr)
            if i % 100 == 0 or i == len(jobs):
                print("  %d/%d (%.0f s)" % (i, len(jobs), time.time() - t0), flush=True)
    wall = time.time() - t0
    kinds = {}
    for r in results:
        k = r["kind"] if not r["error"] else "failed"
        kinds[k] = kinds.get(k, 0) + 1
    ok = [r for r in results if not r["error"]]
    mib = 1 << 20
    print("done in %.1f s: %s" % (wall, ", ".join("%s %d" % (k, n) for k, n in sorted(kinds.items()))))
    print("size: PNG in %.1f MiB, pack out %.1f MiB; as RGBA8 with mips %.1f MiB" % (
        sum(r["in"] for r in ok) / mib, sum(r["out"] for r in ok) / mib, sum(r["rgba"] for r in ok) / mib))
    return 1 if kinds.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
