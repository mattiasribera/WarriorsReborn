#!/usr/bin/env python3
"""Adds material shine to the specular maps of a relief pack (the runtime's --dry-highlights reads it).

The relief pack's <hash>_s maps (tools/relief_from_albedo_ai.py) hold R = intensity and G = gloss guessed from the
colour's fine detail alone, the same for a brick wall, a car or a shop window. This tool asks a local open image-text
model which material each texture shows and writes the answer into the maps' two free channels, keeping R and G
byte for byte (so the game draws exactly as before unless --dry-highlights is on):
  - B = shine: how bright the highlight of the material is (car paint, glass and chrome high, sheet metal, fences
    and signs medium, brick, concrete, asphalt, wood, cloth and paper low; 0 = no material: the runtime keeps R/G);
  - A = gloss: how tight the highlight is (glass and chrome very tight, painted metal medium, matte surfaces broad).

The model is OpenAI CLIP ViT-B/32 (MIT licence; https://github.com/openai/CLIP), as the ONNX export of
huggingface.co/Xenova/clip-vit-base-patch32 at a pinned commit. It is not part of this repository: on first use the
tool downloads the image model, the text model and the tokenizer (about 300 MB, SHA-256 checked) into
~/.cache/relief-ai/models/clip-vit-b32 (or --model-dir). It runs locally through onnxruntime (CUDA when available,
else CPU) and needs the `tokenizers` package. Nothing leaves the machine.

Each colour texture of --pack (the PNG pack the relief maps were made from) is classified as a whole and in a 3 x 3
grid of half-size crops, so an atlas (a car's body, windows and tyres; a facade's bricks and windows) gets a
material per region; each region's material probabilities give its shine and gloss (a weighted mean, so an unsure
answer stays mild), blended smoothly across the texture and scaled per texel by the colour's smoothness (fine
detail such as mortar, rust or tyre tread shines less; transparent texels not at all). Textures of the classes
character, ui_screen, ui_font_hud and fx (selection.csv / manifest.csv, as the relief tool reads them) are left
alone, as are textures without a <hash>_s map in --relief.

The maps are rewritten where they are (--relief) or into --out (a pack of their own: give it to the game before
the relief pack). A map whose .dds sits next to its .png gets a new BC7 .dds too (bc7enc: --encoder or
$TEXPACK_BC_ENCODER), since the game reads the .dds first. Resumable: the results go to material_shine.csv in the
output folder and a hash listed there is skipped (--force redoes it). PNG decoding uses the pngraw helper (--pngraw).
"""
import argparse
import csv
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import normal_from_albedo as nfa  # noqa: E402  (blur, write_dds, read_hash_list)
import relief_from_albedo_ai as rai  # noqa: E402  (ensure_model, read_classes)
import texture_compress as tc  # noqa: E402  (pack_hashes, find_encoder, ENCODER_ENV)
import texture_upscale as tu  # noqa: E402  (read_png, write_png, PNGRAW)

MODEL_COMMIT = "6b7c523703ae26e39be4b03774966f8571c7f523"
MODEL_BASE = "https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/%s/" % MODEL_COMMIT
MODEL_FILES = {   # local name: (path in the repository, SHA-256)
    "vision_model_fp16.onnx": ("onnx/vision_model_fp16.onnx", "35c4e0fb0aeee527dcde1693520b214a34424a786babd530f35366bad5844efd"),
    "text_model_fp16.onnx": ("onnx/text_model_fp16.onnx", "df587ffbf248bf20d44fa6e16adc5ebc27ead691860e5333dbdaab5fd6bf3f6e"),
    "tokenizer.json": ("tokenizer.json", "f7f3b7af117d467b58374797691a6438d3e6b9e9cef800dfd5dced7f697a90cd"),
}
DEFAULT_MODEL_DIR = os.path.join(os.path.expanduser("~"), ".cache", "relief-ai", "models", "clip-vit-b32")
SKIP_CLASSES = ("character", "ui_screen", "ui_font_hud", "fx")
CSV_NAME = "material_shine.csv"

