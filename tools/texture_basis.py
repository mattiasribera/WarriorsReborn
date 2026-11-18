#!/usr/bin/env python3
"""Converts a texture pack (colour replacements and/or relief companion maps) into a KTX2 pack.

Basis Universal (https://github.com/BinomialLLC/basis_universal, Apache-2.0) supercompresses GPU
blocks; the runtime transcodes <hash>.ktx2 files to BC1/BC3/BC7 on its texture-pack loader threads
(runtime/src/ktx2.h), so a KTX2 pack is smaller on disk than a .dds pack and takes the same memory
as the BC format it transcodes to. Reads every <hash>, <hash>_n and <hash>_s texture of --pack, as
.png and/or .dds (a .dds is decoded level by level by the pngraw helper; with both, --source picks
one), with the pack's selection.csv / manifest.csv categories (see tools/texture_compress.py), and
writes into --out, per texture:
  - UI/font colour textures (texture_compress.py's rule: category ui/font, the ui-like heuristic,
    --skip-list; only PNGs): the PNG is copied as is;
  - opaque colour (BC1 in a .dds pack): --opaque, default keep (the BC1 .dds is copied as is);
    etc1s transcodes to BC1 (the same memory, a third of the size on disk, but visibly softer);
  - colour with alpha (BC7 in a .dds pack): --alpha codec, default xubc7 (transcodes to BC7);
  - _n (normal + height in alpha) and _s (4 independent channels) maps: --maps codec, default
    xubc7 (transcodes to BC7), with linear metrics.
etc1s -> BC1 (BC3 with alpha) is the smallest but loses detail and correlates channels; uastc ->
BC7 is high quality, near BC7 sizes; xubc7 is BC7 itself, supercompressed (the runtime gets those
BC7 blocks back exactly), its --xubc7-rdo level trades size for quality.
Every .ktx2 has the complete mip chain down to 1x1: a .dds source keeps its own levels and its
BC1/BC7 class; a PNG gets its chain made exactly as tools/texture_compress.py makes it (Lanczos-3 in
display space, premultiplied alpha, --alpha-coverage), passed to the encoder as an uncompressed
mipmapped .dds. --like DDS_PACK reproduces an existing .dds pack made from the same PNGs (its
textures, BC1/BC7 choice and sizes). Other files of the pack (.emit masks, CSVs) are copied as is,
so --out is a complete pack. Outputs newer than their input are skipped (resumable; --force redoes
them); -j runs that many single-threaded encoders.

The encoder is basisu from Basis Universal v2.50 (cmake --build . --target basisu); it is not part
of this repository: pass --basisu PATH or set TEXPACK_BASISU. PNG and .dds decoding use the pngraw
helper of tools/texture_upscale.py (--pngraw PATH; runtime/build/pngraw); numpy is required.
"""
import argparse
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
import texture_compress as tc  # noqa: E402  (classify, mip_chain, read_categories, load_skip_list, level_dims)
import texture_upscale as tu   # noqa: E402  (read_png, PNGRAW)

ENCODER_ENV = "TEXPACK_BASISU"
CODECS = ("etc1s", "uastc", "xubc7")
KTX2_ID = b"\xabKTX 20\xbb\r\n\x1a\n"
HEX = set("0123456789abcdef")


def codec_args(codec, opts, linear):
    """basisu options of one codec (opts: the quality settings of build_parser)."""
    if codec == "etc1s":
        a = ["-etc1s", "-quality", str(opts["etc1s_quality"]), "-effort", str(opts["effort"])]
    elif codec == "uastc":
        a = ["-uastc", "-uastc_level", str(opts["uastc_level"])]
        if opts["uastc_rdo"] > 0:
            a += ["-uastc_rdo_l", "%g" % opts["uastc_rdo"]]
    elif codec == "xubc7":
        a = ["-xubc7", "-effort", str(opts["effort"]), "-xubc7_rdo_level", str(opts["xubc7_rdo"])]
        if opts["xubc7_bc7e"] >= 0:
            a += ["-xubc7_bc7e_scalar_level", str(opts["xubc7_bc7e"])]
    else:
        raise ValueError(codec)
    return a + (["-linear"] if linear else [])


