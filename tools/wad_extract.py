"""Extract every file from The Warriors (Xbox) WAD archives.

XBoxWad.idx layout:
    header   {u32 numWads, u32 numFiles, u32 unknown(0x100000), u32 ofsWadTable, u32 ofsFileTable}
    wads     numWads  x {char name[32]; u32 handle}
    files    numFiles x {u32 loc; u32 size; u32 zero; u32 nameHash}
    wad index = loc >> 24, byte offset = (loc & 0xFFFFFF) << 10
nameHash = CRC32 of the lowercased path with backslashes, prefixed with "ee_files\".

The index stores no names, so names are recovered by hashing candidate strings
harvested from default.xbe and from the extracted payloads themselves. Files whose
name stays unknown are written as _unnamed/<hash>.bin.

Usage: python3 wad_extract.py <image.iso> <default.xbe> <output-dir>
"""
import csv
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdvdfs_extract import SECTOR, find_partition, walk  # noqa: E402

PREFIX = "ee_files\\"
CANDIDATE = re.compile(rb"[A-Za-z0-9_\-./\\ ]{3,200}")
TOKEN = re.compile(rb"[A-Za-z0-9_\-]{2,64}")
# Path templates found in default.xbe ("anims\%s", "sectors/%s_%d.xsec", ...).
# {t} is a harvested token, {n} a small integer.
TOKEN_TEMPLATES = [
    "anims\\{t}", "anims\\{t}.anm", "paks\\{t}.pak", "paks\\{t}", "sectors\\{t}.xlev",
    "{t}_sub.scn", "{t}.scn", "{t}.lua", "{t}.lev", "{t}.cnk", "{t}.txt",
    "levels\\{t}\\{t}.lua", "levels\\{t}\\{t}_objs.txt",
]
NUMBER_TEMPLATES = ["chars\\{n}", "objs\\{n}", "sectors\\{t}_{n}.xsec"]
MAX_NUMBER = 20000


def normalize(path):
    path = path.replace("/", "\\").lower().strip()
    for lead in ("d:\\", "ee_files\\", "\\"):
        if path.startswith(lead):
            path = path[len(lead):]
    return path


def wad_hash(path):
    return zlib.crc32((PREFIX + normalize(path)).encode("latin-1"))


def raw_hash(path):
    # "ee_files\..\levels\x" collapses to "levels\x", dropping the prefix.
    return zlib.crc32(normalize(path).encode("latin-1"))


def try_name(path, wanted, names):
    for h in (wad_hash(path), raw_hash(path)):
        if h in wanted and h not in names:
            names[h] = normalize(path)


def expand_templates(tokens, wanted, names):
    """Combine harvested tokens and integers with the engine's path templates."""
    for t in tokens:
        for template in TOKEN_TEMPLATES:
            try_name(template.format(t=t), wanted, names)
    for n in range(MAX_NUMBER):
        for template in NUMBER_TEMPLATES[:2]:
            try_name(template.format(n=n), wanted, names)
    # Sector files are numbered per level; level names come from recovered .xlev names.
    levels = {v[len("sectors\\"):-len(".xlev")] for v in names.values() if v.endswith(".xlev")}
    for level in levels | set(tokens):
        for n in range(256):
            try_name(NUMBER_TEMPLATES[2].format(t=level, n=n), wanted, names)


def read_disc_files(f):
    base, root_sector, root_size = find_partition(f)
    return base, {p.lstrip("/").lower(): (s, n) for p, s, n in walk(f, base, root_sector, root_size)}


def parse_index(data):
    num_wads, num_files, _, ofs_wads, ofs_files = struct.unpack_from("<5I", data)
    wads = [data[ofs_wads + i * 36:ofs_wads + i * 36 + 32].split(b"\0")[0].decode() for i in range(num_wads)]
    files = [struct.unpack_from("<4I", data, ofs_files + i * 16) for i in range(num_files)]
    return wads, files


def harvest(blob, wanted, names, tokens):
    """Hash every printable run (and its suffixes after path separators) in blob."""
    tokens.update(m.group().decode("latin-1").lower() for m in TOKEN.finditer(blob))
    for m in CANDIDATE.finditer(blob):
        text = m.group().decode("latin-1")
        variants = {text}
        # Paths are often embedded in longer strings; also try every suffix starting at a separator.
        for i, ch in enumerate(text):
            if ch in "\\/ ":
                variants.add(text[i + 1:])
        for v in variants:
            if len(v) >= 3:
                try_name(v, wanted, names)


def main():
    image, xbe_path, out_dir = sys.argv[1:4]
    with open(image, "rb") as f:
        base, disc = read_disc_files(f)

        def read(name, offset=0, size=None):
            sector, length = disc[name.lower()]
            f.seek(base + sector * SECTOR + offset)
            return f.read(length if size is None else size)

        wads, entries = parse_index(read("XBoxWad.idx"))
        wanted = {e[3] for e in entries}
        names, tokens = {}, set()

        with open(xbe_path, "rb") as xbe:
            harvest(xbe.read(), wanted, names, tokens)
        print(f"names from default.xbe: {len(names)}/{len(wanted)}")

        payloads = [read(wads[loc >> 24], (loc & 0xFFFFFF) << 10, size) for loc, size, _, _ in entries]
        for blob in payloads:
            harvest(blob, wanted, names, tokens)
        print(f"names after payload pass: {len(names)}/{len(wanted)} (tokens: {len(tokens)})")

        expand_templates(sorted(tokens), wanted, names)
        print(f"names after template pass: {len(names)}/{len(wanted)}")

        os.makedirs(out_dir, exist_ok=True)
        with open(os.path.join(out_dir, "manifest.csv"), "w", newline="") as mf:
            writer = csv.writer(mf)
            writer.writerow(["index", "hash", "wad", "offset", "size", "name"])
            for i, ((loc, size, _, h), blob) in enumerate(zip(entries, payloads)):
                name = names.get(h)
                rel = name.replace("\\", "/") if name else f"_unnamed/{h:08x}.bin"
                dest = os.path.join(out_dir, wads[loc >> 24], rel)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as o:
                    o.write(blob)
                writer.writerow([i, f"{h:08x}", wads[loc >> 24], (loc & 0xFFFFFF) << 10, size, name or ""])
        print(f"extracted {len(entries)} files to {out_dir}")


if __name__ == "__main__":
    main()