# material: (shine, gloss, prompts). Shine is B / 255 (the runtime's highlight intensity is twice it), gloss A / 255
# (the runtime's Blinn-Phong exponent is 2^(3 + 7 gloss): 8 to 1024). The last group catches what is not a lit
# surface (sky, neon tubes, fire) and gives it no shine.
MATERIALS = {
    "car": (0.80, 0.85, ["the painted body of a car", "a car door with a window", "the hood of a car", "a car with wheels"]),
    "glass": (0.90, 0.95, ["a glass window", "a shop window", "glass panes in a window frame", "a dirty glass window"]),
    "chrome": (1.00, 0.90, ["shiny chrome metal", "a chrome car bumper", "polished steel"]),
    "metal": (0.50, 0.60, ["corrugated metal sheet", "a metal roll-up shutter", "painted sheet metal", "a steel door",
                           "a metal dumpster", "metal pipes"]),
    "fence": (0.55, 0.65, ["a chain-link fence", "a metal railing", "a metal grate"]),
    "sign": (0.35, 0.55, ["a painted metal shop sign with letters", "a street sign"]),
    "brick": (0.05, 0.25, ["a brick wall", "a stone wall"]),
    "concrete": (0.07, 0.30, ["a concrete wall", "a concrete floor", "a plaster wall"]),
    "asphalt": (0.08, 0.35, ["an asphalt road", "a cracked sidewalk"]),
    "dirt": (0.03, 0.15, ["dirt ground", "grass", "garbage and rubbish"]),
    "wood": (0.08, 0.30, ["wooden planks", "a wooden crate", "a wooden door"]),
    "fabric": (0.04, 0.20, ["cloth fabric", "an old sofa", "a mattress"]),
    "paper": (0.05, 0.20, ["a paper poster on a wall", "graffiti on a wall", "cardboard"]),
    "none": (0.00, 0.00, ["the night sky", "a glowing neon sign", "fire and flames"]),
}
NAMES = list(MATERIALS)
SHINE = np.array([MATERIALS[n][0] for n in NAMES], dtype=np.float32)
GLOSS = np.array([MATERIALS[n][1] for n in NAMES], dtype=np.float32)
GLOBAL_WEIGHT = 0.4   # a crop's material: 0.4 x the whole texture's + 0.6 x its own
GRID = 3              # crops per side (half the texture each, centred at 1/4, 1/2, 3/4)
MIN_GRID_SIZE = 64    # smaller textures are classified as a whole only
CLIP_SIZE = 224
CLIP_MEAN = np.array([0.48145466, 0.4578275, 0.40821073], dtype=np.float32)
CLIP_STD = np.array([0.26862954, 0.26130258, 0.27577711], dtype=np.float32)


# ---- model -------------------------------------------------------------------------------------------------

def ensure_models(model_dir, download=True):
    for name, (path, sha) in MODEL_FILES.items():
        rai.ensure_model(os.path.join(model_dir, name), MODEL_BASE + path, sha, download)
    return model_dir


