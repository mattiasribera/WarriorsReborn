#!/usr/bin/env python3
"""Builds an AI-upscaled texture pack from a texture dump (xbrun --dump-textures DIR).

Reads DIR/manifest.csv and the <hash>.png files, selects textures, upscales them and writes the
pack as <out>/<name>/<hash>.png, the layout `xbrun --texture-pack <out>/<name>` loads. Backends:
- ncnn (default): realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN, the official
  release unpacked into ~/tools/realesrgan); <name> is --model;
- spandrel: a chain of PyTorch models (--chain 1x-DEDXT,4x-GameAI-2-0, files looked up in
  --model-dir) run by tools/spandrel_infer.py inside the venv given by --python; <name> is the
  chain joined with '+' unless --name is given. The chain's total scale is taken from the output. Everything stays local: dumps and packs are derived
from the user's own game data and belong under extracted/ (gitignored); never publish them.

Per texture:
- selection (defaults; see --help): textures smaller than --min-size on either side, LIN_*
  (linear) textures and UI/font-like textures are skipped; --include-linear / --include-ui /
  --min-size change that, --skip-list FILE skips listed hashes, --reject FILE applies a review
  page export (tools/texture_review.py) and deletes rejected textures from the pack;
- the color is wrap-padded by --pad texels on every side (the title samples almost every
  texture with WRAP addressing, so tiling textures stay seamless), upscaled 4x and cropped back;
- textures with alpha get their alpha channel upscaled separately as a grayscale image (same
  padding) and recombined, so the model never sees premultiplied or garbage colors as alpha;
- results larger than --max-size on the longer side are box-filtered down by powers of two.

UI/font heuristic: some texels are transparent, at least 40% of the texels have alpha 0 and the
visible texels are nearly gray (font atlases, HUD glyphs). It is a heuristic: check the review
page and override with --include-ui or --skip-list.

PNG decoding uses runtime/build/pngraw (built by `make -C runtime`); numpy is required.
"""
import argparse
import csv
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PNGRAW = os.path.join(ROOT, "runtime", "build", "pngraw")
DEFAULT_ESRGAN = os.path.expanduser("~/tools/realesrgan/realesrgan-ncnn-vulkan")
DEFAULT_PYTHON = os.path.expanduser("~/tools/upscale-venv/bin/python")
DEFAULT_MODEL_DIR = os.path.expanduser("~/tools/upscale-models")
DEFAULT_INFER = os.path.join(ROOT, "tools", "spandrel_infer.py")
SCALE = 4   # ncnn backend
MODEL_EXTS = (".safetensors", ".pth")   # preference order


def read_manifest(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for k in ("w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"):
            r[k] = int(r[k])
    return rows


def read_png(path):
    """H x W x 4 uint8 RGBA (any PNG color type)."""
    res = subprocess.run([PNGRAW, path], capture_output=True, check=True)
    nl = res.stdout.index(b"\n")
    w, h = map(int, res.stdout[:nl].split())
    return np.frombuffer(res.stdout[nl + 1:], dtype=np.uint8).reshape(h, w, 4).copy()


def write_png(path, img):
    """img: H x W x 3 (RGB) or H x W x 4 (RGBA) uint8."""
    h, w, c = img.shape
    rows = np.zeros((h, w * c + 1), dtype=np.uint8)
    rows[:, 1:] = img.reshape(h, w * c)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if c == 4 else 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(rows.tobytes(), 6))
                + chunk(b"IEND", b""))


def ui_like(rgba):
    a = rgba[..., 3]
    if a.min() == 255:
        return False
    if (a == 0).mean() < 0.4:
        return False
    vis = rgba[a > 0][:, :3].astype(np.int16)
    if not len(vis):
        return True
    spread = vis.max(axis=1) - vis.min(axis=1)
    return bool((spread < 24).mean() >= 0.95)


def select(rows, dump_dir, args, skip_hashes, rejected):
    """Returns (selected rows, [(row, reason)] skipped)."""
    chosen, skipped = [], []
    for r in rows:
        h = r["hash"]
        if h in rejected:
            skipped.append((r, "rejected"))
        elif h in skip_hashes:
            skipped.append((r, "skip-list"))
        elif min(r["w"], r["h"]) < args.min_size:
            skipped.append((r, "small"))
        elif r["linear"] and not args.include_linear:
            skipped.append((r, "linear"))
        elif not os.path.exists(os.path.join(dump_dir, h + ".png")):
            skipped.append((r, "no-image"))
        elif not args.include_ui and r["has_alpha"] and ui_like(read_png(os.path.join(dump_dir, h + ".png"))):
            skipped.append((r, "ui-like"))
        else:
            chosen.append(r)
    return chosen, skipped


def wrap_pad(img, pad):
    return np.pad(img, ((pad, pad), (pad, pad), (0, 0)), mode="wrap")


