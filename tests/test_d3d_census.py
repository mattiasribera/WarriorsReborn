"""Game-independent tests for tools/d3d_census.py on synthetic byte buffers."""
import os
import shutil
import struct
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import d3d_census as dc  # noqa: E402

LIB = 0x400000  # synthetic library entry point


def call(src, dst):
    return b"\xe8" + struct.pack("<i", dst - (src + 5))


def jmp(src, dst):
    return b"\xe9" + struct.pack("<i", dst - (src + 5))


class CallSiteTest(unittest.TestCase):
    def test_branch_decoding(self):
        insn = dc.Insn(0x1000, call(0x1000, LIB), "")
        self.assertEqual(dc.branch(insn), ("call", LIB))
        self.assertEqual(dc.branch(dc.Insn(0x1000, jmp(0x1000, LIB), "")), ("jmp", LIB))
        self.assertEqual(dc.branch(dc.Insn(0x1000, b"\xff\x15\x78\x56\x34\x12", "")), ("calli", 0x12345678))
        self.assertIsNone(dc.branch(dc.Insn(0x1000, b"\x90", "")))

    def test_direct_call_and_thunk(self):
        # 0x1000: call LIB; 0x1005: call thunk; 0x100a: ret; 0x100b: thunk = jmp LIB
        insns = dc.insns_from_chunks(0x1000, [call(0x1000, LIB), call(0x1005, 0x100b), b"\xc3", jmp(0x100b, LIB)])
        sites = dc.find_call_sites(insns, lambda a: a == LIB)
        got = sorted((s["caller"], s["kind"], s["target"]) for s in sites)
        self.assertEqual(got, [(0x1000, "call", LIB), (0x1005, "thunk_call", LIB)])

    def test_tail_jump_counts_when_not_thunk(self):
        insns = dc.insns_from_chunks(0x2000, [b"\x90", jmp(0x2001, LIB)])
        sites = dc.find_call_sites(insns, lambda a: a == LIB)
        self.assertEqual([(s["caller"], s["kind"]) for s in sites], [(0x2001, "tailjmp")])


class ArgumentTest(unittest.TestCase):
    def test_push_immediates_in_argument_order(self):
        # push 5 ; push 0x89 ; mov ecx, 0x11 ; call LIB  ->  arg0 = 0x89, arg1 = 5
        chunks = [b"\x6a\x05", b"\x68\x89\x00\x00\x00", b"\xb9\x11\x00\x00\x00", call(0x100c, LIB)]
        insns = dc.insns_from_chunks(0x1000, chunks)
        args, regs = dc.recover_args(insns, 3, 2, set())
        self.assertEqual(args, [0x89, 5])
        self.assertEqual(regs, {"ecx": 0x11})

    def test_non_immediate_push_and_block_start(self):
        # push 7 (separate block) | push eax ; call LIB
        insns = dc.insns_from_chunks(0x1000, [b"\x6a\x07", b"\x50", call(0x1003, LIB)])
        args, _ = dc.recover_args(insns, 2, 2, {0x1002})
        self.assertEqual(args, [None, None])
        args, _ = dc.recover_args(insns, 2, 2, set())
        self.assertEqual(args, [None, 7])

    def test_negative_imm8_is_sign_extended(self):
        insns = dc.insns_from_chunks(0x1000, [b"\x6a\xff", call(0x1002, LIB)])
        self.assertEqual(dc.recover_args(insns, 1, 1, set())[0], [0xFFFFFFFF])

    def test_callee_arg_count_from_ret(self):
        stdcall = dc.insns_from_chunks(LIB, [b"\x8b\x44\x24\x04", b"\xc2\x08\x00"])
        self.assertEqual(dc.ret_arg_count(stdcall, 0), (2, "ret 0x8"))
        plain = dc.insns_from_chunks(LIB, [b"\x90", b"\xc3"])
        self.assertEqual(dc.ret_arg_count(plain, 0), (0, "ret"))

    def test_cdecl_cleanup_after_call(self):
        insns = dc.insns_from_chunks(0x1000, [call(0x1000, LIB), b"\x83\xc4\x0c"])
        self.assertEqual(dc.caller_cleanup(insns, 0), 3)


