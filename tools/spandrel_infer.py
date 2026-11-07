#!/usr/bin/env python3
"""Runs a chain of PyTorch super-resolution models over a directory of PNGs (spandrel backend).

Usage (inside the inference venv, see README "Texture pack v2"):
    ~/tools/upscale-venv/bin/python -I tools/spandrel_infer.py -i IN -o OUT \\
        --model ~/tools/upscale-models/1x_DEDXT.pth --model ~/tools/upscale-models/4x-GameAI-2-0.pth

Every IN/*.png is converted to RGB, passed through the models in order (e.g. a 1x DXT cleanup
model, then a 4x upscaler) and written to OUT with the same name, like realesrgan-ncnn-vulkan
does; tools/texture_upscale.py --backend spandrel drives it. Large images are processed in
overlapping tiles so 6 GB GPUs do not run out of memory; when a tile still runs out of GPU memory
the image is retried with half the tile size (down to 8). An image that fails otherwise is
reported ("failed: NAME: error") and gets no output; the other images go on. --skip-existing
leaves images whose output already exists alone (resuming an interrupted run).

Model loading never executes code from the downloaded files: .safetensors files are read with
safetensors, .pth files with torch.load(weights_only=True) (tensors only, no arbitrary pickle);
spandrel only builds the matching architecture (ESRGAN, RGT, SPAN, ...) from the state dict.

PNGs are decoded with tools/texture_upscale.py's pngraw helper (pngraw.exe on Windows, next to
the tools or in runtime/build), or the decoder given with --pngraw.

torch and spandrel are imported lazily, so this module (tile_apply, run_chain) is importable and
testable on a Python without them.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # texture_upscale (also under -I)

import numpy as np  # noqa: E402

import texture_upscale as tu  # noqa: E402


def tile_apply(img, fn, scale, tile=256, overlap=16):
    """fn(H x W x C float32) -> (H*scale) x (W*scale) x C, applied in tile x tile pieces that
    see `overlap` extra source texels of context on each inner side (only the centers are kept)."""
    h, w = img.shape[:2]
    if h <= tile and w <= tile:
        return fn(img)
    out = None
    for y in range(0, h, tile):
        for x in range(0, w, tile):
            y0, x0 = max(y - overlap, 0), max(x - overlap, 0)
            y1, x1 = min(y + tile + overlap, h), min(x + tile + overlap, w)
            ty1, tx1 = min(y + tile, h), min(x + tile, w)
            r = fn(img[y0:y1, x0:x1])
            if out is None:
                out = np.zeros((h * scale, w * scale, r.shape[2]), dtype=np.float32)
            out[y * scale:ty1 * scale, x * scale:tx1 * scale] = \
                r[(y - y0) * scale:(ty1 - y0) * scale, (x - x0) * scale:(tx1 - x0) * scale]
    return out


def run_chain(img, stages, tile=256, overlap=16):
    """img: H x W x (3|4) uint8 (alpha ignored); stages: [(fn, scale)]. Returns RGB uint8."""
    x = img[..., :3].astype(np.float32) / 255.0
    for fn, scale in stages:
        x = np.clip(tile_apply(x, fn, scale, tile, overlap), 0.0, 1.0)
    return (x * 255.0 + 0.5).astype(np.uint8)


def is_oom(exc):
    """True for a CUDA out-of-memory error (torch.cuda.OutOfMemoryError is a RuntimeError)."""
    return isinstance(exc, RuntimeError) and "out of memory" in str(exc)


def run_chain_adaptive(img, stages, tile, overlap, min_tile=8, on_oom=None):
    """run_chain, retried with half the tile size while the GPU runs out of memory. Returns
    (RGB uint8, tile size that worked)."""
    while True:
        try:
            return run_chain(img, stages, tile, overlap), tile
        except RuntimeError as e:
            if not is_oom(e) or tile // 2 < min(min_tile, tile):
                raise
            if on_oom:
                on_oom()
            tile //= 2


def load_model(path, device="cuda", half=True):
    """Returns (fn, scale) for a model file, loading weights only (see module doc)."""
    import spandrel
    import torch

    if path.lower().endswith(".safetensors"):
        from safetensors.torch import load_file
        state = load_file(path)
    else:
        state = torch.load(path, map_location="cpu", weights_only=True)
    desc = spandrel.ModelLoader().load_from_state_dict(spandrel.canonicalize_state_dict(state))
    if not isinstance(desc, spandrel.ImageModelDescriptor):
        sys.exit("%s: not a single-image model (%s)" % (path, type(desc).__name__))
    desc.to(device).eval()
    use_half = half and device.startswith("cuda") and desc.supports_half
    if use_half:
        desc.half()
    print("model %s: %s %dx, %s%s" % (os.path.basename(path), desc.architecture.name, desc.scale, device,
                                       " fp16" if use_half else ""), flush=True)

    def fn(img):
        t = torch.from_numpy(np.ascontiguousarray(img.transpose(2, 0, 1)))[None].to(device)
        if use_half:
            t = t.half()
        with torch.inference_mode():
            o = desc(t)
        return o[0].float().clamp(0, 1).permute(1, 2, 0).cpu().numpy()
    return fn, desc.scale


def main(argv=None, load=load_model):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-i", dest="indir", required=True, help="input directory of PNGs")
    ap.add_argument("-o", dest="outdir", required=True, help="output directory")
    ap.add_argument("--model", action="append", required=True, help="model file (.safetensors/.pth); repeat to chain")
    ap.add_argument("--tile", type=int, default=256, help="tile size in input texels per stage")
    ap.add_argument("--overlap", type=int, default=16, help="tile context in input texels")
    ap.add_argument("--device", default="cuda", help="torch device (cuda, cuda:1, cpu)")
    ap.add_argument("--no-half", action="store_true", help="run in fp32 even when the model supports fp16")
    ap.add_argument("--skip-existing", action="store_true", help="leave images whose output exists alone")
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    args = ap.parse_args(argv)
    if not os.path.isfile(args.pngraw):
        sys.exit(tu.missing_pngraw(args.pngraw))
    tu.PNGRAW = os.path.abspath(args.pngraw)
    names = sorted(n for n in os.listdir(args.indir) if n.lower().endswith(".png"))
    if args.skip_existing:
        names = [n for n in names if not os.path.exists(os.path.join(args.outdir, n))]
    if not names:
        print("0 images to process", flush=True)
        return 0
    stages = [load(p, args.device, not args.no_half) for p in args.model]

    def free_gpu():
        if "torch" in sys.modules:
            sys.modules["torch"].cuda.empty_cache()
    t0, failed, tile = time.monotonic(), 0, args.tile
    for i, n in enumerate(names, 1):
        try:
            out, used = run_chain_adaptive(tu.read_png(os.path.join(args.indir, n)), stages, tile, args.overlap,
                                           on_oom=free_gpu)
        except Exception as e:   # noqa: BLE001  one bad texture must not stop the batch
            free_gpu()
            failed += 1
            print("failed: %s: %s" % (n, str(e).splitlines()[0] if str(e) else type(e).__name__), flush=True)
            continue
        if used != tile:
            print("tile %d -> %d (out of GPU memory at %s)" % (tile, used, n), flush=True)
        tu.write_png(os.path.join(args.outdir, n) + ".tmp", out)
        os.replace(os.path.join(args.outdir, n) + ".tmp", os.path.join(args.outdir, n))
        if i % 50 == 0:
            print("  %d/%d (%.0f s)" % (i, len(names), time.monotonic() - t0), flush=True)
    print("%d images through %d model(s) in %.1f s, %d failed" % (
        len(names), len(stages), time.monotonic() - t0, failed), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
