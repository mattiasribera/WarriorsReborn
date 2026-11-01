"""Static census of how title code in default.xbe uses Xbox Direct3D 8 (D3D8/D3DX8/XGRAPHC).

Finds every title -> D3D call site (direct `call rel32`, calls through `jmp rel32` thunks,
tail jumps, `call [abs]` through pointers that hold D3D addresses), counts them per entry
point, recovers `push imm` arguments (argument count taken from the callee's `ret N`, or
the caller's `add esp, N` for cdecl), finds title instructions that read or write D3D
library globals directly (render-state arrays, dirty flags, the device / push-buffer
pointers), and looks for vertex-shader microcode, shader declarations and texture headers
in .rdata/.data and in a sample of the extracted WAD files.

Instruction boundaries come from a linear sweep with `objdump -D -b binary -m i386`;
everything else is decoded here from the raw instruction bytes.

Usage: python3 tools/d3d_census.py <default.xbe> <default.syms>
           [--module-map ghidra/module_map.csv] [--wad-dir extracted/wad]
           [--wad-budget-mb 256] [--out-dir extracted]
Writes <out-dir>/d3d_inventory.csv and <out-dir>/d3d_inventory.md.
"""
import argparse
import bisect
import collections
import csv
import os
import re
import struct
import subprocess
import tempfile

Insn = collections.namedtuple("Insn", "addr raw text")

LIB_SECTIONS = ("D3D", "D3DX", "XGRPH")
LIB_PREFIXES = ("D3D8__", "XGRAPHC__", "D3DX8__")
# Xbox D3DFORMAT codes (bits 8-15 of the pixel-container Format dword), from the XDK
# d3d8types.h values as documented by Cxbx-Reloaded. Names are inferred, codes observed.
FORMATS = {
    0x00: "L8", 0x01: "AL8", 0x02: "A1R5G5B5", 0x03: "X1R5G5B5", 0x04: "A4R4G4B4",
    0x05: "R5G6B5", 0x06: "A8R8G8B8", 0x07: "X8R8G8B8", 0x0B: "P8", 0x0C: "DXT1",
    0x0E: "DXT3", 0x0F: "DXT5", 0x10: "LIN_A1R5G5B5", 0x11: "LIN_R5G6B5",
    0x12: "LIN_A8R8G8B8", 0x13: "LIN_L8", 0x16: "LIN_R8B8", 0x17: "LIN_G8B8", 0x19: "A8",
    0x1A: "A8L8", 0x1B: "LIN_AL8", 0x1C: "LIN_X1R5G5B5", 0x1D: "LIN_A4R4G4B4",
    0x1E: "LIN_X8R8G8B8", 0x1F: "LIN_A8", 0x20: "LIN_A8L8", 0x24: "YUY2", 0x25: "UYVY",
    0x27: "L6V5U5", 0x28: "V8U8", 0x29: "R8B8", 0x2A: "D24S8", 0x2B: "F24S8", 0x2C: "D16",
    0x2D: "F16", 0x2E: "LIN_D24S8", 0x2F: "LIN_F24S8", 0x30: "LIN_D16", 0x31: "LIN_F16",
    0x32: "L16", 0x33: "V16U16", 0x35: "LIN_L16", 0x36: "LIN_V16U16", 0x3A: "A8B8G8R8",
    0x3B: "B8G8R8A8", 0x3C: "R8G8B8A8", 0x3F: "LIN_A8B8G8R8", 0x40: "LIN_B8G8R8A8",
    0x41: "LIN_R8G8B8A8", 0x64: "VERTEXDATA",
}
# Index of the D3DFORMAT argument in public D3D8/XGraphics signatures (inferred).
FORMAT_ARG = {"D3DDevice_CreateTexture": 4, "XGSetTextureHeader": 4, "XGSetSurfaceHeader": 2,
              "D3DDevice_CreateCubeTexture": 3, "D3DDevice_CreateVolumeTexture": 5}
# [I] title code ORs bits here right before storing into D3D_g_RenderState (inline SetRenderState).
INFERRED_GLOBALS = {0x37D4D8: "D3D__DirtyFlags?[I]"}
VSH_TYPES = {0x2078: "program", 0x7778: "write", 0x7378: "state"}
# D3DVSDT_* vertex element types (values observed in declarations; names inferred from the XDK).
VSDT_TYPES = {0x02: "NONE", 0x11: "NORMSHORT1", 0x12: "FLOAT1", 0x14: "PBYTE1", 0x15: "SHORT1",
              0x16: "NORMPACKED3", 0x21: "NORMSHORT2", 0x22: "FLOAT2", 0x24: "PBYTE2", 0x25: "SHORT2",
              0x31: "NORMSHORT3", 0x32: "FLOAT3", 0x34: "PBYTE3", 0x35: "SHORT3", 0x40: "D3DCOLOR",
              0x41: "NORMSHORT4", 0x42: "FLOAT4", 0x44: "PBYTE4", 0x45: "SHORT4", 0x72: "FLOAT2H"}