class GlobalAccessTest(unittest.TestCase):
    def test_read_and_write_classification(self):
        write = dc.Insn(0x1000, bytes.fromhex("890dd8d43700"), "mov    DWORD PTR ds:0x37d4d8,ecx")
        read = dc.Insn(0x1006, bytes.fromhex("8b0dd8d43700"), "mov    ecx,DWORD PTR ds:0x37d4d8")
        moffs = dc.Insn(0x100c, bytes.fromhex("a380d93700"), "mov    ds:0x37d980,eax")
        rng = (0x37d000, 0x380000)
        self.assertEqual(dc.global_access(write, *rng), [(0x37d4d8, "write")])
        self.assertEqual(dc.global_access(read, *rng), [(0x37d4d8, "read")])
        self.assertEqual(dc.global_access(moffs, *rng), [(0x37d980, "write")])
        self.assertEqual(dc.global_access(read, 0x100, 0x200), [])

    def test_parse_objdump_line(self):
        lines = ["   741e2:\t89 0d d8 d4 37 00    \tmov    DWORD PTR ds:0x37d4d8,ecx", "junk", "   741e8:\t(bad)"]
        insns = dc.parse_objdump(lines)
        self.assertEqual(insns[0], dc.Insn(0x741E2, bytes.fromhex("890dd8d43700"), "mov    DWORD PTR ds:0x37d4d8,ecx"))
        self.assertEqual(len(insns), 1)

    @unittest.skipUnless(shutil.which("objdump"), "objdump not installed")
    def test_disassemble_roundtrip(self):
        code = b"\x6a\x05" + call(0x1002, LIB) + b"\xc3"
        insns = dc.disassemble(code, 0x1000)
        self.assertEqual([i.addr for i in insns], [0x1000, 0x1002, 0x1007])
        self.assertEqual(dc.branch(insns[1]), ("call", LIB))


class DataScanTest(unittest.TestCase):
    def test_format_dword_decoding(self):
        info = dc.decode_format(0x06760F29)
        self.assertEqual((info["name"], info["levels"], info["width"], info["height"]), ("DXT5", 6, 128, 64))
        self.assertEqual(info["dims"], 2)
        self.assertEqual(dc.decode_format(0x00001229)["name"], "LIN_A8R8G8B8")
        self.assertEqual(dc.decode_format(0x0000FF29)["name"], "0xff")

    def test_texture_header_scan(self):
        hdr = struct.pack("<5I", 0x00040001, 0, 0, 0x06670C29, 0)
        buf = b"\x11" * 8 + hdr + b"\x00" * 16 + struct.pack("<5I", 0x00040001, 0, 0, 0x12345678, 0)
        self.assertEqual(dc.scan_texture_headers(buf), [0x06670C29])

    def test_vertex_shader_program_scan(self):
        prog = struct.pack("<I", (2 << 16) | 0x2078) + struct.pack("<8I", 0, 1, 2, 3, 0, 4, 5, 6)
        bad = struct.pack("<I", (2 << 16) | 0x2078) + struct.pack("<8I", 9, 1, 2, 3, 0, 4, 5, 6)
        found = dc.scan_vsh_programs(b"\xaa" * 4 + prog + bad)
        self.assertEqual(found, [(4, "program", 2)])

    def test_stack_built_declaration(self):
        def store(off, val):  # mov dword [esp+off], imm32
            return b"\xc7\x44\x24" + bytes([off]) + struct.pack("<I", val)
        chunks = [store(0x20, 0xFFFFFFFF), store(0x1c, 0x40220004), store(0x18, 0x40400002),
                  store(0x14, 0x40320000), store(0x10, 0x20000000), call(0x1028, LIB)]
        insns = dc.insns_from_chunks(0x1000, chunks)
        self.assertEqual(dc.stack_declarations(insns), [(0x1020, ["v0:FLOAT3", "v2:D3DCOLOR", "v4:FLOAT2"])])
        self.assertEqual(dc.stack_declarations(insns[1:]), [])  # no END token
        gap = dc.insns_from_chunks(0x1000, [store(0x24, 0xFFFFFFFF), store(0x14, 0x40320000), store(0x10, 0x20000000)])
        self.assertEqual(dc.stack_declarations(gap), [])  # END too far away
        one = dc.insns_from_chunks(0x1000, [store(0x1c, 0xFFFFFFFF), store(0x14, 0x40320000), store(0x10, 0x20000000)])
        self.assertEqual(dc.stack_declarations(one), [(0x1010, ["v0:FLOAT3", "?"])])

    def test_vertex_shader_declaration_scan(self):
        decl = struct.pack("<5I", 0x20000000, 0x40020000, 0x40420003, 0x40020001, 0xFFFFFFFF)
        self.assertEqual(dc.scan_vsh_declarations(b"\x00" * 4 + decl), [(4, 3)])
        self.assertEqual(dc.scan_vsh_declarations(struct.pack("<2I", 0x20000000, 0xFFFFFFFF)), [])


if __name__ == "__main__":
    unittest.main()
