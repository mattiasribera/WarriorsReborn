#!/usr/bin/env python3
"""Sharp, non-generative upscale of flat UI art (maps, menus, posters) with re-typeset labels.

An AI upscaler blurs flat map art and turns small lettering into made-up glyphs. This tool
redraws the art from the original pixels only, so nothing is invented:

  1. Colour classes: k-means on the region's colours gives K flat classes (water, paper, park,
     each line colour, ink, ...). Every texel gets its nearest class.
  2. Clean edges: each class's one-hot mask is upscaled with Lanczos-3 and the classes are
     re-split with a steep softmax, so a class boundary becomes a smooth anti-aliased curve at
     the new size (like tracing the shapes and rendering them again) instead of a blurred step.
  3. Colour and detail: each class keeps its local colour (a normalized Gaussian average of
     its own texels, so a faded or shaded part of a fill stays faded) and the texel detail
     inside a region (paper grain, folds) is added back from a Lanczos upscale of the residual;
     near boundaries the residual is faded out so it does not bring the blur back.
  4. Labels: each label box is found in the original (its dark ink texels, Otsu threshold
     inside the box), the old lettering is removed by harmonic inpainting of the texels under
     it, and the label is typeset again with a TrueType font, stretched to the old lettering's
     exact ink box, in the old ink colour (the median of its darkest texels).

Outside the --region boxes the image is a plain Lanczos-3 upscale (premultiplied by alpha), so
photographic borders and transparent UI markers keep their look; alpha is never redrawn.

  ui_redraw.py --spec spec.json SRC.png OUT.png

spec.json (all coordinates are texels of SRC, boxes are x0,y0,x1,y1 with x1/y1 exclusive):

  {"scale": 4,
   "regions": [[52, 22, 460, 378]],          // redrawn areas (default: the whole image)
   "classes": 14,                            // k-means classes per region
   "font": "fonts/Arvo-Bold.ttf",            // default font, relative to the spec file
   "labels": [{"text": "MANHATTAN", "box": [62, 138, 150, 156]},
              {"text": "How To Use This Map", "box": [312, 334, 378, 346],
               "font": "...", "color": [40, 40, 40], "halo": 0.0}]}

A spec names texels of a game texture: keep it with your own dumps, never publish it.
Requires numpy and Pillow (the TrueType renderer and PNG I/O).
"""
import argparse
import json
import os
import sys

import numpy as np

LANCZOS_A = 3


# ---------------------------------------------------------------- resampling

def lanczos_matrix(n_in, scale, a=LANCZOS_A):
    """(n_in*scale, n_in) weights of a Lanczos-a upscale by an integer factor, edges clamped."""
    n_out = n_in * scale
    u = (np.arange(n_out) + 0.5) / scale - 0.5
    base = np.floor(u).astype(int)
    m = np.zeros((n_out, n_in), np.float64)
    for t in range(-a + 1, a + 1):
        j = base + t
        x = u - j
        w = np.sinc(x) * np.sinc(x / a)
        w[np.abs(x) >= a] = 0.0
        np.add.at(m, (np.arange(n_out), np.clip(j, 0, n_in - 1)), w)
    m /= m.sum(axis=1, keepdims=True)
    return m


def upscale(img, scale):
    """Lanczos-3 upscale of an (H, W[, C]) float array by an integer factor."""
    h, w = img.shape[:2]
    my, mx = lanczos_matrix(h, scale), lanczos_matrix(w, scale)
    if img.ndim == 2:
        return my @ img @ mx.T
    return np.stack([my @ img[..., c] @ mx.T for c in range(img.shape[2])], axis=-1)


def gauss_blur(img, sigma):
    """Separable Gaussian blur with edge clamping, (H, W[, C]) float."""
    r = max(1, int(3 * sigma + 0.5))
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2)
    k /= k.sum()
    out = img.astype(np.float64)
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode="edge")
        acc = np.zeros_like(out)
        for i, kv in enumerate(k):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += kv * p[tuple(sl)]
        out = acc
    return out


# ---------------------------------------------------------------- colour classes

