#!/usr/bin/env python3
"""Named audio extractor for The Warriors (Xbox): every XACT wave as a WAV named by its cue name.

Inputs (all from the user's own copy of the game, nothing game-derived lives in the repository):
  <disc>/audio/*.xsb, *.xwb   the sound bank (cue -> wave bank, wave) and the 369 wave banks
  <disc>/XBoxWad.idx, *.wad*  warriors.glr chunk 0x29 "Static Sounds": {crc32(name), cue, u16 bank,
                              u16} sorted by hash, and its debug twin (index hash 0x67775e17) whose
                              records also carry the last 15 characters of each name
  <extracted>/lua_src, <extracted>/wad/**/*.scn, <disc>/default.xbe
                              candidate strings for the names (tools/wad_extract.py output)

Names are recovered by CRC-32 of candidate "vags/..." strings, accepted only when the debug twin's
tail matches, then extended by joining known directories with the tails until nothing new is
found (~22.7k of 23.5k cues). The cue map and decoding run in runtime/build/xact_extract, which
uses the runtime's own sound bank parser and wave reader (built by `make -C runtime`).

Output (default extracted/audio, gitignored):
  <name>.wav                   e.g. vags/character/ajax/fight/hit_01.wav (16-bit PCM)
  _unnamed/<bank>_<index>.wav  cues whose name is unknown
  catalog.csv                  name, bank, index, duration, channels, rate, category, file
  names.csv                    recovered names (cache; --rescan-names rebuilds it)

The same "vags/..." path under a --sound-pack folder replaces that sound in the runtime.

Usage: audio_extract.py [--disc DIR] [--extracted DIR] [--out DIR] [--filter PATTERN] [-j N]
"""
import argparse
import collections
import concurrent.futures
import csv
import fnmatch
import glob
import os
import re
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TWIN_HASH = 0x67775E17          # XBoxWad.idx hash of the named debug copy of the table
STATIC_SOUNDS = 0x29            # .glr chunk type of the name table
TAIL = 15                       # characters of each name kept by the debug twin
CATALOG_FIELDS = ["name", "bank", "index", "duration", "channels", "rate", "category", "file"]

Sound = collections.namedtuple("Sound", "hash cue bank tail")


def crc32(s):
    return zlib.crc32(s.encode("latin1")) & 0xFFFFFFFF


# ---- WAD archive (layout in tools/wad_extract.py) ----

def wad_hash(path):
    return crc32("ee_files\\" + path.lower().replace("/", "\\"))


def wad_read(disc, name_hash):
    """Bytes of the WAD file whose index name hash is name_hash, or None."""
    try:
        with open(os.path.join(disc, "XBoxWad.idx"), "rb") as f:
            idx = f.read()
    except OSError:
        return None
    nwads, nfiles, _, wad_table, file_table = struct.unpack_from("<5I", idx, 0)
    for i in range(nfiles):
        loc, size, _, h = struct.unpack_from("<4I", idx, file_table + 16 * i)
        if h != name_hash or loc >> 24 >= nwads:
            continue
        wad = idx[wad_table + 36 * (loc >> 24):wad_table + 36 * (loc >> 24) + 32].split(b"\0")[0].decode("latin1")
        with open(os.path.join(disc, wad), "rb") as f:
            f.seek((loc & 0xFFFFFF) << 10)
            return f.read(size)
    return None


def static_sounds(blob, named):
    """Records of the .glr chunk 0x29: Sound(hash, cue, bank number, tail or '')."""
    count = struct.unpack_from("<I", blob, 0)[0]
    o = 16
    for _ in range(count):
        typ, size = struct.unpack_from("<II", blob, o)
        if typ == STATIC_SOUNDS:
            break
        o += 16 + size
    else:
        return []
    o += 16
    n = struct.unpack_from("<I", blob, o)[0]
    o += 4
    out = []
    for _ in range(n):
        tail = ""
        if named:
            tail = blob[o:o + 16].split(b"\0")[0].decode("latin1")
            o += 16
        h, cue, bank, _ = struct.unpack_from("<IIHH", blob, o)
        o += 12
        out.append(Sound(h, cue, bank, tail))
    return out


# ---- name recovery ----

CANDIDATE = re.compile(rb"[A-Za-z0-9_/\\.\-]{4,}")


def harvest(blob):
    return {m.decode("latin1") for m in CANDIDATE.findall(blob)}