def crop(img, pad, scale):
    p = pad * scale
    return img[p:img.shape[0] - p, p:img.shape[1] - p]


def box_down(img, factor):
    h, w, c = img.shape
    img = img[: h - h % factor, : w - w % factor].astype(np.uint32)
    s = img.reshape(h // factor, factor, w // factor, factor, c).sum(axis=(1, 3))
    return ((s + factor * factor // 2) // (factor * factor)).astype(np.uint8)


def fit(img, max_size):
    f = 1
    while max(img.shape[0], img.shape[1]) // f > max_size and min(img.shape[0], img.shape[1]) // f > 1:
        f *= 2
    return box_down(img, f) if f > 1 else img


def load_rejections(path):
    """Review page export: {"rejected": [hash, ...]} (other keys ignored)."""
    if not path:
        return set()
    with open(path) as f:
        data = json.load(f)
    return {h.lower() for h in data.get("rejected", [])}


def load_skip_list(path):
    if not path:
        return set()
    with open(path) as f:
        return {line.split("#")[0].strip().lower() for line in f if line.split("#")[0].strip()}


def _norm(name):
    return name.lower().replace("_", "-").replace(" ", "-")


def resolve_model(model_dir, name):
    """Model file in model_dir whose stem equals `name` ignoring case, '-', '_' and spaces
    (1x-DEDXT finds 1x_DEDXT.pth); .safetensors is preferred over .pth."""
    want = _norm(name).replace("-", "")
    files = sorted(os.listdir(model_dir)) if os.path.isdir(model_dir) else []
    for ext in MODEL_EXTS:
        for n in files:
            stem, e = os.path.splitext(n)
            if e.lower() == ext and _norm(stem).replace("-", "") == want:
                return os.path.join(model_dir, n)
    sys.exit("model %r not found in %s (expected %s)" % (name, model_dir, " or ".join("<name>" + e for e in MODEL_EXTS)))


def run_upscaler(args, indir, outdir):
    if args.backend == "spandrel":
        cmd = [args.python, "-I", args.infer, "-i", indir, "-o", outdir, "--tile", str(args.tile)]
        if args.gpu is not None:
            cmd += ["--device", "cuda:%d" % args.gpu]
        for p in args.model_paths:
            cmd += ["--model", p]
    else:
        cmd = [args.esrgan, "-i", indir, "-o", outdir, "-n", args.model, "-s", str(SCALE), "-f", "png"]
        if args.models:
            cmd += ["-m", args.models]
        if args.gpu is not None:
            cmd += ["-g", str(args.gpu)]
    t0 = time.monotonic()
    res = subprocess.run(cmd, capture_output=True, text=True)
    dt = time.monotonic() - t0
    if res.returncode:
        sys.exit("upscaler failed (%d): %s\n%s" % (res.returncode, " ".join(cmd), res.stderr[-2000:]))
    return dt


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dump", default=os.path.join(ROOT, "extracted", "texture_dump"), help="dump directory (manifest.csv)")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "texture_pack"), help="pack root; the pack is OUT/MODEL")
    ap.add_argument("--backend", choices=("ncnn", "spandrel"), default="ncnn", help="upscaler backend (see above)")
    ap.add_argument("--model", default="realesrgan-x4plus", help="ncnn: realesrgan-x4plus, realesrgan-x4plus-anime, realesr-animevideov3")
    ap.add_argument("--chain", help="spandrel: comma-separated model names applied in order, e.g. 1x-DEDXT,4x-GameAI-2-0")
    ap.add_argument("--name", help="spandrel: pack name (default: the chain joined with '+')")
    ap.add_argument("--model-dir", default=DEFAULT_MODEL_DIR, help="spandrel: directory with the model files")
    ap.add_argument("--python", default=DEFAULT_PYTHON, help="spandrel: interpreter of the venv with torch + spandrel")
    ap.add_argument("--infer", default=DEFAULT_INFER, help="spandrel: inference script")
    ap.add_argument("--tile", type=int, default=256, help="spandrel: tile size in input texels")
    ap.add_argument("--esrgan", default=DEFAULT_ESRGAN, help="realesrgan-ncnn-vulkan executable")
    ap.add_argument("--models", help="model directory (default: models/ next to the executable)")
    ap.add_argument("--gpu", type=int, default=0, help="GPU id for the upscaler (default 0; auto may pick a CPU device)")
    ap.add_argument("--min-size", type=int, default=16, help="skip textures smaller than this on either side")
    ap.add_argument("--max-size", type=int, default=2048, help="largest output side (box-filtered down by powers of two)")
    ap.add_argument("--pad", type=int, default=16, help="wrap padding in source texels")
    ap.add_argument("--include-linear", action="store_true", help="also upscale LIN_* textures")
    ap.add_argument("--include-ui", action="store_true", help="also upscale UI/font-like textures")
    ap.add_argument("--skip-list", help="file with hashes to skip (one per line, # comments)")
    ap.add_argument("--reject", help="review export JSON ({\"rejected\": [...]}); rejected textures are removed from the pack")
    ap.add_argument("--force", action="store_true", help="redo textures already in the pack")
    ap.add_argument("--keep-work", help="keep the padded inputs and raw outputs in this directory")
    args = ap.parse_args(argv)
    if args.models is None:
        args.models = os.path.join(os.path.dirname(os.path.abspath(args.esrgan)), "models")
        if not os.path.isdir(args.models):
            args.models = None

    if args.backend == "spandrel":
        if not args.chain:
            sys.exit("--backend spandrel needs --chain")
        chain = [c.strip() for c in args.chain.split(",") if c.strip()]
        args.model_paths = [resolve_model(args.model_dir, c) for c in chain]
        args.model = args.name or "+".join(chain)
    if not os.path.exists(PNGRAW):
        sys.exit("missing %s: run make -C runtime" % PNGRAW)
    rows = read_manifest(os.path.join(args.dump, "manifest.csv"))
    rejected = load_rejections(args.reject)
    chosen, skipped = select(rows, args.dump, args, load_skip_list(args.skip_list), rejected)
    pack = os.path.join(args.out, args.model)
    os.makedirs(pack, exist_ok=True)
    for r, why in skipped:
        stale = os.path.join(pack, r["hash"] + ".png")
        if why in ("rejected", "skip-list") and os.path.exists(stale):
            os.remove(stale)
    todo = [r for r in chosen if args.force or not os.path.exists(os.path.join(pack, r["hash"] + ".png"))]
    print("textures: %d in the manifest, %d selected (%d to upscale), %d skipped (%s)" % (
        len(rows), len(chosen), len(todo), len(skipped),
        ", ".join("%s %d" % (k, sum(1 for _, w in skipped if w == k)) for k in sorted({w for _, w in skipped}))))

    # work files next to the pack (not in /tmp: padded inputs and 4x outputs are hundreds of MB)
    work = args.keep_work or tempfile.mkdtemp(prefix=".work-", dir=args.out)
    indir, outdir = os.path.join(work, "in"), os.path.join(work, "out")
    for d in (indir, outdir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    gpu_s, out_px = 0.0, 0
    try:
        images = {}
        for r in todo:
            img = read_png(os.path.join(args.dump, r["hash"] + ".png"))
            images[r["hash"]] = img
            padded = wrap_pad(img, args.pad)
            write_png(os.path.join(indir, r["hash"] + ".png"), padded[..., :3])
            if r["has_alpha"]:
                write_png(os.path.join(indir, r["hash"] + "_a.png"), np.repeat(padded[..., 3:4], 3, axis=2))
        if todo:
            gpu_s = run_upscaler(args, indir, outdir)
        for r in todo:
            h = r["hash"]
            raw = read_png(os.path.join(outdir, h + ".png"))
            ph, pw = r["h"] + 2 * args.pad, r["w"] + 2 * args.pad
            scale = raw.shape[0] // ph
            if scale < 1 or raw.shape[0] != ph * scale or raw.shape[1] != pw * scale:
                sys.exit("%s: upscaler output %dx%d is not an integer multiple of %dx%d" % (
                    h, raw.shape[1], raw.shape[0], pw, ph))
            color = crop(raw, args.pad, scale)
            out = np.empty(color.shape[:2] + (4,), dtype=np.uint8)
            out[..., :3] = color[..., :3]
            if r["has_alpha"]:
                a = crop(read_png(os.path.join(outdir, h + "_a.png")), args.pad, scale)
                out[..., 3] = ((a[..., :3].astype(np.uint16).sum(axis=2) + 1) // 3).astype(np.uint8)
            else:
                out[..., 3] = 255
            out = fit(out, args.max_size)
            write_png(os.path.join(pack, h + ".png"), out)
            out_px += out.shape[0] * out.shape[1]
    finally:
        if not args.keep_work:
            shutil.rmtree(work, ignore_errors=True)

    with open(os.path.join(pack, "selection.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["hash", "format", "w", "h", "status"])
        for r in chosen:
            wr.writerow([r["hash"], r["format"], r["w"], r["h"], "upscaled"])
        for r, why in skipped:
            wr.writerow([r["hash"], r["format"], r["w"], r["h"], "skipped:" + why])
    in_px = sum(r["w"] * r["h"] for r in todo)
    print("upscaled %d textures (%.2f Mtexels in, %.2f Mtexels out) in %.1f s of upscaler time (model %s); pack: %s" % (
        len(todo), in_px / 1e6, out_px / 1e6, gpu_s, args.model, pack))
    return 0


if __name__ == "__main__":
    sys.exit(main())