def kmeans(pixels, k, iters=30, seed=1):
    """Deterministic k-means++ on (N, 3) floats; returns (k, 3) centres sorted by luminance."""
    rng = np.random.default_rng(seed)
    pts = pixels.astype(np.float64)
    if len(pts) > 60000:
        pts = pts[rng.choice(len(pts), 60000, replace=False)]
    k = min(k, len(np.unique(pts, axis=0)))
    centers = [pts[rng.integers(len(pts))]]
    for _ in range(1, k):
        d = np.min(((pts[:, None, :] - np.array(centers)[None]) ** 2).sum(-1), axis=1)
        if d.sum() <= 0:
            break
        centers.append(pts[rng.choice(len(pts), p=d / d.sum())])
    c = np.array(centers)
    for _ in range(iters):
        lab = assign(pts, c)
        new = np.array([pts[lab == i].mean(0) if np.any(lab == i) else c[i] for i in range(len(c))])
        if np.allclose(new, c):
            break
        c = new
    return c[np.argsort(c @ np.array([0.299, 0.587, 0.114]))]


def assign(pixels, centers):
    """Index of the nearest centre for each (…, 3) pixel."""
    d = ((pixels[..., None, :] - centers) ** 2).sum(-1)
    return np.argmin(d, axis=-1)


def mix_fit(pixels, ca, cb):
    """Best blend alpha*ca + (1-alpha)*cb for each (N, 3) pixel: (alpha, squared error)."""
    d = ca - cb
    dd = float(d @ d)
    if dd <= 0:
        return np.ones(len(pixels)), ((pixels - ca) ** 2).sum(-1)
    a = np.clip(((pixels - cb) @ d) / dd, 0.0, 1.0)
    err = ((pixels - (a[:, None] * ca + (1 - a[:, None]) * cb)) ** 2).sum(-1)
    return a, err


def prune_mixtures(centers, tol=12.0):
    """Drop centres that are an edge blend of two others (anti-aliasing between two flat colours
    makes k-means spend classes on in-between colours): a centre within tol (RGB distance) of
    the segment between two other centres, at 15-85% of the way, goes."""
    keep = list(range(len(centers)))
    changed = True
    while changed and len(keep) > 2:
        changed = False
        for i in keep:
            others = [j for j in keep if j != i]
            best = None
            for x in range(len(others)):
                for y in range(x + 1, len(others)):
                    a, err = mix_fit(centers[i][None], centers[others[x]], centers[others[y]])
                    if 0.15 <= a[0] <= 0.85 and err[0] <= tol * tol:
                        best = err[0] if best is None else min(best, err[0])
            if best is not None:
                keep.remove(i)
                changed = True
                break
    return centers[keep]


def coverage(rgb, centers, mix_bias=30.0):
    """Fractional class coverage (H, W, K) of each texel, modelling a texel as one class or an
    anti-aliased blend of two: blends are taken only when they fit better than the nearest
    pure class by mix_bias (squared RGB distance), so flat fills stay pure."""
    h, w, _ = rgb.shape
    px = rgb.reshape(-1, 3).astype(np.float64)
    k = len(centers)
    d = ((px[:, None, :] - centers[None]) ** 2).sum(-1)
    best_err = d.min(1)
    cov = np.zeros((len(px), k))
    cov[np.arange(len(px)), d.argmin(1)] = 1.0
    best_mix = best_err - mix_bias
    for a in range(k):
        for b in range(a + 1, k):
            alpha, err = mix_fit(px, centers[a], centers[b])
            better = err < best_mix
            if better.any():
                best_mix = np.where(better, err, best_mix)
                cov[better] = 0.0
                cov[better, a] = alpha[better]
                cov[better, b] = 1.0 - alpha[better]
    return cov.reshape(h, w, k)


def harmonic_fill(img, mask, iters=400):
    """Fill the texels where mask is True from their neighbours (Laplace equation, Jacobi)."""
    out = img.astype(np.float64).copy()
    if not mask.any():
        return out
    known = ~mask
    # start from a wide blur of the known texels so the iteration converges quickly
    w = gauss_blur(known.astype(np.float64), 6.0)
    seed = gauss_blur(np.where(known[..., None] if out.ndim == 3 else known, out, 0.0), 6.0)
    seed = seed / np.maximum(w[..., None] if out.ndim == 3 else w, 1e-6)
    out[mask] = seed[mask]
    for _ in range(iters):
        p = np.pad(out, [(1, 1), (1, 1)] + [(0, 0)] * (out.ndim - 2), mode="edge")
        avg = 0.25 * (p[:-2, 1:-1] + p[2:, 1:-1] + p[1:-1, :-2] + p[1:-1, 2:])
        out[mask] = avg[mask]
    return out