def resize(img, h, w):
    """H x W x C float image resampled to h x w: box filter when shrinking by an integer factor, else bilinear."""
    ih, iw = img.shape[:2]
    if ih % h == 0 and iw % w == 0:
        return img.reshape(h, ih // h, w, iw // w, -1).mean(axis=(1, 3))
    y = np.clip((np.arange(h) + 0.5) * ih / h - 0.5, 0, ih - 1)
    x = np.clip((np.arange(w) + 0.5) * iw / w - 0.5, 0, iw - 1)
    y0, x0 = np.floor(y).astype(int), np.floor(x).astype(int)
    y1, x1 = np.minimum(y0 + 1, ih - 1), np.minimum(x0 + 1, iw - 1)
    fy, fx = (y - y0)[:, None, None], (x - x0)[None, :, None]
    top = img[y0][:, x0] * (1 - fx) + img[y0][:, x1] * fx
    bot = img[y1][:, x0] * (1 - fx) + img[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


class ClipClassifier:
    """Callable: list of H x W x 3 float RGB images (0-1) -> N x len(NAMES) material probabilities."""

    def __init__(self, model_dir, device="auto", threads=4):
        import onnxruntime as ort
        from tokenizers import Tokenizer
        for name in ("preload_dlls", "disable_telemetry_events"):
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
                "auto": [p for p in ("CUDAExecutionProvider",) if p in avail]}[device] + ["CPUExecutionProvider"]
        self.vision = ort.InferenceSession(os.path.join(model_dir, "vision_model_fp16.onnx"), so, providers=want)
        text = ort.InferenceSession(os.path.join(model_dir, "text_model_fp16.onnx"), so, providers=want)
        self.providers = self.vision.get_providers()
        tok = Tokenizer.from_file(os.path.join(model_dir, "tokenizer.json"))
        prompts = [(k, "a video game texture of " + p) for k, n in enumerate(NAMES) for p in MATERIALS[n][2]]
        ids = [tok.encode(p).ids for _, p in prompts]
        pad = tok.token_to_id("<|endoftext|>")
        width = max(map(len, ids))
        emb = text.run(None, {"input_ids": np.array([i + [pad] * (width - len(i)) for i in ids], dtype=np.int64)})[0]
        emb /= np.linalg.norm(emb, axis=1, keepdims=True)
        cls = np.stack([emb[[i for i, (k, _) in enumerate(prompts) if k == c]].mean(axis=0) for c in range(len(NAMES))])
        self.text = cls / np.linalg.norm(cls, axis=1, keepdims=True)   # prompt ensembles, one row per material

    def __call__(self, images):
        x = np.stack([(resize(im, CLIP_SIZE, CLIP_SIZE) - CLIP_MEAN) / CLIP_STD for im in images])
        e = self.vision.run(None, {"pixel_values": x.transpose(0, 3, 1, 2).astype(np.float32)})[0]
        e /= np.linalg.norm(e, axis=1, keepdims=True)
        logits = 100.0 * e @ self.text.T
        p = np.exp(logits - logits.max(axis=1, keepdims=True))
        return p / p.sum(axis=1, keepdims=True)


# ---- material maps -----------------------------------------------------------------------------------------

def crops(rgb):
    """The images to classify: the whole texture, then (when it is at least MIN_GRID_SIZE) GRID x GRID half-size
    crops in row-major order."""
    out = [rgb]
    h, w = rgb.shape[:2]
    if min(h, w) >= MIN_GRID_SIZE:
        ch, cw = h // 2, w // 2
        for gy in range(GRID):
            for gx in range(GRID):
                y, x = gy * (h - ch) // (GRID - 1), gx * (w - cw) // (GRID - 1)
                out.append(rgb[y:y + ch, x:x + cw])
    return out


def region_probs(probs):
    """GRID x GRID x K probabilities of the crops (blended with the whole texture's), or 1 x 1 x K."""
    if len(probs) == 1:
        return probs[:1].reshape(1, 1, -1)
    return (GLOBAL_WEIGHT * probs[0] + (1 - GLOBAL_WEIGHT) * probs[1:]).reshape(GRID, GRID, -1)


def smoothness(rgb):
    """H x W in 0-1: 1 where the colour has no fine detail (paint, glass), 0 for busy texels (mortar, rust, tread)."""
    lum = rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114
    r = max(1, int(round(max(lum.shape) / 128)))
    mean = nfa.blur(lum, r)
    std = np.sqrt(np.maximum(nfa.blur(lum * lum, r) - mean * mean, 0.0))
    return 1.0 - np.clip(std / 0.065, 0.0, 1.0)


def shine_channels(rgba, regions):
    """(B, A) uint8 H x W of an RGBA uint8 texture (already at the specular map's size) and its region
    probabilities: each region's expected shine and gloss, bilinear between region centres, times the texel's
    smoothness; transparent texels (alpha < 128) 0."""
    h, w = rgba.shape[:2]
    field = np.stack([regions @ SHINE, regions @ GLOSS], axis=-1).astype(np.float32)
    field = resize(field, h, w) if field.shape[0] > 1 else np.broadcast_to(field, (h, w, 2))
    rgb = rgba[..., :3].astype(np.float32) / 255.0
    smooth = smoothness(rgb)
    shine = field[..., 0] * (0.3 + 0.7 * smooth)
    gloss = field[..., 1] * (0.75 + 0.25 * smooth)
    if rgba.shape[2] == 4:
        shine = np.where(rgba[..., 3] < 128, 0.0, shine)
    return (np.clip(np.round(shine * 255), 0, 255).astype(np.uint8), np.clip(np.round(gloss * 255), 0, 255).astype(np.uint8))


def shine_map(spec, rgba, classify):
    """The specular map spec (H x W x 4 uint8) with B / A from the colour texture rgba (any size) and the
    classifier; R and G untouched. Returns (map, region probabilities)."""
    h, w = spec.shape[:2]
    colour = rgba if rgba.shape[2] == 4 else np.concatenate([rgba, np.full(rgba.shape[:2] + (1,), 255, np.uint8)], axis=-1)
    rgb = colour[..., :3].astype(np.float32) / 255.0
    regions = region_probs(np.asarray(classify(crops(rgb)), dtype=np.float32))
    small = np.clip(np.round(resize(colour.astype(np.float32), h, w)), 0, 255).astype(np.uint8)
    out = spec.copy()
    out[..., 2], out[..., 3] = shine_channels(small, regions)
    return out, regions


# ---- pack --------------------------------------------------------------------------------------------------

def read_done(path):
    if not os.path.isfile(path):
        return {}
    with open(path, newline="") as f:
        return {r["hash"]: r for r in csv.DictReader(f)}


def process(h, src, opts):
    """One texture: (status, csv row or None); status "made", "class", "no map" or "error: ..." (never raises)."""
    try:
        if opts["classes"].get(h) in opts["skip_classes"]:
            return "class", None
        spec_png = os.path.join(opts["relief"], h + "_s.png")
        if not os.path.isfile(spec_png):
            return "no map", None
        spec = tu.read_png(spec_png)
        dds_src = os.path.join(opts["relief"], h + "_s.dds")
        if os.path.isfile(dds_src) and not opts["encoder"]:
            return "error: %s has a .dds and no BC7 encoder was found (--encoder or $%s)" % (h, tc.ENCODER_ENV), None
        img, regions = shine_map(spec, tu.read_png(src), opts["classify"])
        out_png = os.path.join(opts["out"], h + "_s.png")
        part = out_png + ".part"
        tu.write_png(part, img)
        os.replace(part, out_png)
        if os.path.isfile(dds_src):
            nfa.write_dds(os.path.join(opts["out"], h + "_s.dds"), img, opts["encoder"])
        p = regions.reshape(-1, len(NAMES)).mean(axis=0)
        top = int(p.argmax())
        return "made", {"hash": h, "material": NAMES[top], "p": "%.2f" % p[top],
                        "shine": "%.3f" % (img[..., 2].mean() / 255.0), "gloss": "%.3f" % (img[..., 3].mean() / 255.0)}
    except Exception as e:   # reported per texture; the run goes on
        return "error: %s: %s" % (type(e).__name__, e), None


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="the colour PNG pack the relief maps were made from (<hash>.png)")
    ap.add_argument("--relief", required=True, help="the relief pack (<hash>_s.png maps, made by relief_from_albedo_ai.py)")
    ap.add_argument("--out", help="where the updated maps go (default: --relief itself, in place)")
    ap.add_argument("--hash", action="append", default=[], help="only this texture hash (repeatable)")
    ap.add_argument("--hashes", help="file with the texture hashes to process (one per line, # comments)")
    ap.add_argument("--manifest", help="extra CSV with hash,class (or category) columns (selection.csv / manifest.csv are read too)")
    ap.add_argument("--force", action="store_true", help="redo textures already listed in %s" % CSV_NAME)
    ap.add_argument("--encoder", help="bc7enc executable for maps with a .dds (default: $%s, then bc7enc on PATH)" % tc.ENCODER_ENV)
    ap.add_argument("--model-dir", default=os.environ.get("MATERIAL_SHINE_MODEL_DIR", DEFAULT_MODEL_DIR),
                    help="CLIP ONNX files (default: $MATERIAL_SHINE_MODEL_DIR, then %(default)s; downloaded when missing)")
    ap.add_argument("--no-download", action="store_true", help="fail instead of downloading a missing model")
    ap.add_argument("--device", choices=("auto", "cuda", "cpu"), default="auto", help="inference device (default %(default)s)")
    ap.add_argument("--threads", type=int, default=4, help="CPU inference threads (default %(default)s)")
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    return ap


