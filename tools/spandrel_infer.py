#!/usr/bin/env python3
"""Runs a chain of PyTorch super-resolution models over a directory of PNGs (spandrel backend).

Usage (inside the inference venv, see README "Texture pack v2"):
    ~/tools/upscale-venv/bin/python -I tools/spandrel_infer.py -i IN -o OUT \\
        --model ~/tools/upscale-models/1x_DEDXT.pth --model ~/tools/upscale-models/4x-GameAI-2-0.pth

Every IN/*.png is converted to RGB, passed through the models in order (e.g. a 1x DXT cleanup
model, then a 4x upscaler) and written to OUT with the same name, like realesrgan-ncnn-vulkan
does; tools/texture_upscale.py --backend spandrel drives it. Large images are processed in
overlapping tiles so 6 GB GPUs do not run out of memory.

Model loading never executes code from the downloaded files: .safetensors files are read with
safetensors, .pth files with torch.load(weights_only=True) (tensors only, no arbitrary pickle);
spandrel only builds the matching architecture (ESRGAN, RGT, SPAN, ...) from the state dict.

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
    args = ap.parse_args(argv)
    stages = [load(p, args.device, not args.no_half) for p in args.model]
    names = sorted(n for n in os.listdir(args.indir) if n.lower().endswith(".png"))
    t0 = time.monotonic()
    for n in names:
        out = run_chain(tu.read_png(os.path.join(args.indir, n)), stages, args.tile, args.overlap)
        tu.write_png(os.path.join(args.outdir, n), out)
    print("%d images through %d model(s) in %.1f s" % (len(names), len(stages), time.monotonic() - t0), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
