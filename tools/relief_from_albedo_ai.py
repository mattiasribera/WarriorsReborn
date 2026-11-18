#!/usr/bin/env python3
"""Generates relief and specular maps (<hash>_n.png, <hash>_s.png) for a PNG texture pack with a local AI estimator.

The successor of tools/normal_from_albedo.py (which guesses height from brightness): the normals come from DeepBump
(https://github.com/HugoTini/DeepBump, GPL-3.0), a small open network trained to infer the relief of a material from
its colour picture (mortar between bricks, cobbles, wood grain, dents in sheet metal). It runs locally through
onnxruntime (CUDA when available, else CPU). The weights are not part of this repository: on first use the tool
downloads deepbump256.onnx (26 MB, pinned commit and SHA-256) into ~/.cache/relief-ai/models (or --model PATH).

For every <hash>.png in --pack it writes into --out (never into the pack itself), like the stopgap:
  - <hash>_n.png: a tangent-space normal map, OpenGL convention (+Y = green points up the image;
    runtime/src/normal_maps.h). The texture (at most --max-size, default 512) is turned grey and inferred in
    overlapping 256 px tiles (--overlap) blended with smooth weights, so there are no tile seams; the borders are
    padded by wrapping for textures that tile (class environment_tiling) and by mirroring for the others
    (--padding). The network's slopes are then high-passed (--flatten-radius: a texture's broad tilt is removed,
    a tiling surface has none) and scaled by --strength (default 2: the network's relief is shallow; class
    character x 0.35, as it reads a character atlas as big bulges). Transparent texels (alpha below 128) are flat
    and are filled from their opaque neighbours before inference (no false edge along the alpha border). Alpha is 255.
  - <hash>_s.png (unless --no-specular): R = intensity, G = gloss, at half the normal map's size. Smooth surfaces
    (little fine luminance detail: metal, glass, paint) get a brighter, tighter highlight than rough ones (asphalt,
    cloth, dirt); cavities of the height integrated from the normals (Frankot-Chellappa) are dimmed; saturated
    colours shine less; class character (cloth, skin) is scaled by 0.6. Intensity at most --spec-max, gloss
    between --gloss-min and --gloss-max.

Classes come from the pack's selection.csv / manifest.csv (column class or category, or --manifest); classes
ui_screen, ui_font_hud and fx are skipped (--skip-class), as are UI and font textures by the stopgap's rules and
textures smaller than 8 x 8. Select a subset with --hash H (repeatable) or --hashes FILE. With --dds the maps are
also written as BC7 .dds (bc7enc: --encoder or $TEXPACK_BC_ENCODER), 4x smaller in the renderer's texture cache.

Resumable: a texture whose maps all exist is skipped (--force redoes it); every file is written to a temporary name
and renamed. Progress goes to stdout. Pass --out to the game as one more --texture-pack with --normal-maps (and
--char-lighting / --world-lighting, whose lights shade the relief). Needs numpy and onnxruntime (or
onnxruntime-gpu); PNG decoding uses the pngraw helper (--pngraw).
"""
import argparse
import concurrent.futures
import csv
import hashlib
import os
import sys
import time
import urllib.request

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import normal_from_albedo as nfa  # noqa: E402  (blur, skip_reason, write_dds, read_hash_list, MIN_SIZE)
import texture_compress as tc  # noqa: E402  (pack_hashes, load_skip_list, find_encoder, ENCODER_ENV)
import texture_upscale as tu  # noqa: E402  (read_png, write_png, fit, PNGRAW)

