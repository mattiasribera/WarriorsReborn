"""Game-independent test of tools/texture_review.py: builds the review page from a synthetic
dump and pack (tiny PNGs written here) and checks the embedded data and the copied images.
"""
import csv
import json
import os
import re
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import texture_review as tr  # noqa: E402


def tiny_png(path, w, h):
    raw = b"".join(b"\0" + b"\x80" * (w * 4) for _ in range(h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class TextureReviewTest(unittest.TestCase):
    def test_page(self):
        with tempfile.TemporaryDirectory() as d:
            dump, pack, out = (os.path.join(d, x) for x in ("dump", "pack/m", "review"))
            os.makedirs(dump)
            os.makedirs(pack)
            with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
                w.writerow(["00000000000000a1", "0x0F", 4, 2, 1, 0, 1, 5, 10])
                w.writerow(["00000000000000a2", "0x0C", 4, 4, 1, 0, 0, 9, 11])
                w.writerow(["00000000000000a3", "0x0C", 2, 2, 1, 0, 0, 1, 12])
            for h, tw, th in (("a1", 4, 2), ("a2", 4, 4), ("a3", 2, 2)):
                tiny_png(os.path.join(dump, "00000000000000%s.png" % h), tw, th)
            tiny_png(os.path.join(pack, "00000000000000a1.png"), 16, 8)
            tiny_png(os.path.join(pack, "00000000000000a2.png"), 16, 16)
            with open(os.path.join(pack, "selection.csv"), "w") as f:
                f.write("hash,format,w,h,status\n00000000000000a3,0x0C,2,2,skipped:small\n")
            self.assertEqual(tr.build(dump, pack, out), 2)
            page = open(os.path.join(out, "index.html")).read()
            data = json.loads(re.search(r'<script id="data" type="application/json">(.*?)</script>', page, re.S).group(1))
            self.assertEqual(data["packs"], ["m"])
            self.assertEqual([t["hash"] for t in data["textures"]], ["00000000000000a2", "00000000000000a1"])   # by draws
            self.assertEqual((data["textures"][1]["ups"][0]["uw"], data["textures"][1]["ups"][0]["uh"]), (16, 8))
            self.assertEqual(data["skipped"][0]["status"], "skipped:small")
            for t in data["textures"]:
                self.assertTrue(os.path.exists(os.path.join(out, t["orig"])))
                self.assertTrue(os.path.exists(os.path.join(out, t["ups"][0]["src"])))
            self.assertNotIn("http", page.replace("http-equiv", ""))   # self-contained, nothing remote

    def test_several_packs(self):
        with tempfile.TemporaryDirectory() as d:
            dump, out, root = os.path.join(d, "dump"), os.path.join(d, "review"), os.path.join(d, "packs")
            v1, v2 = os.path.join(root, "realesrgan-x4plus"), os.path.join(root, "1x-DEDXT+4x-GameAI-2-0")
            for p in (dump, v1, v2, os.path.join(root, ".work-x"), os.path.join(root, "empty")):
                os.makedirs(p)
            with open(os.path.join(dump, "manifest.csv"), "w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["hash", "format", "w", "h", "levels", "linear", "has_alpha", "draws", "first_frame"])
                w.writerow(["00000000000000b1", "0x0C", 4, 4, 1, 0, 0, 3, 1])
                w.writerow(["00000000000000b2", "0x0C", 4, 4, 1, 0, 0, 7, 1])
            for h in ("b1", "b2"):
                tiny_png(os.path.join(dump, "00000000000000%s.png" % h), 4, 4)
                tiny_png(os.path.join(v2, "00000000000000%s.png" % h), 16, 16)
            tiny_png(os.path.join(v1, "00000000000000b1.png"), 16, 16)   # b2 only in the v2 pack
            tiny_png(os.path.join(root, ".work-x", "00000000000000b1.png"), 16, 16)
            packs = tr.discover_packs(root)
            self.assertEqual([os.path.basename(p) for p in packs], ["realesrgan-x4plus", "1x-DEDXT+4x-GameAI-2-0"])
            self.assertEqual(tr.build(dump, packs, out, decide=1), 2)
            page = open(os.path.join(out, "index.html")).read()
            data = json.loads(re.search(r'<script id="data" type="application/json">(.*?)</script>', page, re.S).group(1))
            self.assertEqual(data["packs"], ["realesrgan-x4plus", "1x-DEDXT+4x-GameAI-2-0"])
            self.assertEqual(data["decide"], 1)
            b2, b1 = data["textures"]
            self.assertEqual(b2["hash"], "00000000000000b2")
            self.assertIsNone(b2["ups"][0])
            self.assertEqual(b2["ups"][1]["uw"], 16)
            self.assertEqual(len(b1["ups"]), 2)
            for t in data["textures"]:
                for u in t["ups"]:
                    if u:
                        self.assertTrue(os.path.exists(os.path.join(out, u["src"])))


if __name__ == "__main__":
    unittest.main()
