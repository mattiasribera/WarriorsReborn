#!/usr/bin/env python3
"""Prints and checks what a Windows PE file is: machine, subsystem and the DLLs it imports
(the x64 helper must not pick up a 32-bit SDL2.dll or a runtime DLL a
player's Windows does not have).

usage: pe_check.py FILE... [--machine x86|x64] [--allow NAME,NAME,...]
  --machine  fail unless every FILE is that machine
  --allow    fail when a FILE imports a DLL not in the list (case-insensitive, fnmatch patterns
             such as api-ms-win-crt-*.dll)
Exit 0 when every check passed, 1 otherwise.
"""
import fnmatch
import struct
import sys

MACHINES = {0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64"}
SUBSYSTEMS = {2: "gui", 3: "console"}


def rva_to_off(sections, rva):
    for va, vsize, raw, rsize in sections:
        if va <= rva < va + max(vsize, rsize):
            return raw + rva - va
    return None


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def inspect(data):
    """Returns (machine, subsystem, [imported dll names]) of a PE image (bytes)."""
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE signature")
    machine, nsec = struct.unpack_from("<HH", data, pe + 4)
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic not in (0x10B, 0x20B):
        raise ValueError("unknown optional header %#x" % magic)
    subsystem = struct.unpack_from("<H", data, opt + 68)[0]
    dirs = opt + (96 if magic == 0x10B else 112)
    imp_rva = struct.unpack_from("<I", data, dirs + 8)[0]
    sec = opt + opt_size
    sections = []
    for i in range(nsec):
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, vsize, raw, rsize))
    dlls = []
    off = rva_to_off(sections, imp_rva) if imp_rva else None
    while off is not None:
        name_rva = struct.unpack_from("<I", data, off + 12)[0]
        if not name_rva:
            break
        dlls.append(cstr(data, rva_to_off(sections, name_rva)))
        off += 20
    return MACHINES.get(machine, hex(machine)), SUBSYSTEMS.get(subsystem, str(subsystem)), dlls


def main(argv):
    files, machine, allow = [], None, None
    i = 0
    while i < len(argv):
        if argv[i] == "--machine" and i + 1 < len(argv):
            machine = argv[i + 1]
            i += 2
        elif argv[i] == "--allow" and i + 1 < len(argv):
            allow = [a.strip().lower() for a in argv[i + 1].split(",") if a.strip()]
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    ok = True
    for path in files:
        try:
            with open(path, "rb") as f:
                m, sub, dlls = inspect(f.read())
        except (OSError, ValueError, struct.error) as e:
            print("%s: %s" % (path, e))
            ok = False
            continue
        bad = []
        if machine and m != machine:
            bad.append("machine %s, expected %s" % (m, machine))
        if allow is not None:
            extra = [d for d in dlls if not any(fnmatch.fnmatch(d.lower(), a) for a in allow)]
            if extra:
                bad.append("unexpected imports: " + " ".join(extra))
        print("%s: %s %s, imports %s%s" % (path, m, sub, " ".join(dlls) or "(none)", "; FAIL: " + "; ".join(bad) if bad else ""))
        ok = ok and not bad
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