PRIMS = {1: "POINTLIST", 2: "LINELIST", 3: "LINELOOP", 4: "LINESTRIP", 5: "TRIANGLELIST",
         6: "TRIANGLESTRIP", 7: "TRIANGLEFAN", 8: "QUADLIST", 9: "QUADSTRIP", 10: "POLYGON"}
SHADER_SOURCE = re.compile(rb"(?:x?vs|xvs[sw]|x?ps)\.1\.[01][ \t\r\n]")
SHADER_PB_APIS = ("CreateVertexShader", "SetVertexShader", "SelectVertexShader", "LoadVertexShader",
                  "LoadVertexShaderProgram", "RunVertexStateShader", "SetVertexShaderConstant",
                  "SetVertexShaderInput", "DeleteVertexShader", "SetShaderConstantMode",
                  "CreatePixelShader", "SetPixelShader", "SetPixelShaderProgram", "SetPixelShaderConstant",
                  "DeletePixelShader", "CreatePushBuffer", "BeginPushBuffer", "EndPushBuffer",
                  "RunPushBuffer", "BeginPush", "EndPush", "MakeSpace", "MakeRequestedSpace",
                  "KickPushBuffer", "GetPushBufferOffset", "SetPushBufferSize", "BeginState",
                  "XGCompileShader", "XGAssembleShader")
KEY_ARG_APIS = ("SetRenderState", "SetTextureState", "SetTextureStageState", "Create", "Shader",
                "XGSet", "DrawVertices", "DrawIndexedVertices", "SetTransform", "Clear", "SetTile")


# ---------------------------------------------------------------- instruction level

def parse_objdump(lines):
    pat = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{2}(?: [0-9a-f]{2})*)\s*\t(.*)$")
    out = []
    for line in lines:
        m = pat.match(line)
        if m and not m.group(3).startswith("(bad)"):
            out.append(Insn(int(m.group(1), 16), bytes.fromhex(m.group(2)), m.group(3).strip()))
    return out


def disassemble(code, vaddr):
    with tempfile.NamedTemporaryFile(suffix=".bin") as f:
        f.write(code)
        f.flush()
        cmd = ["objdump", "-D", "-b", "binary", "-m", "i386", "-M", "intel", "-w",
               "--insn-width=15", f"--adjust-vma={vaddr:#x}", f.name]
        proc = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return parse_objdump(proc.stdout.splitlines())


def insns_from_chunks(base, chunks):
    out, addr = [], base
    for raw in chunks:
        out.append(Insn(addr, raw, ""))
        addr += len(raw)
    return out


def branch(insn):
    """(kind, target) for call/jmp/jcc with a static target, else None."""
    r, end = insn.raw, insn.addr + len(insn.raw)
    if len(r) == 5 and r[0] in (0xE8, 0xE9):
        return ("call" if r[0] == 0xE8 else "jmp", (end + struct.unpack_from("<i", r, 1)[0]) & 0xFFFFFFFF)
    if len(r) == 6 and r[0] == 0xFF and r[1] in (0x15, 0x25):
        return ("calli" if r[1] == 0x15 else "jmpi", struct.unpack_from("<I", r, 2)[0])
    if len(r) == 2 and (0x70 <= r[0] <= 0x7F or r[0] == 0xEB):
        return ("jcc" if r[0] != 0xEB else "jmp", (end + struct.unpack_from("<b", r, 1)[0]) & 0xFFFFFFFF)
    if len(r) == 6 and r[0] == 0x0F and 0x80 <= r[1] <= 0x8F:
        return ("jcc", (end + struct.unpack_from("<i", r, 2)[0]) & 0xFFFFFFFF)
    return None


def is_ret(insn):
    return insn.raw[:1] in (b"\xc3", b"\xc2")


def recover_args(insns, idx, nargs, block_starts):
    """Immediate stack args (arg0 first, None = not an immediate) and register immediates."""
    args, regs, i = [], {}, idx - 1
    while i >= 0 and len(args) < nargs and idx - i <= 24:
        ins = insns[i]
        r = ins.raw
        if branch(ins) or is_ret(ins) or 0x58 <= r[0] <= 0x5F:
            break
        if r[0] == 0x68 and len(r) == 5:
            args.append(struct.unpack_from("<I", r, 1)[0])
        elif r[0] == 0x6A and len(r) == 2:
            args.append(struct.unpack_from("<b", r, 1)[0] & 0xFFFFFFFF)
        elif 0x50 <= r[0] <= 0x57 or (r[0] == 0xFF and len(r) > 1 and (r[1] >> 3) & 7 == 6):
            args.append(None)
        elif r[0] in (0xB8, 0xB9, 0xBA) and len(r) == 5:
            regs.setdefault(("eax", "ecx", "edx")[r[0] - 0xB8], struct.unpack_from("<I", r, 1)[0])
        if ins.addr in block_starts:
            break
        i -= 1
    return args + [None] * (nargs - len(args)), regs