MODEL_URL = "https://github.com/HugoTini/DeepBump/raw/ef62c0af8a5742a966e557af1b02ab0e5539a28d/deepbump256.onnx"
MODEL_SHA256 = "3a2ababc5fa652b19040a0f3d639b71826368e52ad805cf70ffb0e7c96a54d67"
DEFAULT_MODEL = os.path.join(os.path.expanduser("~"), ".cache", "relief-ai", "models", "deepbump256.onnx")
TILE = 256
SKIP_CLASSES = ("ui_screen", "ui_font_hud", "fx")
TILING_CLASSES = ("environment_tiling",)
CLASS_SPECULAR = {"character": 0.6}
CLASS_STRENGTH = {"character": 0.35}   # the network reads a character atlas as big bulges (folds, limbs)
MIN_Z = 0.05   # flattest normal z kept from the network (caps a slope at 20)


# ---- model -------------------------------------------------------------------------------------------------

def ensure_model(path, url=MODEL_URL, sha256=MODEL_SHA256, download=True):
    """The model file at path, downloaded from url (and checked against sha256) when missing."""
    if os.path.isfile(path):
        return path
    if not download:
        raise FileNotFoundError("model %s missing (run without --no-download to fetch it, or pass --model PATH)" % path)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    part = path + ".part"
    print("downloading %s -> %s" % (url, path), flush=True)
    req = urllib.request.Request(url, headers={"User-Agent": "relief-from-albedo-ai"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=120) as r, open(part, "wb") as f:
        for block in iter(lambda: r.read(1 << 20), b""):
            digest.update(block)
            f.write(block)
    if sha256 and digest.hexdigest() != sha256:
        os.remove(part)
        raise RuntimeError("model download %s: SHA-256 %s, expected %s" % (url, digest.hexdigest(), sha256))
    os.replace(part, path)
    return path


class OnnxEstimator:
    """Callable: H x W float32 grey tile (0-1) -> 3 x H x W float32 encoded normal (n * 0.5 + 0.5)."""

    def __init__(self, path, device="auto", threads=4):
        import onnxruntime as ort
        for name in ("preload_dlls", "disable_telemetry_events"):   # CUDA libraries from pip; no telemetry
            try:
                getattr(ort, name)()
            except Exception:
                pass
        so = ort.SessionOptions()
        so.intra_op_num_threads = max(1, threads)
        so.inter_op_num_threads = 1
        so.log_severity_level = 3
        avail = ort.get_available_providers()
        want = {"cpu": [], "cuda": ["CUDAExecutionProvider"],
                "auto": [p for p in ("CUDAExecutionProvider",) if p in avail]}[device]
        self.session = ort.InferenceSession(path, so, providers=want + ["CPUExecutionProvider"])
        self.providers = self.session.get_providers()
        self.input = self.session.get_inputs()[0].name

    def __call__(self, tile):
        return self.session.run(None, {self.input: tile[None, None].astype(np.float32)})[0][0]


# ---- inference ---------------------------------------------------------------------------------------------

def tile_weight(tile, overlap):
    """tile x tile blending weight: 1 inside, ramping down over `overlap` texels to each edge (never 0)."""
    i = np.arange(tile, dtype=np.float32) + 0.5
    r = np.minimum(1.0, np.minimum(i, tile - i) / max(1, overlap))
    return np.outer(r, r).astype(np.float32)


def infer_tiled(grey, predict, tile=TILE, overlap=64, mode="wrap"):
    """3 x H x W prediction of an H x W grey image from overlapping tile x tile predictions. The image is padded by
    `overlap` on every side (mode "wrap" for tiling textures, "reflect" otherwise) and more at the bottom/right to
    fill the tile grid, so every texel is predicted away from a tile border and blended across tiles."""
    h, w = grey.shape
    stride = tile - overlap

    def grid(n):
        k = 1 if n + 2 * overlap <= tile else -(-(n + 2 * overlap - tile) // stride) + 1
        return k, (k - 1) * stride + tile

    ny, ph = grid(h)
    nx, pw = grid(w)
    padded = np.pad(grey, ((overlap, ph - h - overlap), (overlap, pw - w - overlap)), mode=mode).astype(np.float32)
    acc = np.zeros((3, ph, pw), dtype=np.float32)
    wsum = np.zeros((ph, pw), dtype=np.float32)
    wt = tile_weight(tile, overlap)
    for ty in range(ny):
        for tx in range(nx):
            y, x = ty * stride, tx * stride
            pred = np.asarray(predict(padded[y:y + tile, x:x + tile]), dtype=np.float32)
            acc[:, y:y + tile, x:x + tile] += pred * wt
            wsum[y:y + tile, x:x + tile] += wt
    return (acc / wsum)[:, overlap:overlap + h, overlap:overlap + w]


def network_input(rgba):
    """H x W grey (mean of RGB, 0-1); transparent texels take the colour of the nearby opaque ones (no false edges)."""
    grey = rgba[..., :3].astype(np.float32).mean(axis=-1) / 255.0
    if rgba.shape[2] == 4:
        clear = rgba[..., 3] < 128
        if clear.all():
            grey[:] = 0.5
        elif clear.any():
            opaque = (~clear).astype(np.float32)
            todo = clear.copy()
            for r in (2, 8, 32, 128):   # nearest opaque colours first, wider blurs for texels further away
                num, den = nfa.blur(grey * opaque, r), nfa.blur(opaque, r)
                fill = todo & (den > 1e-3)
                grey[fill] = num[fill] / den[fill]
                todo &= ~fill
            grey[todo] = grey[~clear].mean()
    return grey


def slopes(pred, flatten_radius=0.25):
    """(sx, sy) of the encoded prediction: n ~ (sx, sy, 1), OpenGL (+y up the image); minus their wide blur
    (radius flatten_radius x the texture size; 0 = off)."""
    n = pred * 2.0 - 1.0
    z = np.maximum(n[2], MIN_Z)
    sx, sy = n[0] / z, n[1] / z
    if flatten_radius > 0:
        r = flatten_radius * max(sx.shape)
        sx = sx - nfa.blur(sx, max(1, int(round(r))))
        sy = sy - nfa.blur(sy, max(1, int(round(r))))
    return sx.astype(np.float32), sy.astype(np.float32)


def encode_normals(sx, sy, alpha=None):
    """H x W x 4 uint8 normal map of slopes (n = normalize(sx, sy, 1)); texels with alpha < 128 flat."""
    n = np.stack([sx, sy, np.ones_like(sx)], axis=-1)
    if alpha is not None:
        n[alpha < 128] = (0.0, 0.0, 1.0)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    out = np.empty(sx.shape + (4,), dtype=np.uint8)
    out[..., :3] = np.clip(np.round(n * 127.5 + 127.5), 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def height_from_slopes(sx, sy):
    """Height (0-1, wrapping) whose normals are (sx, sy, 1): Frankot-Chellappa least squares in the Fourier domain.
    In image axes dz/dcol = -sx and dz/drow = +sy (rows go down, +y goes up)."""
    h, w = sx.shape
    p, q = -sx.astype(np.float64), sy.astype(np.float64)
    wx = 2 * np.pi * np.fft.fftfreq(w)[None, :]
    wy = 2 * np.pi * np.fft.fftfreq(h)[:, None]
    den = wx ** 2 + wy ** 2
    den[0, 0] = 1.0
    zf = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / den
    zf[0, 0] = 0.0
    z = np.real(np.fft.ifft2(zf))
    span = z.max() - z.min()
    return ((z - z.min()) / span if span > 1e-9 else np.full_like(z, 0.5)).astype(np.float32)


def specular_map(rgba, height, cls=None, spec_max=0.35, gloss_min=0.15, gloss_max=0.6):
    """H x W x 4 uint8: R intensity (smooth, bright, unsaturated, raised texels up to spec_max), G gloss (smooth:
    gloss_max, rough: gloss_min), B 0, alpha 255."""
    c = rgba[..., :3].astype(np.float32) / 255.0
    lum = c[..., 0] * 0.299 + c[..., 1] * 0.587 + c[..., 2] * 0.114
    sat = c.max(axis=-1) - c.min(axis=-1)
    size = max(lum.shape)
    r = max(1, int(round(size / 128)))
    mean = nfa.blur(lum, r)
    std = np.sqrt(np.maximum(nfa.blur(lum * lum, r) - mean * mean, 0.0))
    smooth = 1.0 - np.clip(std / 0.065, 0.0, 1.0)
    local = height - nfa.blur(height, max(1, int(round(size / 16))))
    cavity = np.clip(0.6 + local / (2.0 * local.std() + 1e-6), 0.3, 1.0)
    inten = spec_max * (0.4 + 0.6 * lum) * (1.0 - 0.5 * sat) * (0.5 + 0.5 * smooth) * cavity
    inten *= CLASS_SPECULAR.get(cls, 1.0)
    gloss = gloss_min + (gloss_max - gloss_min) * smooth
    if rgba.shape[2] == 4:
        inten[rgba[..., 3] < 128] = 0.0
    out = np.zeros(rgba.shape[:2] + (4,), dtype=np.uint8)
    out[..., 0] = np.clip(np.round(np.minimum(inten, spec_max) * 255), 0, 255).astype(np.uint8)
    out[..., 1] = np.clip(np.round(gloss * 255), 0, 255).astype(np.uint8)
    out[..., 3] = 255
    return out


def relief_maps(rgba, predict, cls=None, opts=None):
    """(normal map, specular map or None) of an RGBA texture; predict is the tile estimator."""
    o = dict(DEFAULTS, **(opts or {}))
    mode = o["padding"] if o["padding"] != "auto" else ("wrap" if cls in TILING_CLASSES else "reflect")
    pred = infer_tiled(network_input(rgba), predict, TILE, o["overlap"], mode)
    sx, sy = slopes(pred, o["flatten_radius"])
    k = o["strength"] * CLASS_STRENGTH.get(cls, 1.0)
    sx, sy = sx * k, sy * k
    alpha = rgba[..., 3] if rgba.shape[2] == 4 else None
    nmap = encode_normals(sx, sy, alpha)
    if not o["specular"]:
        return nmap, None
    spec = specular_map(rgba, height_from_slopes(sx, sy), cls, o["spec_max"], o["gloss_min"], o["gloss_max"])
    return nmap, tu.fit(spec, max(nfa.MIN_SIZE, max(spec.shape[:2]) // 2))


DEFAULTS = {"strength": 2.0, "flatten_radius": 0.25, "overlap": 64, "padding": "auto", "specular": True,
            "spec_max": 0.35, "gloss_min": 0.15, "gloss_max": 0.6}


# ---- pack --------------------------------------------------------------------------------------------------

def read_classes(pack, extra=None):
    """hash -> class from the pack's selection.csv / manifest.csv (+ extra): column class, else category."""
    out = {}
    for path in [os.path.join(pack, "selection.csv"), os.path.join(pack, "manifest.csv")] + ([extra] if extra else []):
        if not os.path.exists(path):
            continue
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                h = (r.get("hash") or "").strip().lower()
                c = (r.get("class") or r.get("category") or "").strip().lower()
                if h and c and h not in out:
                    out[h] = c
    return out


def outputs(h, opts):
    names = ["n"] + (["s"] if opts["specular"] else [])
    exts = ["png"] + (["dds"] if opts["encoder"] else [])
    return [os.path.join(opts["out"], "%s_%s.%s" % (h, s, e)) for s in names for e in exts]


def write_png_atomic(path, img):
    part = path + ".part"
    tu.write_png(part, img)
    os.replace(part, path)


def process(h, src, opts):
    """One texture: "made", "exists", "class", "ui", "small" or "error: ..." (never raises)."""
    try:
        if not opts["force"] and all(os.path.isfile(p) for p in outputs(h, opts)):
            return "exists"
        cls = opts["classes"].get(h)
        if cls in opts["skip_classes"]:
            return "class"
        rgba = tu.read_png(src)
        why = nfa.skip_reason(rgba, cls, h in opts["skip"], opts["ui_heuristic"])
        if why:
            return why
        if opts["max_size"]:
            rgba = tu.fit(rgba, opts["max_size"])
        nmap, spec = relief_maps(rgba, opts["estimator"], cls, opts)
        for suffix, img in [("n", nmap)] + ([("s", spec)] if spec is not None else []):
            write_png_atomic(os.path.join(opts["out"], "%s_%s.png" % (h, suffix)), img)
            if opts["encoder"]:
                nfa.write_dds(os.path.join(opts["out"], "%s_%s.dds" % (h, suffix)), img, opts["encoder"])
        return "made"
    except Exception as e:   # reported per texture; the run goes on
        return "error: %s: %s" % (type(e).__name__, e)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="input PNG pack directory (<hash>.png files)")
    ap.add_argument("--out", required=True, help="output directory for <hash>_n / <hash>_s maps (created; not the pack)")
    ap.add_argument("--hash", action="append", default=[], help="only this texture hash (repeatable)")
    ap.add_argument("--hashes", help="file with the texture hashes to process (one per line, # comments)")
    ap.add_argument("--strength", type=float, default=DEFAULTS["strength"],
                    help="relief strength: scale of the estimated slopes (default %(default)s; class character x 0.35)")
    ap.add_argument("--flatten-radius", type=float, default=DEFAULTS["flatten_radius"],
                    help="removes the slopes' blur of this radius (fraction of the texture size; 0 = off; default %(default)s)")
    ap.add_argument("--max-size", type=int, default=512,
                    help="largest side of the normal maps, halved until it fits (0 = the texture's size; default %(default)s); "
                         "specular maps are half that. The maps share the texture cache with the pack (--texpack-budget)")
    ap.add_argument("--overlap", type=int, default=DEFAULTS["overlap"],
                    help="overlap of the 256 px inference tiles, 16-128 (default %(default)s)")
    ap.add_argument("--padding", choices=("auto", "wrap", "reflect"), default="auto",
                    help="border padding: wrap (tiling), reflect, auto = wrap for class environment_tiling (default)")
    ap.add_argument("--no-specular", dest="specular", action="store_false", help="write no <hash>_s maps")
    ap.add_argument("--spec-max", type=float, default=DEFAULTS["spec_max"], help="highest specular intensity, 0-1 (default %(default)s)")
    ap.add_argument("--gloss-min", type=float, default=DEFAULTS["gloss_min"], help="gloss of rough surfaces, 0-1 (default %(default)s)")
    ap.add_argument("--gloss-max", type=float, default=DEFAULTS["gloss_max"], help="gloss of smooth surfaces, 0-1 (default %(default)s)")
    ap.add_argument("--manifest", help="extra CSV with hash,class (or category) columns (selection.csv / manifest.csv are read too)")
    ap.add_argument("--skip-class", action="append", help="class to skip (repeatable; default: %s)" % ", ".join(SKIP_CLASSES))
    ap.add_argument("--skip-list", help="file with hashes to skip (one per line, # comments)")
    ap.add_argument("--no-ui-heuristic", action="store_true", help="only classes and --skip-list mark UI textures")
    ap.add_argument("--force", action="store_true", help="redo textures whose maps already exist")
    ap.add_argument("--dds", action="store_true", help="also write BC7 .dds maps (needs bc7enc)")
    ap.add_argument("--encoder", help="bc7enc executable for --dds (default: $%s, then bc7enc on PATH)" % tc.ENCODER_ENV)
    ap.add_argument("--model", default=os.environ.get("RELIEF_AI_MODEL", DEFAULT_MODEL),
                    help="DeepBump ONNX model (default: $RELIEF_AI_MODEL, then %(default)s; downloaded when missing)")
    ap.add_argument("--no-download", action="store_true", help="fail instead of downloading a missing model")
    ap.add_argument("--device", choices=("auto", "cuda", "cpu"), default="auto", help="inference device (default %(default)s)")
    ap.add_argument("--threads", type=int, default=4, help="CPU inference threads (default %(default)s)")
    ap.add_argument("--jobs", type=int, default=4, help="textures processed at once (decode, BC7 encode; default %(default)s)")
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    return ap


def main(argv=None, estimator=None):
    args = build_parser().parse_args(argv)
    if not os.path.isdir(args.pack):
        sys.exit("cannot read the pack %s" % args.pack)
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    if os.path.realpath(args.out) == os.path.realpath(args.pack):
        sys.exit("--out must differ from --pack (the maps go into a pack of their own)")
    if args.max_size < 0 or 0 < args.max_size < nfa.MIN_SIZE:
        sys.exit("--max-size: 0 or at least %d" % nfa.MIN_SIZE)
    if not (0 <= args.spec_max <= 1 and 0 <= args.gloss_min <= args.gloss_max <= 1 and args.strength >= 0
            and args.flatten_radius >= 0 and 16 <= args.overlap <= 128 and args.jobs >= 1 and args.threads >= 1):
        sys.exit("out of range: --strength >= 0, --flatten-radius >= 0, --overlap 16-128, --spec-max 0-1, "
                 "0 <= --gloss-min <= --gloss-max <= 1, --jobs and --threads >= 1")
    encoder = None
    if args.dds:
        encoder = tc.find_encoder(args.encoder)
        if not encoder:
            sys.exit("--dds: BC encoder not found (bc7enc; pass --encoder PATH or set %s)" % tc.ENCODER_ENV)
    if estimator is None:
        try:
            estimator = OnnxEstimator(ensure_model(args.model, download=not args.no_download), args.device, args.threads)
        except ImportError:
            sys.exit("onnxruntime missing: pip install onnxruntime-gpu (CUDA) or onnxruntime (CPU)")
        except (OSError, RuntimeError) as e:
            sys.exit("model: %s" % e)
        print("model %s on %s" % (args.model, ", ".join(estimator.providers)))
    tu.PNGRAW = args.pngraw
    os.makedirs(args.out, exist_ok=True)
    names = tc.pack_hashes(args.pack)
    wanted = [h.lower() for h in args.hash] + (nfa.read_hash_list(args.hashes) if args.hashes else [])
    if wanted:
        for h in [h for h in wanted if h not in names]:
            print("  %s: not in the pack" % h, file=sys.stderr)
        names = {h: names[h] for h in dict.fromkeys(wanted) if h in names}
    opts = dict(vars(args), out=args.out, encoder=encoder, estimator=estimator,
                classes=read_classes(args.pack, args.manifest), skip=tc.load_skip_list(args.skip_list),
                ui_heuristic=not args.no_ui_heuristic,
                skip_classes=set(c.lower() for c in (args.skip_class or SKIP_CLASSES)))
    print("relief maps (AI estimate): %d textures from %s into %s" % (len(names), args.pack, args.out), flush=True)
    t0 = time.time()
    counts = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(process, h, src, opts): h for h, src in names.items()}
        for i, fut in enumerate(concurrent.futures.as_completed(futures), 1):
            r = fut.result()
            key = "failed" if r.startswith("error") else r
            counts[key] = counts.get(key, 0) + 1
            if key == "failed":
                print("  %s: %s" % (futures[fut], r), file=sys.stderr, flush=True)
            if i % 50 == 0 or i == len(names):
                el = time.time() - t0
                print("  %d/%d made %d, existing %d (%.0f s, about %.0f s left)" % (
                    i, len(names), counts.get("made", 0), counts.get("exists", 0), el, el / i * (len(names) - i)),
                    flush=True)
    print("done in %.1f s: %d with maps, %d already there, %d skipped by class, %d UI/font skipped, %d too small, "
          "%d failed" % (time.time() - t0, counts.get("made", 0), counts.get("exists", 0), counts.get("class", 0),
                         counts.get("ui", 0), counts.get("small", 0), counts.get("failed", 0)))
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