def recover_names(sounds, candidates, max_rounds=8):
    """{hash: name} for the sounds whose name is found. A CRC match counts only when the sound's
    tail (if known) matches the name's last 15 characters, which rejects CRC-32 collisions."""
    by_hash = {s.hash: s for s in sounds}
    found = {}

    def attempt(name):
        h = crc32(name)
        s = by_hash.get(h)
        if s is None or h in found or (s.tail and name[-TAIL:] != s.tail[-TAIL:]):
            return False
        found[h] = name
        return True

    for c in candidates:
        c = c.replace("\\", "/")
        attempt(c)
        if not c.startswith("vags/"):
            attempt("vags/" + c)
    for _ in range(max_rounds):
        dirs = set()
        for name in found.values():
            parts = name.split("/")
            for k in range(1, len(parts)):
                dirs.add("/".join(parts[:k]) + "/")
        by_suffix = collections.defaultdict(list)   # directory suffixes ending in "/" -> directories
        for d in dirs:
            for k in range(1, min(len(d), TAIL) + 1):
                by_suffix[d[-k:]].append(d)
        before = len(found)
        for s in sounds:
            if s.hash in found or not s.tail:
                continue
            t = s.tail
            done = False
            # the tail starts inside a directory: <directory ending in t[:p+1]> + t[p+1:]
            for p in (i for i, ch in enumerate(t) if ch == "/"):
                for d in by_suffix.get(t[:p + 1], ()):
                    if attempt(d + t[p + 1:]):
                        done = True
                        break
                if done:
                    break
            # the tail starts at a path component: <directory> + t
            if not done and len(t) == TAIL:
                for d in dirs:
                    if attempt(d + t):
                        break
        if len(found) == before:
            break
    return found


def candidate_strings(disc, extracted):
    paths = sorted(glob.glob(os.path.join(extracted, "lua_src", "**", "*.lua"), recursive=True))
    paths += [os.path.join(disc, "default.xbe")]
    paths += sorted(glob.glob(os.path.join(extracted, "wad", "**", "*.scn"), recursive=True))
    out = set()
    for p in paths:
        try:
            with open(p, "rb") as f:
                out |= harvest(f.read())
        except OSError:
            pass
    return out


def load_names(disc, extracted, cache, rescan):
    """{cue: name} from the cache, else recovered from the game files (and cached)."""
    if not rescan and os.path.isfile(cache):
        with open(cache, newline="") as f:
            return {int(r["cue"]): r["name"] for r in csv.DictReader(f) if r["name"]}
    glr = wad_read(disc, wad_hash("warriors.glr"))
    if glr is None:
        print(f"[audio] no warriors.glr in {disc}/XBoxWad.idx: every cue goes to _unnamed/", file=sys.stderr)
        return {}
    twin = wad_read(disc, TWIN_HASH)
    sounds = static_sounds(twin, named=True) if twin else static_sounds(glr, named=False)
    found = recover_names(sounds, candidate_strings(disc, extracted))
    names = {s.cue: found[s.hash] for s in sounds if s.hash in found}
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    with open(cache, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["cue", "hash", "name"])
        for s in sorted(sounds, key=lambda s: s.cue):
            w.writerow([s.cue, f"{s.hash:08x}", found.get(s.hash, "")])
    print(f"[audio] names recovered for {len(names)} of {len(sounds)} cues")
    return names


# ---- layout ----

SAFE_NAME = re.compile(r"^[A-Za-z0-9_.\-]+(/[A-Za-z0-9_.\-]+)*$")


def out_rel(name, bank, index):
    """Output path of a wave relative to the output folder (forward slashes)."""
    name = name.replace("\\", "/")
    if name and SAFE_NAME.match(name) and ".." not in name.split("/"):
        return name + ".wav"
    return f"_unnamed/{bank}_{index:04d}.wav"


def category(name, bank):
    parts = name.split("/")
    top = parts[1] if len(parts) > 2 else ""
    if top == "character":
        return "voice"
    if top == "speeches":
        return "dialogue"
    if top in ("xboxmusic", "music"):
        return "music"
    if top == "ambient":
        return "ambient"
    if top:
        return "sfx"
    m = re.match(r"xbox(\d+)$", bank)
    n = int(m.group(1)) if m else -1
    return {2: "voice", 3: "dialogue"}.get(n, "music" if n >= 4 else "sfx")


def matches(rel, pattern):
    if any(ch in pattern for ch in "*?["):
        return fnmatch.fnmatchcase(rel, pattern)
    return pattern in rel


# ---- decoding (runtime/build/xact_extract) ----

def cue_waves(helper, xsb):
    res = subprocess.run([helper, "cues", xsb], capture_output=True, text=True, check=True)
    out = []
    for line in res.stdout.splitlines():
        cue, bank, wave = line.split("\t")
        out.append((int(cue), bank, int(wave)))
    return out