def rgba_dds(levels):
    """Uncompressed 32-bit RGBA .dds (legacy header, R in the low byte) holding every level: the
    source mips basisu keeps (encoder/basisu_comp.cpp read_uncompressed_dds_file)."""
    h, w = levels[0].shape[:2]
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000   # CAPS HEIGHT WIDTH PITCH PIXELFORMAT MIPMAPCOUNT
    hdr = struct.pack("<7I", 124, flags, h, w, w * 4, 0, len(levels)) + b"\0" * 44
    hdr += struct.pack("<2I4s5I", 32, 0x41, b"\0\0\0\0", 32, 0xFF, 0xFF00, 0xFF0000, 0xFF000000)   # RGB | ALPHAPIXELS
    hdr += struct.pack("<5I", 0x1000 | 0x400008, 0, 0, 0, 0)
    body = b"".join(np.ascontiguousarray(l if l.shape[2] == 4 else np.concatenate(
        [l, np.full(l.shape[:2] + (1,), 255, np.uint8)], axis=2)).tobytes() for l in levels)
    return b"DDS " + hdr + body


def ktx2_info(data):
    """(w, h, levels, faces, layers) of a KTX2 file, or ValueError."""
    if len(data) < 48 or data[:12] != KTX2_ID:
        raise ValueError("not a KTX2 file")
    w, h, d, layers, faces, levels = struct.unpack_from("<6I", data, 20)
    return w, h, levels, faces, layers


def encode_ktx2(basisu, levels, codec, opts, linear, work, dst):
    """Encodes the mip chain (H x W x 4 uint8 arrays) into dst (written atomically; work: a scratch
    directory)."""
    src, tmp = os.path.join(work, "src.dds"), os.path.join(work, "out.ktx2")
    with open(src, "wb") as f:
        f.write(rgba_dds(levels))
    cmd = [basisu, "-ktx2", "-no_multithreading", "-quiet", "-no_stats"] + codec_args(codec, opts, linear) + [src, "-output_file", tmp]
    r = subprocess.run(cmd, capture_output=True, env=dict(os.environ, OMP_NUM_THREADS="1"))
    if r.returncode or not os.path.exists(tmp):
        raise RuntimeError("basisu failed (%d): %s" % (r.returncode, (r.stdout + r.stderr).decode(errors="replace")[-300:]))
    with open(tmp, "rb") as f:
        w, h, n, faces, layers = ktx2_info(f.read(64))
    want = (levels[0].shape[1], levels[0].shape[0], len(levels))
    if (w, h, n) != want or faces != 1 or layers > 1:
        raise RuntimeError("basisu wrote %dx%d with %d levels (%d faces, %d layers), expected %dx%d with %d" % ((w, h, n, faces, layers) + want))
    part = dst + ".part"   # next to dst, so the final rename is atomic whatever filesystem work is on
    shutil.move(tmp, part)
    os.replace(part, dst)


def up_to_date(src, dst):
    return os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src)


def split_name(name):
    """('<hash>', '' | '_n' | '_s', '.png' | ...) of a pack file name, or None for other files."""
    low = name.lower()
    if len(low) < 20 or not set(low[:16]) <= HEX:
        return None
    rest = low[16:]
    for kind in ("_n", "_s", ""):
        if rest.startswith(kind) and rest[len(kind):] in (".png", ".dds", ".ktx2"):
            return low[:16], kind, rest[len(kind):]
    return None


def scan(pack):
    """(jobs, others): jobs = [(hash, kind, {".png": path, ".dds": path})] for every texture or map
    with a .png and/or a .dds, others = [files copied as is: masks, CSVs ...]; .ktx2 files of the
    input are ignored."""
    files, others = {}, []
    for n in sorted(os.listdir(pack)):
        p = os.path.join(pack, n)
        if not os.path.isfile(p):
            continue
        s = split_name(n)
        if not s:
            others.append(p)
        elif s[2] != ".ktx2":
            files.setdefault((s[0], s[1]), {})[s[2]] = p
    return [(h, k, f) for (h, k), f in sorted(files.items())], others


