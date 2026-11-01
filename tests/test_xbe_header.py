"""Checks the runtime's XBE decoding (runtime/build/xbeinfo) against known values.

Skipped when the game XBE (not in git) is absent.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XBE = os.path.join(ROOT, "extracted", "default.xbe")
XBEINFO = os.path.join(ROOT, "runtime", "build", "xbeinfo")


def run_xbeinfo():
    subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/xbeinfo"], check=True)
    out = subprocess.run([XBEINFO, XBE], check=True, capture_output=True, text=True).stdout
    info, sections, thunks = {}, [], []
    for line in out.splitlines():
        key, _, value = line.partition("=")
        if key == "section":
            name, vaddr, vsize, raw, rsize, flags = value.split(",")
            sections.append((name, int(vaddr, 16), int(vsize, 16), int(raw, 16), int(rsize, 16), int(flags, 16)))
        elif key == "thunk":
            thunks.append(int(value, 0))
        else:
            info[key] = value
    return info, sections, thunks


@unittest.skipUnless(os.path.exists(XBE), "game XBE not present")
class XbeHeaderTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.info, cls.sections, cls.thunks = run_xbeinfo()

    def in_image(self, addr):
        base = int(self.info["base"], 16)
        return base <= addr < base + int(self.info["image_size"], 16)

    def test_base_and_section_count(self):
        self.assertEqual(int(self.info["base"], 16), 0x10000)
        self.assertEqual(len(self.sections), 19)

    def test_text_section(self):
        text = [s for s in self.sections if s[0] == ".text"]
        self.assertEqual(len(text), 1)
        self.assertEqual(text[0][1], 0x11000)
        self.assertEqual(text[0][2], 0x34BEFC)

    def test_xdk_image_sections_present(self):
        names = {s[0] for s in self.sections}
        self.assertTrue({"$$XTIMAGE", "$$XSIMAGE"} <= names)

    def test_entry_and_thunk_decode_inside_image(self):
        self.assertEqual(self.info["key"], "retail")
        self.assertTrue(self.in_image(int(self.info["entry"], 16)))
        self.assertTrue(self.in_image(int(self.info["thunk_table"], 16)))

    def test_thunks_are_valid_ordinals(self):
        self.assertGreater(len(self.thunks), 50)
        self.assertEqual(len(set(self.thunks)), len(self.thunks))
        for ordinal in self.thunks:
            self.assertTrue(1 <= ordinal <= 378, ordinal)


if __name__ == "__main__":
    unittest.main()
