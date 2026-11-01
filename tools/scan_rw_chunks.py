"""Scan files inside an Xbox disc image for RenderWare chunk headers.

A RenderWare stream chunk header is 12 bytes: type (u32), size (u32), library ID
stamp (u32). The stamp's high 16 bits encode the RW version, so matching known
stamps with known chunk types gives a cheap census of RW formats in the data.

Usage: python3 scan_rw_chunks.py <image.iso> [file-name-substring ...]
"""
import collections
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xdvdfs_extract import SECTOR, find_partition, walk  # noqa: E402

CHUNK_NAMES = {
    0x01: "Struct", 0x02: "String", 0x03: "Extension", 0x05: "Camera",
    0x06: "Texture", 0x07: "Material", 0x08: "MaterialList", 0x0B: "World",
    0x0E: "FrameList", 0x0F: "Geometry", 0x10: "Clump", 0x12: "Light",
    0x14: "Atomic", 0x15: "TextureNative", 0x16: "TexDictionary",
    0x1A: "GeometryList", 0x1B: "AnimAnimation", 0x2B: "UVAnimDict",
    0x116: "SkinPLG", 0x11E: "HAnimPLG", 0x120: "MatFXPLG",
}
# High 16 bits of library ID stamps for RW 3.1 .. 3.7.
STAMP_HIGH = re.compile(rb"(?s)(?=(.{4})(.{4})(.{2})(\x00\x08|[\x00-\x07][\x0c\x10\x14\x18\x1c]))")
CHUNK = 64 * 1024 * 1024


def scan(f, start, length, counts, stamps):
    pos = 0
    while pos < length:
        f.seek(start + pos)
        # Overlap by 11 bytes so headers across buffer boundaries are not lost.
        buf = f.read(min(CHUNK + 11, length - pos))
        for m in STAMP_HIGH.finditer(buf):
            ctype, size = struct.unpack("<II", m.group(1) + m.group(2))
            if ctype in CHUNK_NAMES and 0 < size < 64 * 1024 * 1024:
                stamp = struct.unpack("<I", m.group(3) + m.group(4))[0]
                counts[CHUNK_NAMES[ctype]] += 1
                stamps[f"{stamp >> 16:#06x}"] += 1
        pos += CHUNK


def main():
    image, filters = sys.argv[1], [s.lower() for s in sys.argv[2:]]
    with open(image, "rb") as f:
        base, root_sector, root_size = find_partition(f)
        for path, sector, length in walk(f, base, root_sector, root_size):
            if filters and not any(s in path.lower() for s in filters):
                continue
            counts, stamps = collections.Counter(), collections.Counter()
            scan(f, base + sector * SECTOR, length, counts, stamps)
            if counts:
                print(f"{path} ({length} bytes)")
                print("  chunks:", dict(counts.most_common(12)))
                print("  stamps:", dict(stamps.most_common(4)))


if __name__ == "__main__":
    main()