def ret_arg_count(insns, start):
    for ins in insns[start:start + 4000]:
        if ins.raw[:1] == b"\xc3":
            return 0, "ret"
        if ins.raw[:1] == b"\xc2" and len(ins.raw) == 3:
            n = struct.unpack_from("<H", ins.raw, 1)[0]
            return n // 4, f"ret {n:#x}"
    return None, "?"


def caller_cleanup(insns, idx):
    if idx + 1 < len(insns):
        r = insns[idx + 1].raw
        if r[:2] == b"\x83\xc4" and len(r) == 3:
            return r[2] // 4
        if r[:2] == b"\x81\xc4" and len(r) == 6:
            return struct.unpack_from("<I", r, 2)[0] // 4
    return None


def find_call_sites(insns, is_target, deref=None):
    calls = {b[1] for b in map(branch, insns) if b and b[0] == "call"}
    thunks = {}
    for ins in insns:
        b = branch(ins)
        if b and b[0] == "jmp" and is_target(b[1]) and ins.addr in calls:
            thunks[ins.addr] = b[1]
    sites = []
    for idx, ins in enumerate(insns):
        b = branch(ins)
        if not b:
            continue
        kind, tgt = b
        if kind == "call" and is_target(tgt):
            sites.append({"index": idx, "caller": ins.addr, "kind": "call", "target": tgt})
        elif kind == "call" and tgt in thunks:
            sites.append({"index": idx, "caller": ins.addr, "kind": "thunk_call", "target": thunks[tgt]})
        elif kind == "jmp" and is_target(tgt) and ins.addr not in thunks and len(ins.raw) == 5:
            sites.append({"index": idx, "caller": ins.addr, "kind": "tailjmp", "target": tgt})
        elif kind in ("calli", "jmpi") and deref and is_target(deref(tgt) or 0):
            sites.append({"index": idx, "caller": ins.addr, "kind": "indirect", "target": deref(tgt)})
    return sites


def global_access(insn, lo, hi):
    """[(address, read|write|addr)] for absolute addresses in [lo, hi) inside one instruction."""
    if branch(insn):
        return []
    found = []
    for off in range(1, len(insn.raw) - 3):
        v = struct.unpack_from("<I", insn.raw, off)[0]
        if lo <= v < hi and all(v != f[0] for f in found):
            found.append((v, None))
    mnem, _, ops = insn.text.partition(" ")
    dest = ops.strip().split(",")[0]
    out = []
    for v, _ in found:
        if insn.text and f"{v:#x}" not in ops:
            continue  # byte window straddling two operands, not a real address
        if insn.text and ("PTR" not in ops and "ds:" not in ops or mnem == "lea"):
            mode = "addr"
        elif f"{v:#x}" in dest and mnem not in ("cmp", "test", "push") and "," in ops:
            mode = "write"
        else:
            mode = "read"
        out.append((v, mode))
    return out


# ---------------------------------------------------------------- data scans

def decode_format(fmt):
    code = (fmt >> 8) & 0xFF
    return {"dma": fmt & 3, "cube": bool(fmt & 4), "dims": (fmt >> 4) & 15, "code": code,
            "name": FORMATS.get(code, f"{code:#04x}"), "levels": (fmt >> 16) & 15,
            "width": 1 << ((fmt >> 20) & 15), "height": 1 << ((fmt >> 24) & 15)}


def scan_texture_headers(buf):
    """Format dwords of D3D texture pixel-container headers {Common, Data, Lock, Format, Size}."""
    out = []
    for m in re.finditer(rb"(?s)[\x00-\x0f]\x00\x04\x00.{4}\x00\x00\x00\x00[\x21-\x3f]", buf):
        fmt = struct.unpack_from("<I", buf, m.start() + 12)[0] if m.start() + 16 <= len(buf) else 0
        info = decode_format(fmt)
        if info["dma"] in (1, 2) and info["dims"] in (2, 3) and info["code"] in FORMATS:
            out.append(fmt)
    return out


