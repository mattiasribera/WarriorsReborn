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

Per-class presets (--classes FILE): a texture classifier (not part of this repository) writes
classes.json (runtime hash -> class). With it every texture takes the preset of its class from
PRESETS below (or from --presets FILE, a JSON {class: {field: value}} merged over PRESETS):
skip it, its own spandrel chain, its padding and padding mode, and its alpha handling. Textures
the file does not list keep the command-line behavior, and classified textures skip the UI/font
heuristic (the class decides). Without --classes nothing changes.

Character textures are whole-body atlases (face, skin, hair and clothes in one image), so they
are one class, "character", whose preset names a second preset for the skin ("skin":
"character_skin"). When the skin preset has a different spandrel chain, a character texture is
upscaled by both chains and the skin boxes of its classes.json entry (the face and hands
bounding boxes of regions.bbox, scaled to the dump size) take the skin result, the rest the
character (cloth) result, with a feathered seam (composite()). The boxes are bounding boxes,
not per-texel masks, so a face box also covers the hair and neck around it. With the same chain
(the default) or the ncnn backend it is one run. Old class names in classes.json and preset
files (character_skin_face, character_cloth_leather, character_hair) still load.

UI/font heuristic: some texels are transparent, at least 40% of the texels have alpha 0 and the
visible texels are nearly gray (font atlases, HUD glyphs). It is a heuristic: check the review
page and override with --include-ui or --skip-list.