def like_choice(like, name):
    """What a pack holds for a file stem (<hash>, <hash>_n ...): ('bc1' | 'bc7', (w, h)) for its .dds
    (BC3 counts as bc7), ('png', None) for a .png only, None when it has neither."""
    dds = os.path.join(like, name + ".dds")
    if os.path.exists(dds):
        with open(dds, "rb") as f:
            hdr = f.read(148)
        if len(hdr) < 128 or hdr[:4] != b"DDS ":
            raise ValueError("%s is not a DDS file" % dds)
        hh, ww = struct.unpack_from("<2I", hdr, 12)
        fourcc = hdr[84:88]
        code = struct.unpack_from("<I", hdr, 128)[0] if fourcc == b"DX10" and len(hdr) >= 132 else fourcc
        return (tc.BC1 if code in (71, b"DXT1") else tc.BC7), (ww, hh)
    return ("png", None) if os.path.exists(os.path.join(like, name + ".png")) else None


def chain_at(levels, size):
    """The mip chain from the level of this size down (a reference .dds made smaller than the PNG)."""
    for k, l in enumerate(levels):
        if (l.shape[1], l.shape[0]) == tuple(size):
            return levels[k:]
    raise ValueError("--like: %dx%d is not a mip level of the %dx%d PNG" % (size[0], size[1], levels[0].shape[1], levels[0].shape[0]))


def read_dds_levels(path):
    """Every level of a BC1/BC3/BC7 .dds as H x W x 4 uint8 RGBA, decoded by pngraw (the runtime's
    own decoders), and its format: (levels, 'bc1' | 'bc7'); BC3 counts as bc7."""
    fmt, _ = like_choice(os.path.dirname(path), os.path.splitext(os.path.basename(path))[0])
    res = subprocess.run([tu.PNGRAW, path], capture_output=True)
    if res.returncode:
        raise RuntimeError("cannot decode %s: %s" % (path, res.stderr.decode(errors="replace").strip()))
    out, at, data = [], 0, res.stdout
    while at < len(data):
        nl = data.index(b"\n", at)
        w, h = map(int, data[at:nl].split())
        out.append(np.frombuffer(data, np.uint8, w * h * 4, nl + 1).reshape(h, w, 4).copy())
        at = nl + 1 + w * h * 4
    return out, fmt


def pick_source(files, mode):
    """The file to convert: its .png or its .dds (--source newer: the more recent one, the .png on a
    tie; png / dds: that one when present)."""
    png, dds = files.get(".png"), files.get(".dds")
    if not png or not dds:
        return png or dds
    if mode == "png":
        return png
    if mode == "dds":
        return dds
    return dds if os.path.getmtime(dds) > os.path.getmtime(png) else png