def decode(helper, jobs, workers):
    """jobs: [(xwb path, index, out path)] -> [result line per job] using `workers` helpers."""
    if not jobs:
        return []
    workers = max(1, min(workers, len(jobs)))
    # small batches handed out as workers free up: banks differ a lot in size (one holds 18k
    # voice lines), and consecutive jobs of a batch reuse the helper's open bank
    size = max(1, min(256, -(-len(jobs) // workers)))
    chunks = [jobs[i:i + size] for i in range(0, len(jobs), size)]

    def run(chunk):
        text = "".join(f"{p}\t{i}\t{o}\n" for p, i, o in chunk)
        res = subprocess.run([helper, "decode"], input=text, capture_output=True, text=True, check=True)
        lines = res.stdout.splitlines()
        return lines + ["err\thelper stopped"] * (len(chunk) - len(lines))

    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        return [line for part in ex.map(run, chunks) for line in part]


def write_catalog(path, rows, merge):
    """Writes catalog.csv; with merge, rows of earlier (filtered) runs not redone are kept."""
    keep = {}
    if merge and os.path.isfile(path):
        with open(path, newline="") as f:
            keep = {r["file"]: r for r in csv.DictReader(f)}
    keep.update((r["file"], r) for r in rows)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CATALOG_FIELDS)
        w.writeheader()
        for r in sorted(keep.values(), key=lambda r: r["file"]):
            w.writerow(r)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--disc", default=os.path.join(ROOT, "extracted", "disc"))
    ap.add_argument("--extracted", default=os.path.join(ROOT, "extracted"),
                    help="tools/wad_extract.py output (lua_src, wad/**.scn) for name candidates")
    ap.add_argument("--out", default=os.path.join(ROOT, "extracted", "audio"))
    ap.add_argument("--helper", default=os.path.join(ROOT, "runtime", "build", "xact_extract"))
    ap.add_argument("--filter", help="glob (with * ? [) or substring of the output path, e.g. '*/speeches/l01/*'")
    ap.add_argument("-j", type=int, default=os.cpu_count() or 1, help="parallel decoders")
    ap.add_argument("--rescan-names", action="store_true", help="recover names again instead of using names.csv")
    a = ap.parse_args(argv)
    t0 = time.time()
    if not os.access(a.helper, os.X_OK):
        print(f"{a.helper} missing: run make -C runtime", file=sys.stderr)
        return 2
    xsbs = sorted(glob.glob(os.path.join(a.disc, "audio", "*.xsb")))
    if not xsbs:
        print(f"no sound bank in {a.disc}/audio", file=sys.stderr)
        return 2
    names = load_names(a.disc, a.extracted, os.path.join(a.out, "names.csv"), a.rescan_names)
    rows, jobs = [], []
    for xsb in xsbs:
        for cue, bank, wave in cue_waves(a.helper, xsb):
            name = names.get(cue, "")
            rel = out_rel(name, bank, wave)
            if a.filter and not matches(rel, a.filter):
                continue
            rows.append({"name": name, "bank": bank, "index": wave, "category": category(name, bank), "file": rel})
            jobs.append((os.path.join(a.disc, "audio", bank + ".xwb"), wave, os.path.join(a.out, rel)))
    order = sorted(range(len(jobs)), key=lambda i: (jobs[i][0], jobs[i][1]))   # one bank at a time
    rows, jobs = [rows[i] for i in order], [jobs[i] for i in order]
    for d in {os.path.dirname(j[2]) for j in jobs}:
        os.makedirs(d, exist_ok=True)
    done, errors, total = [], 0, 0
    for row, job, res in zip(rows, jobs, decode(a.helper, jobs, a.j)):
        f = res.split("\t")
        if f[0] != "ok":
            errors += 1
            print(f"[audio] {row['file']}: {f[-1]}", file=sys.stderr)
            continue
        frames, ch, rate = int(f[1]), int(f[2]), int(f[3])
        row.update(duration=f"{frames / rate:.4f}" if rate else "0", channels=ch, rate=rate)
        total += 44 + frames * ch * 2
        done.append(row)
    os.makedirs(a.out, exist_ok=True)
    write_catalog(os.path.join(a.out, "catalog.csv"), done, merge=bool(a.filter))
    named = sum(1 for r in done if not r["file"].startswith("_unnamed/"))
    print(f"[audio] {len(done)} WAVs ({named} named, {len(done) - named} unnamed), {total / 2**30:.2f} GiB, "
          f"{errors} errors, {time.time() - t0:.1f} s -> {a.out}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
