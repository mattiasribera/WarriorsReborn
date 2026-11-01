#!/usr/bin/env python3
"""Sets PE header flags zig's linker has no option for on a 32-bit Windows executable:
IMAGE_FILE_LARGE_ADDRESS_AWARE (a WOW64 process gets the 4 GB address space, needed for the
0x80000000 contiguous window) and IMAGE_FILE_RELOCS_STRIPPED (the loader must sit at its
fixed base 0x10000 or fail to start), and clears IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE.

usage: pe_flags.py FILE.exe [--check]   (--check only verifies the flags)
"""
import struct
import sys

LAA, RELOCS_STRIPPED, DYNAMIC_BASE = 0x0020, 0x0001, 0x0040


def main():
    path, check = sys.argv[1], "--check" in sys.argv[2:]
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if data[:2] != b"MZ":
            sys.exit("%s: not a PE file" % path)
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            sys.exit("%s: no PE signature" % path)
        chars_off = pe + 4 + 18                  # COFF header: Characteristics
        opt = pe + 4 + 20                        # optional header (PE32)
        if struct.unpack_from("<H", data, opt)[0] != 0x10B:
            sys.exit("%s: not PE32" % path)
        dllc_off = opt + 70                      # DllCharacteristics
        chars = struct.unpack_from("<H", data, chars_off)[0]
        dllc = struct.unpack_from("<H", data, dllc_off)[0]
        want_chars, want_dllc = chars | LAA | RELOCS_STRIPPED, dllc & ~DYNAMIC_BASE
        if check:
            ok = (chars, dllc) == (want_chars, want_dllc)
            print("%s: Characteristics %#06x DllCharacteristics %#06x %s" % (path, chars, dllc, "ok" if ok else "NOT SET"))
            sys.exit(0 if ok else 1)
        struct.pack_into("<H", data, chars_off, want_chars)
        struct.pack_into("<H", data, dllc_off, want_dllc)
        f.seek(0)
        f.write(data)


if __name__ == "__main__":
    main()