def convert_one(job):
    """One texture or map; returns a result dict (never raises)."""
    h, kind, files, opts = job
    t0 = time.time()
    src = pick_source(files, opts["source"])
    res = {"hash": h + kind, "kind": None, "in": os.path.getsize(src), "out": 0, "s": 0.0, "error": None}
    try:
        tu.PNGRAW = opts["pngraw"]
        stem = h + kind
        dst, png, dds_out = (os.path.join(opts["out"], stem + e) for e in (".ktx2", ".png", ".dds"))
        like = like_choice(opts["like"], stem) if opts["like"] else None
        if opts["like"] and like is None:   # not in the reference pack: not in the output either
            for stale in (dst, png, dds_out):
                if os.path.exists(stale):
                    os.remove(stale)
            res["kind"] = "not-in-like"
            return res
        from_dds = src.lower().endswith(".dds")
        if from_dds:
            levels, fmt = read_dds_levels(src)
        else:
            rgba = tu.read_png(src)
            if rgba.shape[2] == 3:
                rgba = np.concatenate([rgba, np.full(rgba.shape[:2] + (1,), 255, np.uint8)], axis=2)
            # the class first (a changed --skip-list or category moves a texture between PNG and .ktx2)
            fmt = (like[0] if like else
                   tc.BC7 if kind else tc.classify(rgba, opts["categories"].get(h), tc.BC1, opts["ui_heuristic"], h in opts["skip"]))
        if fmt == "png":
            if opts["force"] or not up_to_date(src, png):
                shutil.copyfile(src, png)
            for stale in (dst, dds_out):   # either would win over the copied PNG
                if os.path.exists(stale):
                    os.remove(stale)
            res.update(kind="png", out=os.path.getsize(png))
            return res
        codec = opts["maps"] if kind else opts["alpha"] if fmt == tc.BC7 else opts["opaque"]
        keep = opts["like"] and os.path.join(opts["like"], stem + ".dds") or files.get(".dds")
        if codec == "keep" and keep and os.path.exists(keep):   # opaque BC1 stays the .dds it is
            if opts["force"] or not up_to_date(keep, dds_out):
                shutil.copyfile(keep, dds_out)
            if os.path.exists(dst):
                os.remove(dst)
            res.update(kind="kept-dds", out=os.path.getsize(dds_out))
            return res
        if codec == "keep":
            codec = "etc1s"   # no .dds to keep (a PNG-only opaque texture)
        if os.path.exists(dds_out):
            os.remove(dds_out)   # a .dds of the same map would win over the new .ktx2
        if not opts["force"] and up_to_date(src, dst):
            res["kind"] = "up-to-date"
            res["out"] = os.path.getsize(dst) + (os.path.getsize(png) if os.path.exists(png) else 0)
            return res
        if not from_dds:
            ref = None
            if not kind and fmt == tc.BC7 and (opts["coverage"] == "all" or (
                    opts["coverage"] == "cutout" and opts["categories"].get(h) == "cutout")):
                ref = opts["coverage_ref"]
            levels = tc.mip_chain(rgba, opts["mip_filter"], ref)
            if like and like[1] != (rgba.shape[1], rgba.shape[0]):
                levels = chain_at(levels, like[1])
        with tempfile.TemporaryDirectory(prefix=".work-", dir=opts["out"]) as work:
            encode_ktx2(opts["basisu"], levels, codec, opts, bool(kind), work, dst)
        if opts["with_png"] and files.get(".png") and (opts["force"] or not up_to_date(files[".png"], png)):
            shutil.copyfile(files[".png"], png)
        res["kind"] = ("map-" if kind else "") + codec + ("-from-dds" if from_dds else "")
        res["out"] = os.path.getsize(dst) + (os.path.getsize(png) if opts["with_png"] and os.path.exists(png) else 0)
    except Exception as e:   # reported per texture; the run goes on
        res["error"] = "%s: %s" % (type(e).__name__, e)
    res["s"] = time.time() - t0
    return res


