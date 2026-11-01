"""Guest fs:[KPCR] instruction patching (runtime/src/fspatch.c), the Windows %fs strategy.

The unit test binary is game-independent. The census test compares the sites the patcher
finds in the title's code sections with a linear disassembly (objdump) of the same bytes;
it is skipped when the game XBE (not in git) or objdump is absent.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")
XBE = os.path.join(ROOT, "extracted", "default.xbe")

# Absolute fs:[disp] forms the patcher handles, as objdump prints them (AT&T syntax).
FS_RE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*(mov|movzbl|cmp|push|pop)\s+[^\s]*%fs:(0x[0-9a-f]+)(?:,|$)")


class FsPatchUnitTest(unittest.TestCase):
    def test_patched_instructions(self):
        subprocess.run(["make", "-s", "-C", RUNTIME, "build/fspatch_test"], check=True)
        res = subprocess.run([os.path.join(RUNTIME, "build", "fspatch_test")], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("all checks passed", res.stdout)


@unittest.skipUnless(os.path.exists(XBE), "game XBE not present")
@unittest.skipUnless(shutil.which("objdump"), "objdump not installed")
class FsPatchCensusTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", RUNTIME, "build/xbeinfo"], check=True)
        xbeinfo = os.path.join(RUNTIME, "build", "xbeinfo")
        out = subprocess.run([xbeinfo, "--fs-sites", XBE], check=True, capture_output=True, text=True).stdout
        cls.sites = {}
        for line in out.splitlines():
            name, addr, length, disp = line.partition("=")[2].split(",")
            cls.sites[int(addr, 16)] = (name, int(length), int(disp, 16))
        out = subprocess.run([xbeinfo, XBE], check=True, capture_output=True, text=True).stdout
        cls.sections = []
        for line in out.splitlines():
            if line.startswith("section="):
                name, vaddr, vsize, raw, rsize, flags = line.partition("=")[2].split(",")
                cls.sections.append((name, int(vaddr, 16), int(raw, 16), min(int(vsize, 16), int(rsize, 16))))
        cls.data = open(XBE, "rb").read()

    def objdump_sites(self, name, vaddr, raw, size):
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(self.data[raw:raw + size])
            f.flush()
            out = subprocess.run(["objdump", "-D", "-b", "binary", "-mi386", "--adjust-vma=%#x" % vaddr, f.name],
                                 check=True, capture_output=True, text=True).stdout
        found = {}
        for line in out.splitlines():
            m = FS_RE.match(line)
            if m and int(m.group(3), 16) < 0x300 and int(m.group(3), 16) % 4 == 0:
                found[int(m.group(1), 16)] = int(m.group(3), 16)
        return found

    def test_patcher_covers_the_disassembly(self):
        patched_sections = {s[0] for s in self.sites.values()}
        for name, vaddr, raw, size in self.sections:
            if name not in patched_sections:
                continue
            for addr, disp in self.objdump_sites(name, vaddr, raw, size).items():
                self.assertIn(addr, self.sites, "%s %08x fs:[%#x] not patched" % (name, addr, disp))
                self.assertEqual(self.sites[addr][2], disp)

    def test_census(self):
        # 512 sites in the linear disassembly plus KeGetCurrentIrql-style "movzx eax, byte fs:[0x24]; ret"
        # at 0x3d5dff in XACTENG, which objdump misses after a jump table.
        self.assertEqual(len(self.sites), 513)
        self.assertIn(0x3d5dff, self.sites)
        by_disp = {}
        for _, _, disp in self.sites.values():
            by_disp[disp] = by_disp.get(disp, 0) + 1
        self.assertEqual(sorted(by_disp), [0x0, 0x4, 0x20, 0x24, 0x28, 0x58])
        for addr, (name, length, _) in self.sites.items():
            self.assertNotIn(name, (".rdata", ".data", "DOLBY", "BINKDATA", "BINKBSS"))
            self.assertIn(length, (6, 7, 8))


if __name__ == "__main__":
    unittest.main()