def max_filter3(img):
    """3x3 maximum filter of an (H, W[, C]) array, edges clamped."""
    p = np.pad(img, [(1, 1), (1, 1)] + [(0, 0)] * (img.ndim - 2), mode="edge")
    h, w = img.shape[:2]
    return np.max([p[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)], axis=0)


def redraw_flat(rgb, scale, classes=14, sharpness=24.0, color_sigma=2.0, shape_sigma=0.7,
                mix_tol=0.0, ink_detail=1.0, exclude=None):
    """Sharp upscale of flat art: (H, W, 3) floats 0-255 -> (H*scale, W*scale, 3).

    exclude: optional (H, W) bool mask of texels that are not art (removed lettering); they
    take no part in the classes or colours and are redrawn from their surroundings."""
    h, w, _ = rgb.shape
    valid = np.ones((h, w), bool) if exclude is None else ~exclude
    centers = kmeans(rgb[valid].reshape(-1, 3), classes)
    if mix_tol > 0:
        centers = prune_mixtures(centers, mix_tol)
    k = len(centers)
    onehot = coverage(rgb, centers)
    if exclude is not None and exclude.any():
        onehot = harmonic_fill(onehot, exclude)
        rgb = harmonic_fill(rgb, exclude)
    # local colour of each class: normalized Gaussian average of its own texels
    local = np.zeros((h, w, k, 3))
    for i in range(k):
        m = onehot[..., i] ** 2 * valid   # mostly-pure texels set the colour
        num = gauss_blur(rgb * m[..., None], color_sigma)
        den = gauss_blur(m, color_sigma)[..., None]
        local[..., i, :] = np.where(den > 1e-3, num / np.maximum(den, 1e-6), centers[i])
    approx = (onehot[..., None] * local).sum(2)
    residual = rgb - approx
    # interior confidence: 1 deep inside a class, 0 at a boundary
    conf = gauss_blur(onehot, 0.8).max(-1)
    conf = np.clip((conf - 0.75) / 0.2, 0.0, 1.0)

    # shapes: a slightly blurred class mask rounds the texel staircase into smooth curves; dividing
    # by its local peak (at least 0.5) keeps 1-texel lines and dots at full strength while a lone
    # stray texel stays weak and is absorbed by its surroundings
    shape = gauss_blur(onehot, shape_sigma) if shape_sigma > 0 else onehot
    shape = shape / np.maximum(max_filter3(shape), 0.5)
    # one class at a time keeps memory at K full-size planes
    z = np.stack([(sharpness * upscale(shape[..., i], scale)).astype(np.float32) for i in range(k)])
    z -= z.max(0, keepdims=True)
    np.exp(z, out=z)
    z /= z.sum(0, keepdims=True)
    base = np.zeros((h * scale, w * scale, 3))
    for i in range(k):
        base += z[i][..., None] * upscale(local[..., i, :], scale)
    detail = upscale(residual * conf[..., None], scale)
    out = base + detail
    if ink_detail > 0:
        # fine dark grey linework (streets, tiny lettering) thinner than a class shape: where a
        # sharpened Lanczos upscale is a darker, near-grey stroke than the redraw, take it back
        lan = upscale(rgb, scale)
        lan = lan + 0.8 * (lan - gauss_blur(lan, 0.6 * scale))
        lum_l, lum_o = luminance(lan), luminance(out)
        chroma = lan.max(-1) - lan.min(-1)
        w = np.clip((lum_o - lum_l - 6.0) / 24.0, 0, 1) * np.clip((45.0 - chroma) / 20.0, 0, 1)
        w = w * np.clip((150.0 - lum_l) / 40.0, 0, 1) * ink_detail
        out = out * (1 - w[..., None]) + lan * w[..., None]
    return np.clip(out, 0.0, 255.0)


# ---------------------------------------------------------------- labels

