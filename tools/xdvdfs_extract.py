"""Minimal XDVDFS (original Xbox disc) reader.

Usage:
    python3 xdvdfs_extract.py <image.iso> list
    python3 xdvdfs_extract.py <image.iso> extract <path-in-image> <output-file>
    python3 xdvdfs_extract.py <image.iso> extract-all <output-dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Plain xiso images start the filesystem at 0; redump-style full images
# carry a video partition first and start it at 0x18300000.
CANDIDATE_OFFSETS = (0, 0x18300000, 0x2080000, 0xFD90000)
ATTR_DIRECTORY = 0x10


def find_partition(f):
    for base in CANDIDATE_OFFSETS:
        f.seek(base + 32 * SECTOR)
        header = f.read(36)
        if header[:20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", header, 20)
            return base, root_sector, root_size
    raise SystemExit("XDVDFS header not found")


def read_dir(f, base, sector, size):
    """Walk the directory's binary tree and return {name: (sector, size, attrs)}."""
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    entries = {}
    stack = [0]
    while stack:
        off = stack.pop()
        if off + 14 > len(table):
            continue
        left, right, start, length, attrs, name_len = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:
            continue
        name = table[off + 14:off + 14 + name_len].decode("latin-1")
        entries[name] = (start, length, attrs)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
    return entries


def walk(f, base, sector, size, prefix=""):
    for name, (start, length, attrs) in sorted(read_dir(f, base, sector, size).items()):
        path = f"{prefix}/{name}"
        if attrs & ATTR_DIRECTORY:
            if length:
                yield from walk(f, base, start, length, path)
        else:
            yield path, start, length


def copy_range(f, offset, length, out_path, chunk=8 << 20):
    f.seek(offset)
    with open(out_path, "wb") as o:
        remaining = length
        while remaining:
            data = f.read(min(chunk, remaining))
            if not data:
                raise SystemExit(f"short read for {out_path}")
            o.write(data)
            remaining -= len(data)


def extract_all(f, base, files, out_dir):
    """Write every file under out_dir, keeping the image's directory layout."""
    root = os.path.abspath(out_dir)
    total = 0
    for path, start, length in files:
        target = os.path.abspath(os.path.join(root, *path.strip("/").split("/")))
        if not target.startswith(root + os.sep):
            raise SystemExit(f"unsafe path in image: {path!r}")
        os.makedirs(os.path.dirname(target), exist_ok=True)
        if os.path.exists(target) and os.path.getsize(target) == length:
            continue  # resumable
        copy_range(f, base + start * SECTOR, length, target)
        total += length
    return total


def main():
    image, command = sys.argv[1], sys.argv[2]
    with open(image, "rb") as f:
        base, root_sector, root_size = find_partition(f)
        files = list(walk(f, base, root_sector, root_size))
        if command == "list":
            print(f"partition offset: {base:#x}, files: {len(files)}")
            for path, _, length in files:
                print(f"{length:>12}  {path}")
        elif command == "extract":
            wanted, out = sys.argv[3], sys.argv[4]
            match = next(((s, n) for p, s, n in files if p.lower() == wanted.lower()), None)
            if not match:
                raise SystemExit(f"not found: {wanted}")
            start, length = match
            f.seek(base + start * SECTOR)
            with open(out, "wb") as o:
                o.write(f.read(length))
            print(f"wrote {length} bytes to {out}")
        elif command == "extract-all":
            written = extract_all(f, base, files, sys.argv[3])
            print(f"extracted {len(files)} files ({written} bytes written) to {sys.argv[3]}")
        else:
            raise SystemExit(f"unknown command: {command}")


if __name__ == "__main__":
    main()