def scan_vsh_programs(buf):
    """(offset, type, instruction count) of Xbox vertex shader microcode blobs (4-aligned)."""
    out = []
    for m in re.finditer(rb"(?s)\x78[\x20\x73\x77]..", buf):
        off = m.start()
        if off % 4:
            continue
        hdr = struct.unpack_from("<I", buf, off)[0]
        count = hdr >> 16
        if not 1 <= count <= 136 or off + 4 + count * 16 > len(buf):
            continue
        words = struct.unpack_from(f"<{count * 4}I", buf, off + 4)
        if all(words[i * 4] == 0 for i in range(count)) and any(words):
            out.append((off, VSH_TYPES[hdr & 0xFFFF], count))
    return out


def scan_vsh_declarations(buf):
    """(offset, register token count) of vertex shader declarations STREAM ... END."""
    out = []
    for m in re.finditer(rb"(?s)[\x00-\x0f]\x00\x00\x20", buf):
        off = m.start()
        if off % 4:
            continue
        regs, pos = 0, off + 4
        while pos + 4 <= len(buf) and pos - off < 256:
            t = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
            if t == 0xFFFFFFFF:
                if regs:
                    out.append((off, regs))
                break
            if is_reg_token(t):
                regs += 1
            elif not (t >> 28 == 5 or (t >> 28 == 2 and t & 0x0FFFFFFF < 16)):
                break
    return out


def is_reg_token(t):
    return t >> 28 == 4 and t & 0xFFFF < 16 and (t >> 16) & 0xFF in VSDT_TYPES


def stack_store(ins):
    """(base, displacement, imm32) for `mov dword [esp/ebp + disp], imm32`, else None."""
    r = ins.raw
    if r[:3] == b"\xc7\x44\x24" and len(r) == 8:
        return "esp", struct.unpack_from("<b", r, 3)[0], struct.unpack_from("<I", r, 4)[0]
    if r[:3] == b"\xc7\x84\x24" and len(r) == 11:
        return "esp", struct.unpack_from("<i", r, 3)[0], struct.unpack_from("<I", r, 7)[0]
    if r[:2] == b"\xc7\x45" and len(r) == 7:
        return "ebp", struct.unpack_from("<b", r, 2)[0], struct.unpack_from("<I", r, 3)[0]
    if r[:2] == b"\xc7\x85" and len(r) == 10:
        return "ebp", struct.unpack_from("<i", r, 2)[0], struct.unpack_from("<I", r, 6)[0]
    return None


def stack_declarations(insns):
    """Vertex shader declarations built on the stack: STREAM token stored at [base+d], REG/STREAM
    tokens at d+4, d+8, ... up to an END token, all stored by nearby `mov [esp/ebp+disp], imm32`.
    A slot without a nearby store is reported as "?" (one per gap, two consecutive gaps abort).

    Returns [(address of the STREAM store, ["v<reg>:<type>", ...])]."""
    out = []
    for i, ins in enumerate(insns):
        st = stack_store(ins)
        if not st or st[2] & 0xFFFFFFF0 != 0x20000000:
            continue
        slots = {}
        for j in sorted(range(max(0, i - 16), min(len(insns), i + 16)), key=lambda j: abs(j - i)):
            other = stack_store(insns[j])
            if other and other[0] == st[0]:
                slots.setdefault(other[1], other[2])
        if is_reg_token(slots.get(st[1] - 4, 0)):
            continue  # second stream inside a multi-stream declaration
        regs, disp, misses = [], st[1] + 4, 0
        while misses < 2 and len(regs) < 24 and slots.get(disp) != 0xFFFFFFFF:
            t = slots.get(disp)
            if t is None:  # slot stored elsewhere (other branch, or esp moved by a push)
                regs.append("?")
                misses += 1
            elif is_reg_token(t):
                regs.append(f"v{t & 0xFFFF}:{VSDT_TYPES[(t >> 16) & 0xFF]}")
                misses = 0
            elif t & 0xFFFFFFF0 == 0x20000000:
                regs.append(f"stream{t & 15}")
            else:
                break
            disp += 4
        if any(r.startswith("v") for r in regs) and slots.get(disp) == 0xFFFFFFFF:
            out.append((ins.addr, regs))
    return out


# ---------------------------------------------------------------- image / inputs

class Xbe:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        self.base = struct.unpack_from("<I", self.data, 0x104)[0]
        count, hdrs = struct.unpack_from("<II", self.data, 0x11C)
        self.sections = []
        for i in range(count):
            _, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", self.data, hdrs - self.base + i * 0x38)
            name_off = name_va - self.base
            name = self.data[name_off:self.data.index(b"\0", name_off)].decode()
            self.sections.append((name, va, vsize, raw, rsize))

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def section_of(self, va):
        return next((s[0] for s in self.sections if s[1] <= va < s[1] + s[2]), None)

    def raw(self, name):
        _, _, _, raw, rsize = self.section(name)
        return self.data[raw:raw + rsize]

    def dword(self, va):
        for _, sva, _, raw, rsize in self.sections:
            if sva <= va <= sva + rsize - 4:
                return struct.unpack_from("<I", self.data, raw + va - sva)[0]
        return None