def otsu(values):
    """Otsu threshold of a 1-D array of 0-255 values (-inf when they cannot be split: one level)."""
    hist = np.bincount(np.clip(values.astype(int), 0, 255), minlength=256).astype(np.float64)
    total = hist.sum()
    if np.count_nonzero(hist) < 2:
        return -np.inf
    cum = np.cumsum(hist)
    cmean = np.cumsum(hist * np.arange(256))
    best, thr = -1.0, 128.0
    for t in range(1, 256):
        w0, w1 = cum[t - 1], total - cum[t - 1]
        if w0 == 0 or w1 == 0:
            continue
        m0 = cmean[t - 1] / w0
        m1 = (cmean[-1] - cmean[t - 1]) / w1
        v = w0 * w1 * (m0 - m1) ** 2
        if v > best:
            best, thr = v, t - 0.5
    return thr


def luminance(rgb):
    return rgb[..., :3] @ np.array([0.299, 0.587, 0.114])


def find_ink(rgb, box, dark_margin=0.0, max_chroma=60.0):
    """Dark lettering inside box: (mask over the whole image, tight ink box, ink colour).

    Ink is darker than the box's Otsu threshold (minus dark_margin) and nearly grey (max minus
    min channel below max_chroma), so a coloured line crossing the label is not taken for ink."""
    x0, y0, x1, y1 = box
    lum = luminance(rgb)
    sub = lum[y0:y1, x0:x1]
    thr = otsu(sub.ravel()) - dark_margin
    chroma = rgb[y0:y1, x0:x1].max(-1) - rgb[y0:y1, x0:x1].min(-1)
    ink = np.zeros(lum.shape, bool)
    ink[y0:y1, x0:x1] = (sub < thr) & (chroma < max_chroma)
    if not ink.any():
        raise ValueError("no ink found in box %s" % (box,))
    tight = ink_box(ink[y0:y1, x0:x1], x0, y0)
    vals = rgb[ink][:, :3]
    order = np.argsort(luminance(vals))
    color = np.median(vals[order[: max(1, len(order) // 3)]], axis=0)
    return ink, tight, color


def ink_box(m, x0=0, y0=0, row_frac=0.4, col_frac=0.25):
    """Tight box of lettering in an ink mask, ignoring stray marks around it: the rows holding
    at least row_frac of the fullest row's ink, then the outermost columns with at least
    col_frac of those rows inked."""
    rows = m.sum(1)
    ry = np.nonzero(rows >= row_frac * rows.max())[0]
    ty0, ty1 = int(ry.min()), int(ry.max()) + 1
    cols = m[ty0:ty1].sum(0)
    cx = np.nonzero(cols >= max(1.0, col_frac * (ty1 - ty0)))[0]
    return (x0 + int(cx.min()), y0 + ty0, x0 + int(cx.max()) + 1, y0 + ty1)


def dilate(mask, r=1):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, 0), dx, 1)
    return out


def render_label(text, font_path, size_wh):
    """Coverage (H, W) floats 0-1 of text stretched so its ink fills size_wh exactly."""
    from PIL import Image, ImageDraw, ImageFont
    tw, th = size_wh
    px = max(16, th * 4)
    font = ImageFont.truetype(font_path, px)
    l, t, r, b = font.getbbox(text)
    im = Image.new("L", (r - l + px, b - t + px), 0)
    ImageDraw.Draw(im).text((px // 2 - l, px // 2 - t), text, font=font, fill=255)
    bb = im.getbbox()
    im = im.crop(bb).resize((tw, th), Image.LANCZOS)
    return np.asarray(im, np.float64) / 255.0


# ---------------------------------------------------------------- whole image

def premul_upscale(rgba, scale):
    a = rgba[..., 3:4] / 255.0
    up_a = np.clip(upscale(a, scale), 0.0, 1.0)
    up_c = upscale(rgba[..., :3] * a, scale)
    rgb = np.where(up_a > 1e-4, up_c / np.maximum(up_a, 1e-4), 0.0)
    return np.concatenate([np.clip(rgb, 0, 255), up_a * 255.0], axis=-1)


def redraw(rgba, spec, spec_dir="."):
    """Apply a spec to an (H, W, 4) uint8/float image; returns (H*s, W*s, 4) uint8."""
    rgba = rgba.astype(np.float64)
    h, w = rgba.shape[:2]
    s = int(spec.get("scale", 4))
    out = premul_upscale(rgba, s)
    labels = spec.get("labels", [])
    found = []
    removed = np.zeros((h, w), bool)
    for lab in labels:
        ink, tight, color = find_ink(rgba[..., :3], lab["box"], lab.get("dark_margin", 0.0),
                                    lab.get("max_chroma", 60.0))
        removed |= dilate(ink, int(lab.get("grow", 1)))
        if "ink_box" in lab:
            tight = tuple(int(v) for v in lab["ink_box"])
        found.append((lab, tight, color))
        if spec.get("verbose"):
            print("label %r: ink box %s colour %s" % (lab["text"], tight, np.round(color).astype(int).tolist()))
    for reg in spec.get("regions", [[0, 0, w, h]]):
        x0, y0, x1, y1 = reg
        pad = 4
        px0, py0, px1, py1 = max(0, x0 - pad), max(0, y0 - pad), min(w, x1 + pad), min(h, y1 + pad)
        sub = rgba[py0:py1, px0:px1, :3]
        flat = redraw_flat(sub, s, classes=int(spec.get("classes", 14)),
                           sharpness=float(spec.get("sharpness", 24.0)),
                           shape_sigma=float(spec.get("shape_sigma", 0.7)),
                           mix_tol=float(spec.get("mix_tol", 0.0)),
                           ink_detail=float(spec.get("ink_detail", 1.0)),
                           exclude=removed[py0:py1, px0:px1])
        ox, oy = (x0 - px0) * s, (y0 - py0) * s
        new = flat[oy:oy + (y1 - y0) * s, ox:ox + (x1 - x0) * s]
        # feather the region's edge into the plain upscale around it (no seam)
        fw = max(1, int(spec.get("feather", 2)) * s)
        ry = np.minimum(np.arange(new.shape[0]), np.arange(new.shape[0])[::-1]) + 0.5
        rx = np.minimum(np.arange(new.shape[1]), np.arange(new.shape[1])[::-1]) + 0.5
        wgt = np.clip(np.minimum(ry[:, None], rx[None, :]) / fw, 0.0, 1.0)[..., None]
        old = out[y0 * s:y1 * s, x0 * s:x1 * s, :3]
        old[:] = old * (1 - wgt) + new * wgt
    font_default = spec.get("font")
    for lab, tight, color in found:
        font = lab.get("font", font_default)
        if not font:
            raise ValueError("label %r: no font" % lab["text"])
        if not os.path.isabs(font):
            font = os.path.join(spec_dir, font)
        col = np.array(lab.get("color", color), np.float64)
        tx0, ty0, tx1, ty1 = (v * s for v in tight)
        cov = render_label(lab["text"], font, (tx1 - tx0, ty1 - ty0))
        halo = float(lab.get("halo", 0.0))
        if halo > 0:
            hcov = np.clip(gauss_blur(np.pad(cov, s), s * 0.5) * 3.0, 0, 1) * halo
            hy0, hx0 = ty0 - s, tx0 - s
            reg = out[hy0:hy0 + hcov.shape[0], hx0:hx0 + hcov.shape[1], :3]
            hc = np.array(lab.get("halo_color", [235, 225, 195]), np.float64)
            reg[:] = reg * (1 - hcov[..., None]) + hc * hcov[..., None]
        reg = out[ty0:ty1, tx0:tx1, :3]
        reg[:] = reg * (1 - cov[..., None]) + col * cov[..., None]
    return np.clip(out + 0.5, 0, 255).astype(np.uint8)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("--spec", required=True, help="JSON spec (see the module docstring)")
    args = ap.parse_args(argv)
    from PIL import Image
    with open(args.spec) as f:
        spec = json.load(f)
    rgba = np.asarray(Image.open(args.src).convert("RGBA"))
    res = redraw(rgba, spec, os.path.dirname(os.path.abspath(args.spec)))
    Image.fromarray(res, "RGBA").save(args.out)
    print("%s: %dx%d -> %dx%d" % (args.out, rgba.shape[1], rgba.shape[0], res.shape[1], res.shape[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