Classification as the source (texture pack v4): when --dump has no manifest.csv, the rows come
from --classes itself (every classified hash whose <hash>.png is in --dump, e.g.
extracted/texture_classes/full); --review applies the review page's overrides
(review-classes.json {"overrides": {hash: class}}) over the auto classes, and --keep-original
(keep-original.json {"hashes": [...]}) leaves those textures out of the pack (the runtime then
uses the game's own texture). Presets can also give a class:
  chain8:      chain used instead of `chain` when the texture still fits --max-size at 8x
               (e.g. a 4x model followed by a 2x model, for large surfaces);
  alpha_chain: chain for the alpha images (a lighter model; resized to the color's scale);
  bleed:       fill the color under (nearly) transparent texels from the visible neighbors first,
               so the model and filtering never pull garbage color into edges (dark halos);
  anchor:      0..1, pull the low frequencies back to the original's (no tint/brightness shift).
--keep-work DIR keeps the padded inputs and raw outputs (one directory per chain) and makes the
run resumable: finished outputs are not redone. A texture without upscaler output (it failed) is
listed as failed:no-output in selection.csv and the run returns 2.

PNG decoding uses the pngraw helper (see find_pngraw: pngraw.exe on Windows, next to this tool or
in runtime/build; `make -C runtime build/pngraw` or `build/pngraw.exe`), or --pngraw PATH;
numpy is required.
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


def find_pngraw(platform=sys.platform, tool_dir=os.path.dirname(os.path.abspath(__file__)), root=ROOT):
    """Path of the PNG decoder: pngraw.exe on Windows, pngraw elsewhere; next to the tool first
    (a copied tools folder), then runtime/build. When neither exists, the runtime/build path (for
    the error message)."""
    name = "pngraw.exe" if platform == "win32" else "pngraw"
    candidates = [os.path.join(tool_dir, name), os.path.join(root, "runtime", "build", name)]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return candidates[-1]


def missing_pngraw(path):
    exe = "pngraw.exe" if path.lower().endswith(".exe") else "pngraw"
    return ("missing %s: run make -C runtime build/%s, put %s next to this tool, or pass --pngraw PATH"
            % (path, exe, exe))


PNGRAW = find_pngraw()
DEFAULT_ESRGAN = os.path.expanduser("~/tools/realesrgan/realesrgan-ncnn-vulkan")


def default_python(platform=sys.platform, home=os.path.expanduser("~")):
    """The inference venv's interpreter (a Windows venv keeps it in Scripts\\python.exe)."""
    venv = os.path.join(home, "tools", "upscale-venv")
    return os.path.join(venv, "Scripts", "python.exe") if platform == "win32" else os.path.join(venv, "bin", "python")


DEFAULT_PYTHON = default_python()
DEFAULT_MODEL_DIR = os.path.expanduser("~/tools/upscale-models")
DEFAULT_INFER = os.path.join(ROOT, "tools", "spandrel_infer.py")
SCALE = 4   # ncnn backend

# Per-class upscale presets, used with --classes (tune here or override with --presets FILE).
#   skip:  leave the texture out of the pack (the title keeps the original)
#   chain: spandrel model chain for this class, e.g. ["1x-DEDXT", "4x-GameAI-2-0"]; None = the
#          run's --chain (the ncnn backend always uses --model)
#   pad:   padding in source texels; None = --pad
#   wrap:  True pads by wrapping around (seamless repeats under WRAP addressing); False repeats
#          the edge texels (atlases and pictures, whose opposite edge is unrelated)
#   alpha: "separate" upscales alpha as its own grayscale image (the default path); "mask" does
#          the same and then thresholds at 128 when the source alpha is binary, so cutouts
#          (fences, foliage) stay hard-edged
#   skin:  name of the preset whose chain upscales the skin boxes (face, hands) of the texture,
#          composited into this preset's result (character atlases); None = one run
#   chain8, alpha_chain, bleed, anchor: see the module doc (v4)
DEFAULT_PRESET = {"skip": False, "chain": None, "pad": None, "wrap": True, "alpha": "separate", "skin": None,
                  "chain8": None, "alpha_chain": None, "bleed": False, "anchor": 0.0}
PRESETS = {
    "character":               {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate",
                                "skin": "character_skin"},
    "character_skin":          {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate"},
    "environment_tiling":      {"skip": False, "chain": None, "pad": 16,   "wrap": True,  "alpha": "separate"},
    "environment_unique":      {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate"},
    "prop":                    {"skip": False, "chain": None, "pad": 8,    "wrap": True,  "alpha": "separate"},
    "signage_text":            {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate"},
    "decal_alpha":             {"skip": False, "chain": None, "pad": 8,    "wrap": True,  "alpha": "mask"},
    "fx":                      {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate"},
    "ui_font_hud":             {"skip": True,  "chain": None, "pad": None, "wrap": True,  "alpha": "separate"},
    "ui_screen":               {"skip": False, "chain": None, "pad": 8,    "wrap": False, "alpha": "separate"},
    "unknown":                 dict(DEFAULT_PRESET),
}
for _p in PRESETS.values():
    for _k, _v in DEFAULT_PRESET.items():
        _p.setdefault(_k, _v)
OLD_CLASSES = {"character_skin_face": "character", "character_cloth_leather": "character",
               "character_hair": "character"}
OLD_PRESETS = {"character_skin_face": "character_skin", "character_cloth_leather": "character",
               "character_hair": "character"}
SKIN_REGIONS = ("face", "hands")
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


def load_classes(path, review=None):
    """{runtime hash: class} from classes.json ({"textures": {hash: {"class": ...}}}) or a flat
    {hash: class} map, with the "overrides" of the review file `review` applied; {} without a path."""
    if not path:
        return {}
    with open(path) as f:
        data = json.load(f)
    tex = data.get("textures", data)
    out = {}
    for h, v in tex.items():
        c = v["class"] if isinstance(v, dict) else v
        out[h.lower()] = OLD_CLASSES.get(c, c)
    if review:
        with open(review) as f:
            for h, c in json.load(f).get("overrides", {}).items():
                if h.lower() in out:
                    out[h.lower()] = OLD_CLASSES.get(c, c)
    return out


def load_keep_original(path):
    """Hashes of keep-original.json ({"hashes": [...]}, or a plain list); empty without a path."""
    if not path:
        return set()
    with open(path) as f:
        data = json.load(f)
    return {h.lower() for h in (data.get("hashes", []) if isinstance(data, dict) else data)}


LINEAR_FORMATS = {"0x12", "0x1e", "0x1f", "0x3f", "0x40", "0x10", "0x11", "0x13", "0x1d"}   # Xbox LIN_* formats


def rows_from_classes(path, image_dir):
    """Manifest-like rows for every classes.json entry whose <hash>.png is in image_dir: size and
    alpha from the image, linear from the entry's Xbox format code."""
    with open(path) as f:
        tex = json.load(f).get("textures", {})
    rows = []
    for h in sorted(tex):
        v = tex[h] if isinstance(tex[h], dict) else {}
        png = os.path.join(image_dir, h.lower() + ".png")
        if not os.path.exists(png):
            continue
        img = read_png(png)
        fmt = str(v.get("format", ""))
        rows.append({"hash": h.lower(), "format": fmt, "w": img.shape[1], "h": img.shape[0], "levels": 1,
                     "linear": int(fmt.lower() in LINEAR_FORMATS), "has_alpha": int(img[..., 3].min() < 255),
                     "draws": 0, "first_frame": 0})
    return rows


def load_skin_regions(path):
    """{runtime hash: (classes.json size [w, h], [[x0, y0, x1, y1], ...])}: the skin boxes (face,
    hands) of every entry with regions; {} without a path."""
    if not path:
        return {}
    with open(path) as f:
        data = json.load(f)
    out = {}
    for h, v in data.get("textures", data).items():
        if not isinstance(v, dict) or not v.get("regions") or not v.get("size"):
            continue
        bb = v["regions"].get("bbox") or {}
        boxes = [list(bb[k]) for k in SKIN_REGIONS if bb.get(k)]
        if boxes:
            out[h.lower()] = (list(v["size"]), boxes)
    return out


def region_mask(h, w, size, boxes):
    """h x w float32 mask, 1 inside `boxes` ([x0, y0, x1, y1) in texels of a size[0] x size[1]
    texture, scaled to w x h), 0 elsewhere."""
    m = np.zeros((h, w), np.float32)
    sx, sy = w / size[0], h / size[1]
    for x0, y0, x1, y1 in boxes:
        m[int(round(y0 * sy)):int(round(y1 * sy)), int(round(x0 * sx)):int(round(x1 * sx))] = 1.0
    return m


def _blur(a, r, axis):
    """Box blur of radius r along `axis` with edge replication."""
    if r <= 0:
        return a
    pad = [(0, 0)] * a.ndim
    pad[axis] = (r + 1, r)
    c = np.cumsum(np.pad(a, pad, mode="edge"), axis=axis, dtype=np.float64)
    n = a.shape[axis]
    hi = np.take(c, np.arange(2 * r + 1, 2 * r + 1 + n), axis=axis)
    lo = np.take(c, np.arange(0, n), axis=axis)
    return ((hi - lo) / (2 * r + 1)).astype(np.float32)


def composite(base, skin, mask, scale):
    """`skin` where the source-resolution `mask` is 1, `base` elsewhere: the mask is scaled up by
    `scale` and feathered over half a source texel on each side of a box edge."""
    m = np.repeat(np.repeat(mask, scale, axis=0), scale, axis=1)
    r = scale // 2
    m = _blur(_blur(m, r, 0), r, 1)[..., None]
    out = base.astype(np.float32) * (1 - m) + skin.astype(np.float32) * m
    return np.clip(np.rint(out), 0, 255).astype(np.uint8)


def load_presets(path):
    """PRESETS with the {class: {field: value}} overrides of the JSON file at `path` merged in."""
    presets = {c: dict(p) for c, p in PRESETS.items()}
    if path:
        with open(path) as f:
            for c, fields in json.load(f).items():
                c = OLD_PRESETS.get(c, c)
                bad = set(fields) - set(DEFAULT_PRESET)
                if bad:
                    sys.exit("%s: unknown preset field(s) %s for %s" % (path, ", ".join(sorted(bad)), c))
                presets[c] = dict(presets.get(c, DEFAULT_PRESET), **fields)
    return presets


def preset_for(h, classes, presets):
    """(class or None, preset) of a texture; unlisted textures get DEFAULT_PRESET."""
    c = classes.get(h)
    if c is None:
        return None, DEFAULT_PRESET
    return c, presets.get(c, DEFAULT_PRESET)


def select(rows, dump_dir, args, skip_hashes, rejected, classes=None, presets=None, keep=None):
    """Returns (selected rows, [(row, reason)] skipped)."""
    chosen, skipped = [], []
    classes, presets, keep = classes or {}, presets or PRESETS, keep or set()
    for r in rows:
        h = r["hash"]
        cls, preset = preset_for(h, classes, presets)
        if h in keep:
            skipped.append((r, "keep-original"))
        elif h in rejected:
            skipped.append((r, "rejected"))
        elif h in skip_hashes:
            skipped.append((r, "skip-list"))
        elif preset["skip"]:
            skipped.append((r, "class:" + cls))
        elif min(r["w"], r["h"]) < args.min_size:
            skipped.append((r, "small"))
        elif r["linear"] and not args.include_linear:
            skipped.append((r, "linear"))
        elif not os.path.exists(os.path.join(dump_dir, h + ".png")):
            skipped.append((r, "no-image"))
        elif cls is None and not args.include_ui and r["has_alpha"] and ui_like(read_png(os.path.join(dump_dir, h + ".png"))):
            skipped.append((r, "ui-like"))
        else:
            chosen.append(r)
    return chosen, skipped


def wrap_pad(img, pad, wrap=True):
    """Pad by `pad` texels on every side, wrapping around (or repeating the edge texels)."""
    return np.pad(img, ((pad, pad), (pad, pad), (0, 0)), mode="wrap" if wrap else "edge")


def binary_alpha(a):
    """True when every alpha value is near 0 or near 255 (a cutout mask)."""
    return bool(((a < 16) | (a > 239)).all())


def bleed(img, alpha_min=8, passes=64):
    """Fills the color of (nearly) transparent texels with the average of their visible neighbors,
    growing outward (wrap-around), so neither the model nor bilinear/mip filtering pulls the
    garbage color under alpha 0 into visible edges (dark halos). Alpha is unchanged."""
    out = img.copy()
    known = img[..., 3] >= alpha_min
    if known.all() or not known.any():
        return out
    col = np.where(known[..., None], img[..., :3].astype(np.float32), 0.0)
    k = known.astype(np.float32)
    for _ in range(passes):
        if k.all():
            break
        s = sum(np.roll(np.roll(col * k[..., None], dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1))
        n = sum(np.roll(np.roll(k, dy, 0), dx, 1) for dy in (-1, 0, 1) for dx in (-1, 0, 1))
        grow = (k == 0) & (n > 0)
        col[grow] = s[grow] / n[grow][:, None]
        k = np.where(grow, 1.0, k)
    if not k.all():   # unreachable texels (more than `passes` away): mean visible color
        col[k == 0] = img[known][:, :3].mean(axis=0)
    fill = ~known
    out[..., :3][fill] = (col[fill] + 0.5).astype(np.uint8)
    return out


def _blur5(x):
    """Separable [1 4 6 4 1]/16 blur with wrap-around, float32 H x W x C."""
    k = (1.0, 4.0, 6.0, 4.0, 1.0)
    for ax in (0, 1):
        x = sum(w * np.roll(x, s, axis=ax) for w, s in zip(k, (-2, -1, 0, 1, 2))) / 16.0
    return x


def upsample(x, s):
    """Bilinear (texel-center aligned, wrap-around) upsampling of H x W x C float32 by s."""
    if s == 1:
        return x
    h, w = x.shape[:2]

    def axis(n):
        c = (np.arange(n * s) + 0.5) / s - 0.5
        i0 = np.floor(c).astype(int)
        return i0 % n, (i0 + 1) % n, (c - i0).astype(np.float32)
    y0, y1, fy = axis(h)
    x0, x1, fx = axis(w)
    r = x[y0] * (1 - fy)[:, None, None] + x[y1] * fy[:, None, None]
    return r[:, x0] * (1 - fx)[None, :, None] + r[:, x1] * fx[None, :, None]


def refine(src, up, strength=1.0, anchor=1.0):
    """src: H x W x C uint8 original, up: (H*s) x (W*s) x C uint8 model output. Mixes the model's
    detail with a bilinear upscale (strength) and pulls the low frequencies back to the original's
    (anchor: removes tint and brightness shifts without reintroducing DXT blocks)."""
    s = up.shape[0] // src.shape[0]
    x = up.astype(np.float32)
    o = src.astype(np.float32)
    if strength < 1.0:
        x = upsample(o, s) + strength * (x - upsample(o, s))
    if anchor > 0.0:
        low = box_down(np.clip(x + 0.5, 0, 255).astype(np.uint8), s).astype(np.float32) if s > 1 else x
        x = x + anchor * upsample(_blur5(_blur5(o)) - _blur5(_blur5(low)), s)
    return np.clip(x + 0.5, 0, 255).astype(np.uint8)


def sharpen_alpha(a):
    """Binary-alpha cutouts: smoothstep around 50% so the upscaled mask keeps a crisp but
    anti-aliased edge."""
    t = np.clip((a.astype(np.float32) / 255.0 - 0.35) / 0.3, 0.0, 1.0)
    return (t * t * (3 - 2 * t) * 255.0 + 0.5).astype(np.uint8)


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


def run_upscaler(args, indir, outdir, model_paths=None):
    if args.backend == "spandrel":
        cmd = [args.python, "-I", args.infer, "-i", indir, "-o", outdir, "--tile", str(args.tile),
               "--pngraw", PNGRAW]
        if args.gpu is not None:
            cmd += ["--device", "cuda:%d" % args.gpu]
        if args.keep_work:
            cmd += ["--skip-existing"]
        for p in model_paths or args.model_paths:
            cmd += ["--model", p]
    else:
        cmd = [args.esrgan, "-i", indir, "-o", outdir, "-n", args.model, "-s", str(SCALE), "-f", "png"]
        if args.models:
            cmd += ["-m", args.models]
        if args.gpu is not None:
            cmd += ["-g", str(args.gpu)]
    t0 = time.monotonic()
    print("upscaler: %s" % " ".join(os.path.basename(c) for c in cmd[cmd.index("--model"):] if c != "--model")
          if "--model" in cmd else "upscaler: %s" % args.model, flush=True)
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    dt = time.monotonic() - t0
    for line in res.stdout.splitlines():
        if line.startswith(("failed:", "tile ", "model ")) or " images through " in line:
            print("  " + line, flush=True)
    if res.returncode:
        sys.exit("upscaler failed (%d): %s\n%s" % (res.returncode, " ".join(cmd), res.stderr[-2000:]))
    return dt


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pngraw", default=PNGRAW, help="PNG decoder (default: %(default)s)")
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
    ap.add_argument("--classes", help="classes.json (runtime hash -> class): per-class presets (see PRESETS); "
                    "also the texture list when --dump has no manifest.csv")
    ap.add_argument("--review", help="review-classes.json: its overrides replace the classes.json classes")
    ap.add_argument("--keep-original", help="keep-original.json: these hashes stay out of the pack (game texture)")
    ap.add_argument("--presets", help="JSON {class: {field: value}} merged over PRESETS (needs --classes)")
    ap.add_argument("--force", action="store_true", help="redo textures already in the pack")
    ap.add_argument("--keep-work", help="keep the padded inputs and raw outputs in this directory (resumable)")
    return ap


def main(argv=None):
    global PNGRAW
    args = build_parser().parse_args(argv)
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
    if not os.path.isfile(args.pngraw):
        sys.exit(missing_pngraw(args.pngraw))
    PNGRAW = os.path.abspath(args.pngraw)
    manifest = os.path.join(args.dump, "manifest.csv")
    if not os.path.exists(manifest) and args.classes:
        rows = rows_from_classes(args.classes, args.dump)
    else:
        rows = read_manifest(manifest)
    rejected = load_rejections(args.reject)
    classes = load_classes(args.classes, args.review)
    skin_regions = load_skin_regions(args.classes)
    presets = load_presets(args.presets)
    chosen, skipped = select(rows, args.dump, args, load_skip_list(args.skip_list), rejected, classes, presets,
                             load_keep_original(args.keep_original))
    pack = os.path.join(args.out, args.model)
    os.makedirs(pack, exist_ok=True)
    for r, why in skipped:
        stale = os.path.join(pack, r["hash"] + ".png")
        if (why in ("rejected", "skip-list", "keep-original") or why.startswith("class:")) and os.path.exists(stale):
            os.remove(stale)
    todo = [r for r in chosen if args.force or not os.path.exists(os.path.join(pack, r["hash"] + ".png"))]
    print("textures: %d in the manifest, %d selected (%d to upscale), %d skipped (%s)" % (
        len(rows), len(chosen), len(todo), len(skipped),
        ", ".join("%s %d" % (k, sum(1 for _, w in skipped if w == k)) for k in sorted({w for _, w in skipped}))))
    if classes:
        per = {}
        for r in todo:
            c = classes.get(r["hash"], "unlisted")
            per[c] = per.get(c, 0) + 1
        print("to upscale by class: %s" % ", ".join("%s %d" % kv for kv in sorted(per.items())))

    # one upscaler run per distinct model chain (presets may give a class its own chain)
    def chain_paths(preset):
        if args.backend == "spandrel" and preset["chain"]:
            return tuple(resolve_model(args.model_dir, c) for c in preset["chain"])
        return None

    def group(paths):
        if paths not in groups:
            groups.append(paths)
        return groups.index(paths)

    groups, plan, skin_plan, alpha_plan = [], {}, {}, {}
    for r in todo:
        cls, preset = preset_for(r["hash"], classes, presets)
        paths = chain_paths(preset)
        if preset.get("chain8") and args.backend == "spandrel" and max(r["w"], r["h"]) * 8 <= args.max_size:
            paths = tuple(resolve_model(args.model_dir, c) for c in preset["chain8"])
        pad = args.pad if preset["pad"] is None else preset["pad"]
        plan[r["hash"]] = (group(paths), pad, preset["wrap"], preset["alpha"], cls)
        if r["has_alpha"] and preset.get("alpha_chain") and args.backend == "spandrel":
            alpha_plan[r["hash"]] = group(tuple(resolve_model(args.model_dir, c) for c in preset["alpha_chain"]))
        # character atlases: the skin boxes also go through the skin preset's chain (same padding)
        if preset.get("skin") and r["hash"] in skin_regions:
            spaths = chain_paths(presets.get(preset["skin"], DEFAULT_PRESET))
            if spaths != paths:
                if spaths not in groups:
                    groups.append(spaths)
                skin_plan[r["hash"]] = groups.index(spaths)
    if skin_plan:
        print("character skin regions composited from a second chain: %d textures" % len(skin_plan))

    # work files next to the pack (not in /tmp: padded inputs and 4x outputs are hundreds of MB)
    work = args.keep_work or tempfile.mkdtemp(prefix=".work-", dir=args.out)

    def gdir(g, kind):
        """One directory per chain, named after it, so a resumed run finds its own outputs."""
        paths = groups[g] if g < len(groups) else None
        if not paths:
            return os.path.join(work, kind)
        return os.path.join(work, kind + "-" + "+".join(os.path.splitext(os.path.basename(p))[0] for p in paths))

    for g in range(len(groups) or 1):
        for d in (gdir(g, "in"), gdir(g, "out")):
            if not args.keep_work:
                shutil.rmtree(d, ignore_errors=True)
            os.makedirs(d, exist_ok=True)

    def put(g, name, img):
        path = os.path.join(gdir(g, "in"), name)
        if not (args.keep_work and os.path.exists(path)):
            write_png(path, img)
    gpu_s, out_px, failed = 0.0, 0, {}
    try:
        binary, sources = {}, {}
        for r in todo:
            h = r["hash"]
            g, pad, wrap, _, cls = plan[h]
            preset = preset_for(h, classes, presets)[1]
            img = read_png(os.path.join(args.dump, h + ".png"))
            binary[h] = binary_alpha(img[..., 3])
            if preset.get("bleed") and r["has_alpha"]:
                img = bleed(img)
            if preset.get("anchor"):
                sources[h] = img[..., :3]
            padded = wrap_pad(img, pad, wrap)
            put(g, h + ".png", padded[..., :3])
            if h in skin_plan:
                put(skin_plan[h], h + ".png", padded[..., :3])
            if r["has_alpha"]:
                put(alpha_plan.get(h, g), h + "_a.png", np.repeat(padded[..., 3:4], 3, axis=2))
        for g, paths in enumerate(groups):
            gpu_s += run_upscaler(args, gdir(g, "in"), gdir(g, "out"), list(paths) if paths else None)
        for r in todo:
            h = r["hash"]
            g, pad, _, alpha_mode, cls = plan[h]
            outdir = gdir(g, "out")
            need = [os.path.join(outdir, h + ".png")]
            if r["has_alpha"]:
                need.append(os.path.join(gdir(alpha_plan.get(h, g), "out"), h + "_a.png"))
            if h in skin_plan:
                need.append(os.path.join(gdir(skin_plan[h], "out"), h + ".png"))
            if not all(os.path.exists(p) for p in need):
                failed[h] = "no-output"
                continue
            raw = read_png(need[0])
            ph, pw = r["h"] + 2 * pad, r["w"] + 2 * pad
            scale = raw.shape[0] // ph
            if scale < 1 or raw.shape[0] != ph * scale or raw.shape[1] != pw * scale:
                sys.exit("%s: upscaler output %dx%d is not an integer multiple of %dx%d" % (
                    h, raw.shape[1], raw.shape[0], pw, ph))
            color = crop(raw, pad, scale)
            if h in skin_plan:
                sraw = read_png(os.path.join(gdir(skin_plan[h], "out"), h + ".png"))
                if sraw.shape[:2] != raw.shape[:2]:
                    sys.exit("%s: skin chain output %dx%d differs from %dx%d" % (
                        h, sraw.shape[1], sraw.shape[0], raw.shape[1], raw.shape[0]))
                mask = region_mask(r["h"], r["w"], *skin_regions[h])
                color = composite(color[..., :3], crop(sraw, pad, scale)[..., :3], mask, scale)
            if h in sources:
                color = refine(sources[h], color[..., :3], 1.0, float(preset_for(h, classes, presets)[1]["anchor"]))
            out = np.empty(color.shape[:2] + (4,), dtype=np.uint8)
            out[..., :3] = color[..., :3]
            if r["has_alpha"]:
                araw = read_png(need[1])
                gray = (araw[..., :3].astype(np.float32).sum(axis=2) / 3.0)[..., None]
                sa = araw.shape[0] // ph
                if sa < 1 or araw.shape[0] != ph * sa or araw.shape[1] != pw * sa or scale % sa:
                    failed[h] = "alpha-scale"
                    continue
                if sa != scale:   # lighter alpha chain: resize its result to the color's scale
                    gray = upsample(gray, scale // sa)
                a = crop(gray, pad, scale)[..., 0]
                out[..., 3] = np.clip(a + 0.5, 0, 255).astype(np.uint8)
                if alpha_mode == "mask" and binary[h]:
                    out[..., 3] = sharpen_alpha(out[..., 3])
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
        wr.writerow(["hash", "format", "w", "h", "status", "class"])
        for r in chosen:
            status = "failed:" + failed[r["hash"]] if r["hash"] in failed else "upscaled"
            wr.writerow([r["hash"], r["format"], r["w"], r["h"], status, classes.get(r["hash"], "")])
        for r, why in skipped:
            wr.writerow([r["hash"], r["format"], r["w"], r["h"], "skipped:" + why, classes.get(r["hash"], "")])
    in_px = sum(r["w"] * r["h"] for r in todo)
    print("upscaled %d textures (%.2f Mtexels in, %.2f Mtexels out) in %.1f s of upscaler time (model %s); pack: %s" % (
        len(todo) - len(failed), in_px / 1e6, out_px / 1e6, gpu_s, args.model, pack))
    if failed:
        print("failed %d: %s" % (len(failed), ", ".join("%s (%s)" % kv for kv in sorted(failed.items()))))
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
