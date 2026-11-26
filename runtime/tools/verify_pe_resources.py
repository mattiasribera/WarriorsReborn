#!/usr/bin/env python3
"""Small PE parser: print the VERSIONINFO strings of the built Windows PEs.

No installs, no external dependencies: struct + re only. Prints the entries of the
StringFileInfo blocks of each RT_VERSION resource (the 040904B0 unicode table).
Usage: python3 tools/verify_pe_resources.py FILE [FILE...]
"""
import re
import struct
import sys


def parse_sections(data):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nsections = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    opt_size = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    # PE32 optional header: data directories start at +96 of the optional header.
    sec_off = e_lfanew + 24 + opt_size
    sections = []
    for i in range(nsections):
        name = data[sec_off + i * 40:sec_off + i * 40 + 8].split(b"\0", 1)[0].decode(errors="replace")
        vsize, vaddr, size_raw, ptr_raw = struct.unpack_from("<IIII", data, sec_off + i * 40 + 8)
        sections.append((name, vaddr, vsize, ptr_raw, size_raw))
    return sections


def rva_to_offset(sections, rva):
    for name, vaddr, vsize, ptr_raw, size_raw in sections:
        if vaddr and vaddr <= rva < vaddr + max(vsize, size_raw):
            return ptr_raw + (rva - vaddr)
    return None


def resource_tree(data, sections, root_rva):
    """Yield (dir_offset, level) by walking from the resource root."""
    stack = [(root_rva, 0)]
    while stack:
        rva, level = stack.pop()
        off = rva_to_offset(sections, rva)
        if off is None or off + 16 > len(data):
            continue
        named = struct.unpack_from("<H", data, off + 12)[0]
        idents = struct.unpack_from("<H", data, off + 14)[0]
        for i in range(named + idents):
            name_id, child = struct.unpack_from("<II", data, off + 16 + i * 8)
            child_rva = rva + (child & 0x7FFFFFFF)
            child_off = rva_to_offset(sections, child_rva)
            if child_off is None:
                continue
            if child & 0x80000000:
                # Sub-directory: visit children on the next level.
                stack.append((child_rva, level + 1))
            else:
                data_rva, size = struct.unpack_from("<II", data, child_off)[:2]
                yield (child_off, level, data_rva, size)


def version_strings(data, sections, root_rva):
    out = []
    for _off, _level, data_rva, size in resource_tree(data, sections, root_rva):
        start = rva_to_offset(sections, data_rva)
        if start is None:
            continue
        blob = data[start:start + size]
        # VS_VERSIONINFO (the file-level block): key then value data.
        for m in re.finditer(rb"(?:[\x20-\x7e]\x00){2,40}\x00", blob):
            s = m.group(0).decode("utf-16-le", errors="replace").rstrip("\x00")
            if len(s) >= 2 and s[0].isalpha() and s not in ("END", "BEGIN"):
                out.append(s)
        # All UTF-16LE strings inside the block: VERSIONINFO values.
        for m in re.finditer(rb"(?:[\x20-\x7e]\x00){2,80}\x00", blob):
            s = m.group(0).decode("utf-16-le", errors="replace").rstrip("\x00")
            if s and len(s) <= 40 and all(c.isprintable() or c.isspace() for c in s):
                out.append(s)
    return out


def main():
    for path in sys.argv[1:]:
        with open(path, "rb") as f:
            data = f.read()
        if data[:2] != b"MZ":
            print(f"{path}: not a PE file")
            continue
        sections = parse_sections(data)
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        opt = e_lfanew + 24
        # RT_VERSION is resource directory index 2.
        root_rva, _size = struct.unpack_from("<II", data, opt + 96 + 2 * 8)
        strings = []
        if root_rva:
            strings = version_strings(data, sections, root_rva)
        print(f"== {path}")
        for s in strings:
            print(f"   {s}")


if __name__ == "__main__":
    main()