def load_symbols(path):
    syms = collections.defaultdict(list)
    for line in open(path):
        name, _, addr = line.partition("=")
        if addr.strip():
            syms[int(addr, 16)].append(name.strip())
    return syms


def load_module_map(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path) as f:
        return {int(r["address"], 16): r["source_path"] for r in csv.DictReader(f)}


def short(names):
    names = sorted(names, key=lambda n: "_g_" not in n)
    return "/".join(n.split("__", 1)[-1] for n in names)


def label(addr, data_syms):
    keys = sorted(data_syms)
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0 or addr - keys[i] > 0x2000:
        return f"{addr:#x}"
    name = data_syms[keys[i]]
    return name if addr == keys[i] else f"{name}+{addr - keys[i]:#x}"


# ---------------------------------------------------------------- census

def census(args):
    xbe, syms = Xbe(args.xbe), load_symbols(args.syms)
    modules = load_module_map(args.module_map)
    lib_ranges = [(s[1], s[1] + s[2], s[0]) for s in xbe.sections if s[0] in LIB_SECTIONS]
    lib_syms = {a: n for a, n in syms.items() if any(x.startswith(LIB_PREFIXES) for x in n)}
    text = xbe.section(".text")

    def in_lib(a):
        return any(lo <= a < hi for lo, hi, _ in lib_ranges)

    def is_target(a):
        return in_lib(a) or a in lib_syms

    tinsns = disassemble(xbe.raw(".text"), text[1])
    linsns, lindex = [], {}
    for name in LIB_SECTIONS:
        sec = xbe.section(name)
        linsns_part = disassemble(xbe.raw(name), sec[1])
        lindex.update((ins.addr, len(linsns) + i) for i, ins in enumerate(linsns_part))
        linsns += linsns_part
    tindex = {ins.addr: i for i, ins in enumerate(tinsns)}
    block_starts = {b[1] for b in map(branch, tinsns) if b}
    starts = sorted({b[1] for b in map(branch, tinsns) if b and b[0] == "call" and text[1] <= b[1] < text[1] + text[2]}
                    | set(modules) | {a for a in syms if text[1] <= a < text[1] + text[2]})

    def func_of(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return starts[i] if i >= 0 else None

    mod_keys = sorted(modules)

    def module_of(func):
        if func in modules:
            return modules[func]
        i = bisect.bisect_right(mod_keys, func or 0) - 1
        return "~" + modules[mod_keys[i]] if i >= 0 and func - mod_keys[i] < 0x2000 else ""

    # library functions linked into .text (D3D8 wrappers, XAPILIB): extent = start .. first ret
    lib_extent = {}
    for a in syms:
        if a in tindex:
            for ins in tinsns[tindex[a]:tindex[a] + 4000]:
                if is_ret(ins):
                    lib_extent[a] = ins.addr + len(ins.raw)
                    break

    def lib_caller(func, addr):
        return func in lib_extent and addr < lib_extent[func]

    ret_cache = {}

    def callee_args(tgt):
        if tgt not in ret_cache:
            if tgt in lindex:
                ret_cache[tgt] = ret_arg_count(linsns, lindex[tgt])
            elif tgt in tindex:
                ret_cache[tgt] = ret_arg_count(tinsns, tindex[tgt])
            else:
                ret_cache[tgt] = (None, "?")
        return ret_cache[tgt]

    rows, excluded = [], collections.Counter()
    for s in find_call_sites(tinsns, is_target, xbe.dword):
        func = func_of(s["caller"])
        if lib_caller(func, s["caller"]):
            excluded[short(syms[func])] += 1
            continue
        nargs, how = callee_args(s["target"])
        if not nargs:
            cleanup = caller_cleanup(tinsns, s["index"])
            if cleanup:
                nargs, how = cleanup, f"add esp {cleanup * 4:#x}"
        sargs, regs = recover_args(tinsns, s["index"], nargs or 0, block_starts)
        s.update(func=func, module=module_of(func), nargs=nargs, how=how, args=sargs, regs=regs,
                 name=short(lib_syms.get(s["target"], [])), section=xbe.section_of(s["target"]))
        rows.append(s)
    # XGRPH also holds non-XGraphics library code (destructors, allocator wrappers): keep its
    # unnamed targets out of the D3D totals.
    other = [r for r in rows if r["section"] == "XGRPH" and not r["name"]]
    rows = [r for r in rows if not (r["section"] == "XGRPH" and not r["name"])]

    # direct access to library globals (data part of the library sections)
    data_syms = {a: short(n) for a, n in lib_syms.items() if any("_g_" in x or "D3DRS_" in x for x in n)}
    lo = min(data_syms) & ~0xFFF if data_syms else 0
    hi = next(h for _, h, n in lib_ranges if n == "D3D")
    simple = next((a for a, n in syms.items() if "D3D8__D3DDevice_SetRenderState_Simple" in n), None)
    put = xbe.dword(simple + 1) if simple and xbe.dword(simple) & 0xFF == 0xA1 else None
    if put and lo <= put < hi:
        data_syms[put] = "Pusher.m_pPut[observed@SetRenderState_Simple]"
        data_syms[put + 4] = "Pusher.m_pThreshold[observed@SetRenderState_Simple]"
    for addr, name in INFERRED_GLOBALS.items():
        data_syms.setdefault(addr, name)
    accesses = []
    for ins in tinsns:
        for addr, mode in global_access(ins, lo, hi):
            if addr not in lib_syms or addr in data_syms:
                func = func_of(ins.addr)
                if lib_caller(func, ins.addr):
                    continue
                accesses.append({"caller": ins.addr, "func": func, "addr": addr, "mode": mode,
                                 "label": label(addr, data_syms), "module": module_of(func), "text": ins.text})

    # function pointers to library code in initialized data
    fnptrs = collections.Counter()
    for sec in (".rdata", ".data"):
        raw = xbe.raw(sec)
        for off in range(0, len(raw) - 3, 4):
            v = struct.unpack_from("<I", raw, off)[0]
            if v in lib_syms and not (lo <= v < hi):
                fnptrs[(sec, short(lib_syms[v]))] += 1
    data_blob = xbe.raw(".rdata") + xbe.raw(".data")
    statics = {"vsh": scan_vsh_programs(data_blob), "decl": scan_vsh_declarations(data_blob),
               "stack_decl": stack_declarations(tinsns),
               "sources": [m.group(0).strip().decode() for m in SHADER_SOURCE.finditer(xbe.data)],
               "assembler": b"Expected a version statement" in xbe.data}
    wad = scan_wads(args.wad_dir, args.wad_budget_mb * 1024 * 1024)
    return rows, other, accesses, fnptrs, statics, wad, excluded, (lo, hi, put)


def scan_wads(wad_dir, budget):
    manifest = os.path.join(wad_dir or "", "manifest.csv")
    if not wad_dir or not os.path.exists(manifest):
        return None
    with open(manifest) as f:
        entries = [r for r in csv.DictReader(f)]
    total = sum(int(r["size"]) for r in entries)
    stride = max(1, round(total / budget)) if budget else 1
    res = {"files": 0, "bytes": 0, "tex": collections.Counter(), "vsh": 0, "decl": 0, "files_with_vsh": []}
    for r in entries[::stride]:
        name = r["name"] or os.path.join("_unnamed", r["hash"] + ".bin")
        path = os.path.join(wad_dir, r["wad"], *name.replace("\\", "/").split("/"))
        if not os.path.exists(path):
            continue
        buf = open(path, "rb").read()
        res["files"] += 1
        res["bytes"] += len(buf)
        for fmt in scan_texture_headers(buf):
            res["tex"][decode_format(fmt)["name"]] += 1
        vsh = scan_vsh_programs(buf)
        res["vsh"] += len(vsh)
        res["decl"] += len(scan_vsh_declarations(buf))
        if vsh:
            res["files_with_vsh"].append(name)
    res["total_files"], res["total_bytes"] = len(entries), total
    return res


# ---------------------------------------------------------------- output

def fmt_args(args, regs):
    a = ",".join("?" if v is None else f"{v:#x}" for v in args)
    return a + "".join(f" {k}={v:#x}" for k, v in sorted(regs.items()))


def write_outputs(out_dir, rows, other, accesses, fnptrs, statics, wad, excluded, ranges):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "d3d_inventory.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["kind", "site", "caller_func", "module", "target", "target_name", "section", "nargs", "args"])
        for r in rows:
            w.writerow([r["kind"], f"{r['caller']:#x}", f"{r['func']:#x}" if r["func"] else "", r["module"],
                        f"{r['target']:#x}", r["name"], r["section"], r["nargs"], fmt_args(r["args"], r["regs"])])
        for a in accesses:
            w.writerow([f"global_{a['mode']}", f"{a['caller']:#x}", f"{a['func']:#x}" if a["func"] else "",
                        a["module"], f"{a['addr']:#x}", a["label"], "", "", a["text"]])
    by_target = collections.defaultdict(list)
    for r in rows:
        by_target[r["target"]].append(r)
    named = {t: rs for t, rs in by_target.items() if rs[0]["name"]}
    md = ["# Direct3D 8 usage inventory (generated by tools/d3d_census.py)", "",
          "Tags: **[O]** observed directly in the bytes; **[I]** inferred (interpretation, "
          "signature knowledge, heuristics).", "", "## Summary", ""]
    kinds = collections.Counter(r["kind"] for r in rows)
    md += [f"- [O] Title -> D3D8/D3DX8/XGRAPHC call sites: **{len(rows)}** to **{len(by_target)}** distinct "
           f"entry points ({len(named)} named, {len(by_target) - len(named)} unnamed). Kinds: "
           + ", ".join(f"{k} {v}" for k, v in kinds.most_common()) + ".",
           f"- [O] Calls from title-resident library functions (symbols in .text) excluded: "
           f"{sum(excluded.values())} ({', '.join(f'{k} {v}' for k, v in excluded.most_common(8)) or 'none'}).",
           f"- [O] Title instructions referencing library globals in [{ranges[0]:#x}, {ranges[1]:#x}): "
           f"**{len(accesses)}** (" + ", ".join(f"{k} {v}" for k, v in
                                                collections.Counter(a["mode"] for a in accesses).most_common())
           + f") in {len({a['func'] for a in accesses})} functions.",
           f"- [O] Push-buffer put pointer (from D3DDevice_SetRenderState_Simple): "
           f"{ranges[2]:#x}; title references to it: "
           f"{sum(1 for a in accesses if ranges[2] and a['addr'] in (ranges[2], ranges[2] + 4))}." if ranges[2]
           else "- [O] Push-buffer put pointer not located.", ""]
    md += ["## Entry points called by title code [O]", "",
           "Arg count from the callee's `ret N` (stdcall) or the caller's `add esp` (cdecl); "
           "`ret` = no stack args (register or no-arg convention).", "",
           "| target | name | section | sites | kinds | conv | funcs | sample sites |", "|---|---|---|---|---|---|---|---|"]
    for t, rs in sorted(by_target.items(), key=lambda kv: -len(kv[1])):
        k = collections.Counter(r["kind"] for r in rs)
        md.append(f"| {t:#x} | {rs[0]['name'] or '?'} | {rs[0]['section']} | {len(rs)} | "
                  + " ".join(f"{a}:{b}" for a, b in k.items()) + f" | {rs[0]['how']} | "
                  f"{len({r['func'] for r in rs})} | " + " ".join(f"{r['caller']:#x}" for r in rs[:4]) + " |")
    mods = collections.Counter(r["module"] or "(unknown)" for r in rows)
    md += ["", "Call sites by caller source module ([O] exact module_map hit, [I] `~` = nearest preceding "
           "mapped function within 0x2000 bytes):", ""]
    md += [f"- {m}: {n}" for m, n in mods.most_common(15)]
    oth = collections.Counter(r["target"] for r in other)
    md += ["", "## Unnamed XGRPH targets (not counted above)", "",
           f"[O] {len(other)} sites to {len(oth)} unnamed XGRPH addresses; [I] the XGRPH section also holds "
           "non-graphics library code (e.g. one-instruction destructors, XMemFree wrappers), and most sites are "
           "C++ EH unwind funclets at the end of .text.", "",
           "- " + ", ".join(f"{t:#x} x{n}" for t, n in oth.most_common(20))]
    md += ["", "## Direct access to D3D library globals from title code", "",
           "[O] instructions with an absolute address inside the library data range; read/write from the "
           "operand position. Labels are nearest preceding symbol + offset ([I] for field meaning).", "",
           "| address | label | reads | writes | addr-of | funcs | sample sites |", "|---|---|---|---|---|---|---|"]
    by_addr = collections.defaultdict(list)
    for a in accesses:
        by_addr[a["addr"]].append(a)
    for addr, acc in sorted(by_addr.items(), key=lambda kv: -len(kv[1]))[:40]:
        m = collections.Counter(a["mode"] for a in acc)
        md.append(f"| {addr:#x} | {acc[0]['label']} | {m['read']} | {m['write']} | {m['addr']} | "
                  f"{len({a['func'] for a in acc})} | " + " ".join(f"{a['caller']:#x}" for a in acc[:4]) + " |")
    funcs = collections.Counter((a["func"], a["module"]) for a in accesses)
    md += ["", "Top functions touching library globals [O]:", ""]
    md += [f"- {f:#x} {m or '(no module)'}: {n}" for (f, m), n in funcs.most_common(12) if f]
    md += ["", "## Function pointers to library code in .rdata/.data [O]", ""]
    md += [f"- {sec} {name}: {n}" for (sec, name), n in fnptrs.most_common()] or ["- none"]
    md += ["", "## Shaders and push buffers", "", "| API | title sites [O] |", "|---|---|"]
    for api in SHADER_PB_APIS:
        hits = [(rs[0]["name"], len(rs)) for rs in named.values() if api in rs[0]["name"]]
        md.append(f"| {api} | " + (", ".join(f"{n} {c}" for n, c in hits) or "0") + " |")
    vsh = collections.Counter(t for _, t, _ in statics["vsh"])
    md += ["", f"- [O/I] Vertex shader microcode blobs in .rdata/.data (header 0x2078/0x7778/0x7378 + "
           f"zero first dword per instruction; heuristic): {len(statics['vsh'])} {dict(vsh)}, "
           f"instructions {sum(c for _, _, c in statics['vsh'])}.",
           f"- [O/I] Vertex shader declarations (STREAM .. REG .. END tokens): {len(statics['decl'])}."]
    src = collections.Counter(statics["sources"])
    md += [f"- [O] Shader assembly source texts in the XBE (version token such as xvs.1.1): {len(statics['sources'])} "
           f"{dict(src)}; shader assembler error strings linked in: {statics['assembler']}.",
           f"- [O] Vertex declarations built on the stack in title code (`mov [mem], imm32` STREAM/REG/END tokens; "
           "slot values come from the nearest store, so other branches may differ; "
           f"type names [I]): {len(statics['stack_decl'])}"]
    md += [f"  - {a:#x}: {' '.join(regs)}" for a, regs in statics["stack_decl"][:20]]
    if wad:
        md += [f"- [O/I] WAD sample: {wad['files']} of {wad['total_files']} files, "
               f"{wad['bytes'] >> 20} of {wad['total_bytes'] >> 20} MiB: vsh blobs {wad['vsh']}, "
               f"declarations {wad['decl']}; files with vsh: {', '.join(wad['files_with_vsh'][:8]) or 'none'}."]
    md += ["", "## Recovered immediate arguments (key APIs)", "",
           "[O] values of `push imm` / `mov reg, imm` before the call (arg0 first, `?` = not an immediate). "
           "Meaning of the values per public D3D8 signatures is [I].", ""]
    for t, rs in sorted(named.items(), key=lambda kv: kv[1][0]["name"]):
        name = rs[0]["name"]
        if not any(k in name for k in KEY_ARG_APIS):
            continue
        vec = collections.Counter(fmt_args(r["args"], r["regs"]) for r in rs)
        if "Draw" in name:
            prims = collections.Counter(PRIMS.get(r["args"][0], "?") for r in rs if r["args"])
            md.append(f"- **{name}** primitive types (arg0 [I]): {dict(prims)}")
        if any(v.strip(",?") for v in vec):
            md.append(f"- **{name}** ({rs[0]['how']}): " + "; ".join(f"`{v or '-'}` x{c}" for v, c in vec.most_common(12)))
    md += ["", "## Texture formats", ""]
    for t, rs in named.items():
        idx = FORMAT_ARG.get(rs[0]["name"])
        if idx is not None:
            fm = collections.Counter(FORMATS.get(r["args"][idx], f"{r['args'][idx]:#x}") if idx < len(r["args"])
                                     and r["args"][idx] is not None else "?" for r in rs)
            md.append(f"- [O values, I arg position] {rs[0]['name']} format arg: {dict(fm)}")
    if wad:
        md.append(f"- [O/I] Texture headers in WAD sample (Common type=texture, Lock=0, valid Format dword): "
                  f"{sum(wad['tex'].values())} {dict(wad['tex'].most_common())}")
    if excluded:
        md += ["", "## Notes", "", "- [O] Library functions linked into .text (calls from them are not title calls): "
               + ", ".join(excluded)]
    with open(os.path.join(out_dir, "d3d_inventory.md"), "w") as f:
        f.write("\n".join(md) + "\n")
    return by_target


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("xbe")
    p.add_argument("syms")
    p.add_argument("--module-map", default=os.path.join(root, "ghidra", "module_map.csv"))
    p.add_argument("--wad-dir", default=os.path.join(root, "extracted", "wad"))
    p.add_argument("--wad-budget-mb", type=int, default=256)
    p.add_argument("--out-dir", default=os.path.join(root, "extracted"))
    args = p.parse_args()
    rows, other, accesses, fnptrs, statics, wad, excluded, ranges = census(args)
    by_target = write_outputs(args.out_dir, rows, other, accesses, fnptrs, statics, wad, excluded, ranges)
    print(f"{len(rows)} call sites, {len(by_target)} entry points, {len(accesses)} global accesses -> "
          f"{os.path.join(args.out_dir, 'd3d_inventory.md')}")


if __name__ == "__main__":
    main()