def main(argv=None, classify=None):
    args = build_parser().parse_args(argv)
    out = args.out or args.relief
    for d, what in ((args.pack, "pack"), (args.relief, "relief pack")):
        if not os.path.isdir(d):
            sys.exit("cannot read the %s %s" % (what, d))
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    if os.path.realpath(out) == os.path.realpath(args.pack):
        sys.exit("--out must not be the colour pack")
    encoder = tc.find_encoder(args.encoder)
    if classify is None:
        try:
            classify = ClipClassifier(ensure_models(args.model_dir, download=not args.no_download), args.device, args.threads)
        except ImportError as e:
            sys.exit("missing Python package (%s): pip install onnxruntime-gpu (or onnxruntime) tokenizers" % e)
        except (OSError, RuntimeError) as e:
            sys.exit("model: %s" % e)
        print("model %s on %s" % (args.model_dir, ", ".join(classify.providers)))
    tu.PNGRAW = args.pngraw
    os.makedirs(out, exist_ok=True)
    names = tc.pack_hashes(args.pack)
    wanted = [h.lower() for h in args.hash] + (nfa.read_hash_list(args.hashes) if args.hashes else [])
    if wanted:
        for h in [h for h in wanted if h not in names]:
            print("  %s: not in the pack" % h, file=sys.stderr)
        names = {h: names[h] for h in dict.fromkeys(wanted) if h in names}
    csv_path = os.path.join(out, CSV_NAME)
    done = {} if args.force else read_done(csv_path)
    rows = read_done(csv_path)
    opts = {"relief": args.relief, "out": out, "encoder": encoder, "classify": classify,
            "classes": rai.read_classes(args.pack, args.manifest), "skip_classes": set(SKIP_CLASSES)}
    todo = {h: s for h, s in names.items() if h not in done}
    print("material shine: %d textures (%d already done) from %s, maps %s -> %s" % (len(todo), len(names) - len(todo),
                                                                                    args.pack, args.relief, out), flush=True)
    t0 = time.time()
    counts = {}
    for i, (h, src) in enumerate(todo.items(), 1):
        status, row = process(h, src, opts)
        key = "failed" if status.startswith("error") else status
        counts[key] = counts.get(key, 0) + 1
        if row:
            rows[h] = row
        if key == "failed":
            print("  %s: %s" % (h, status), file=sys.stderr, flush=True)
        if i % 100 == 0 or i == len(todo):
            el = time.time() - t0
            print("  %d/%d made %d (%.0f s, about %.0f s left)" % (i, len(todo), counts.get("made", 0), el, el / i * (len(todo) - i)),
                  flush=True)
            part = csv_path + ".part"
            with open(part, "w", newline="") as f:
                wr = csv.DictWriter(f, ["hash", "material", "p", "shine", "gloss"])
                wr.writeheader()
                wr.writerows(rows[k] for k in sorted(rows))
            os.replace(part, csv_path)
    print("done in %.1f s: %d maps with material, %d skipped by class, %d without a specular map, %d failed" % (
        time.time() - t0, counts.get("made", 0), counts.get("class", 0), counts.get("no map", 0), counts.get("failed", 0)))
    return 1 if counts.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