def find_basisu(path):
    path = path or os.environ.get(ENCODER_ENV) or shutil.which("basisu")
    return path if path and os.path.isfile(path) and os.access(path, os.X_OK) else None


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--pack", required=True, help="input PNG pack directory (<hash>.png, <hash>_n.png, <hash>_s.png)")
    ap.add_argument("--out", required=True, help="output pack directory (created)")
    ap.add_argument("--basisu", help="basisu executable (default: $%s, then basisu on PATH)" % ENCODER_ENV)
    ap.add_argument("--pngraw", default=tu.PNGRAW, help="PNG decoder (default: %(default)s)")
    ap.add_argument("-j", "--jobs", type=int, default=3, help="parallel textures, one encoder thread each (default 3)")
    ap.add_argument("--opaque", choices=CODECS + ("keep",), default="keep",
                    help="opaque colour textures: keep (default) copies their BC1 .dds as is (from the pack or --like; etc1s "
                         "when there is none); etc1s -> BC1 is a third of the size but visibly softer")
    ap.add_argument("--alpha", choices=CODECS, default="xubc7", help="codec of colour textures with alpha (default xubc7 -> BC7)")
    ap.add_argument("--maps", choices=("uastc", "xubc7"), default="xubc7", help="codec of _n/_s maps (default xubc7 -> BC7)")
    ap.add_argument("--etc1s-quality", type=int, default=100, choices=range(1, 101), metavar="1-100",
                    help="ETC1S quality (basisu -quality; default 100)")
    ap.add_argument("--effort", type=int, default=4, choices=range(11), metavar="0-10", help="ETC1S / XUBC7 encoder effort (default 4)")
    ap.add_argument("--uastc-level", type=int, default=2, choices=range(5), metavar="0-4", help="UASTC level (default 2)")
    ap.add_argument("--uastc-rdo", type=float, default=0.0, help="UASTC RDO lambda (0 = off, try 0.5-3)")
    ap.add_argument("--xubc7-rdo", type=int, default=0, choices=range(101), metavar="0-100", help="XUBC7 RDO level (0 = lossless BC7, default)")
    ap.add_argument("--xubc7-bc7e", type=int, default=6, choices=range(-1, 7), metavar="-1-6",
                    help="XUBC7 base encoder: bc7e_scalar level 0-6 (default 6: from a BC7 .dds it gives back nearly the same "
                         "blocks), -1 = basisu's faster bc7f")
    ap.add_argument("--mip-filter", choices=("lanczos3", "box"), default="lanczos3", help="mip filter (box = the runtime's own)")
    ap.add_argument("--alpha-coverage", choices=("all", "cutout", "none"), default="all",
                    help="keep alpha-test coverage on every level of colour textures with alpha (as texture_compress.py)")
    ap.add_argument("--alpha-ref", type=int, default=128, help="alpha-test reference for --alpha-coverage (default 128)")
    ap.add_argument("--source", choices=("newer", "png", "dds"), default="newer",
                    help="a texture or map with both a .png and a .dds: convert the newer one (default; a .dds rewritten after "
                         "its .png, e.g. by material_shine.py, holds what the game shows), or always the .png / the .dds. A .dds "
                         "source keeps its own mips and BC1/BC7 choice")
    ap.add_argument("--like", metavar="DDS_PACK",
                    help="reproduce a .dds pack made from this PNG pack (texture_compress.py): only its textures, BC1 -> --opaque "
                         "and BC7/BC3 -> --alpha per its files, at its sizes (a smaller .dds: the PNG's mip chain from that level), "
                         "PNG where it kept a PNG; replaces the classification")
    ap.add_argument("--manifest", help="extra CSV with hash,category columns")
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
    basisu = find_basisu(args.basisu)
    if not basisu:
        sys.exit("basisu not found: build it from https://github.com/BinomialLLC/basis_universal (v2.50, cmake --build . --target basisu) "
                 "and pass --basisu PATH or set %s" % ENCODER_ENV)
    if os.path.realpath(args.out) == os.path.realpath(args.pack):
        sys.exit("--out must differ from --pack")
    os.makedirs(args.out, exist_ok=True)
    opts = {"basisu": basisu, "pngraw": args.pngraw, "out": args.out, "opaque": args.opaque, "alpha": args.alpha, "maps": args.maps,
            "etc1s_quality": args.etc1s_quality, "effort": args.effort, "uastc_level": args.uastc_level, "uastc_rdo": args.uastc_rdo,
            "xubc7_rdo": args.xubc7_rdo, "xubc7_bc7e": args.xubc7_bc7e, "mip_filter": args.mip_filter, "coverage": args.alpha_coverage,
            "coverage_ref": args.alpha_ref, "categories": tc.read_categories(args.pack, args.manifest),
            "skip": tc.load_skip_list(args.skip_list), "ui_heuristic": not args.no_ui_heuristic, "with_png": args.with_png,
            "force": args.force, "like": args.like, "source": args.source}
    if args.like and not os.path.isdir(args.like):
        sys.exit("cannot read the --like pack %s" % args.like)
    found, others = scan(args.pack)
    copied = 0
    for p in others:
        dst = os.path.join(args.out, os.path.basename(p))
        if args.force or not up_to_date(p, dst):
            shutil.copyfile(p, dst)
            copied += 1
    jobs = [(h, k, f, opts) for h, k, f in found]
    print("textures: %d in %s (%d other files copied as is), %d workers, %s" % (len(jobs), args.pack, len(others), args.jobs, basisu))
    t0 = time.time()
    results = []
    with ProcessPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        for i, r in enumerate(ex.map(convert_one, jobs, chunksize=1), 1):
            results.append(r)
            if r["error"]:
                print("  %s: %s" % (r["hash"], r["error"]), file=sys.stderr)
            if i % 100 == 0 or i == len(jobs):
                print("  %d/%d (%.0f s)" % (i, len(jobs), time.time() - t0), flush=True)
    kinds = {}
    for r in results:
        k = r["kind"] if not r["error"] else "failed"
        kinds[k] = kinds.get(k, 0) + 1
    ok = [r for r in results if not r["error"]]
    mib = 1 << 20
    print("done in %.1f s: %s" % (time.time() - t0, ", ".join("%s %d" % (k, n) for k, n in sorted(kinds.items()))))
    print("size: sources in %.1f MiB, converted out %.1f MiB (+ %d files copied)" % (
        sum(r["in"] for r in ok) / mib, sum(r["out"] for r in ok) / mib, copied))
    return 1 if kinds.get("failed") else 0


if __name__ == "__main__":
    sys.exit(main())
