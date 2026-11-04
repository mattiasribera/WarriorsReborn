"""Tests of the named audio extractor (tools/audio_extract) on synthetic data only.

Covers the WAD index reader, the "Static Sounds" name table (chunk 0x29) and its debug twin, cue
name recovery (CRC-32 matches checked against the 15-character tails, prefix expansion), output
paths and categories, and an end-to-end run over synthetic XACT banks written by the helper's own
C test (runtime/build/xact_extract_test), decoded by runtime/build/xact_extract.
"""
import csv
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools", "audio_extract"))
import audio_extract as ax  # noqa: E402

BUILD = os.path.join(ROOT, "runtime", "build")


def crc(s):
    return zlib.crc32(s.encode("latin1")) & 0xFFFFFFFF


def glr_blob(records, named):
    """A .glr pack with one chunk 0x29 holding records (name, cue, bank), sorted by hash."""
    recs = sorted(((crc(n), cue, bank, n) for n, cue, bank in records))
    body = struct.pack("<I", len(recs))
    for h, cue, bank, n in recs:
        if named:
            body += n[-15:].encode("latin1").ljust(16, b"\0")
        body += struct.pack("<IIHH", h, cue, bank, 0)
    other = b"\x01\x02\x03\x04"
    return (struct.pack("<IIII", 2, 0, 0, 0) + struct.pack("<IIII", 0x10, len(other), 0, 1) + other
            + struct.pack("<IIII", 0x29, len(body), 0, 2) + body)


def write_wad(disc, files):
    """XBoxWad.idx + Main.wad0 holding files {index name hash: bytes} at 1 KiB granules."""
    wad, entries = b"", []
    for h, blob in files.items():
        entries.append((len(wad) >> 10, len(blob), h))
        wad += blob + b"\0" * (-len(blob) % 1024)
    with open(os.path.join(disc, "Main.wad0"), "wb") as f:
        f.write(wad)
    wad_table, file_table = 20, 20 + 36
    idx = struct.pack("<5I", 1, len(entries), 0x100000, wad_table, file_table)
    idx += b"Main.wad0".ljust(32, b"\0") + struct.pack("<I", 0)
    for gran, size, h in entries:
        idx += struct.pack("<4I", gran, size, 0, h)
    with open(os.path.join(disc, "XBoxWad.idx"), "wb") as f:
        f.write(idx)


class NamesTest(unittest.TestCase):
    def test_wad_hash_matches_index_convention(self):
        self.assertEqual(ax.wad_hash("Warriors.glr"), crc("ee_files\\warriors.glr"))
        self.assertEqual(ax.wad_hash("paks/global.pak"), crc("ee_files\\paks\\global.pak"))

    def test_wad_read_and_static_sounds(self):
        recs = [("vags/ambient/beach/2gulls", 1, 2), ("vags/character/ajax/fight/hit_01", 0, 2)]
        with tempfile.TemporaryDirectory() as disc:
            write_wad(disc, {ax.wad_hash("warriors.glr"): glr_blob(recs, False), ax.TWIN_HASH: glr_blob(recs, True)})
            table = ax.static_sounds(ax.wad_read(disc, ax.wad_hash("warriors.glr")), named=False)
            twin = ax.static_sounds(ax.wad_read(disc, ax.TWIN_HASH), named=True)
            self.assertIsNone(ax.wad_read(disc, 0x12345678))
        self.assertEqual(sorted((r.cue, r.bank) for r in table), [(0, 2), (1, 2)])
        self.assertEqual({r.hash for r in table}, {crc(n) for n, _, _ in recs})
        self.assertEqual({r.tail for r in twin}, {n[-15:] for n, _, _ in recs})

    def test_recover_names_direct_expansion_and_collision_guard(self):
        names = ["vags/ambient/beach/2gulls", "vags/ambient/beach/foghorn1",
                 "vags/character/ajax/fight/hit_01", "vags/character/ajax/fight/hit_02",
                 "vags/character/voices/intro", "vags/character/voices/246/response_03"]   # tail starts at a component: "246/response_03"
        twin = [ax.Sound(crc(n), i, 2, n[-15:]) for i, n in enumerate(names)]
        # only the first and third names appear in the candidate strings; their siblings come from
        # the directory prefixes plus the tails
        found = ax.recover_names(twin, {"ambient/beach/2gulls", "vags/character/ajax/fight/hit_01",
                                         "character/voices/intro", "junk"})
        self.assertEqual(sorted(found.values()), sorted(names))
        # a candidate whose CRC matches but whose tail differs is rejected
        fake = [ax.Sound(crc("vags/a/b"), 0, 0, "different_tail_")]
        self.assertEqual(ax.recover_names(fake, {"vags/a/b"}), {})
        # without the debug twin (no tails) direct CRC matches are accepted
        self.assertEqual(ax.recover_names([ax.Sound(crc("vags/a/b"), 0, 0, "")], {"a/b"}), {crc("vags/a/b"): "vags/a/b"})

    def test_harvest_candidates(self):
        got = ax.harvest(b"\0\0vags/ambient/x_1\0abc\0weapons/gun.fire\xff")
        self.assertIn("vags/ambient/x_1", got)
        self.assertIn("weapons/gun.fire", got)
        self.assertNotIn("abc", got)


class LayoutTest(unittest.TestCase):
    def test_categories(self):
        self.assertEqual(ax.category("vags/character/ajax/fight/hit_01", "xbox002"), "voice")
        self.assertEqual(ax.category("vags/speeches/l01/l01_c1", "xbox003"), "dialogue")
        self.assertEqual(ax.category("vags/xboxmusic/track01", "xbox100"), "music")
        self.assertEqual(ax.category("vags/music/stinger", "xbox003"), "music")
        self.assertEqual(ax.category("vags/ambient/beach/2gulls", "xbox002"), "ambient")
        self.assertEqual(ax.category("vags/weapons/bat_hit", "xbox000"), "sfx")
        self.assertEqual(ax.category("", "xbox002"), "voice")
        self.assertEqual(ax.category("", "xbox003"), "dialogue")
        self.assertEqual(ax.category("", "xbox001"), "sfx")
        self.assertEqual(ax.category("", "xbox250"), "music")

    def test_output_paths(self):
        self.assertEqual(ax.out_rel("vags/ambient/beach/2gulls", "xbox002", 7), "vags/ambient/beach/2gulls.wav")
        self.assertEqual(ax.out_rel("vags\\a\\b", "xbox002", 7), "vags/a/b.wav")
        for bad in ("", "vags/../etc/x", "/abs/x", "vags//x", "vags/a b"):
            self.assertEqual(ax.out_rel(bad, "xbox002", 7), "_unnamed/xbox002_0007.wav", bad)

    def test_filter(self):
        self.assertTrue(ax.matches("vags/ambient/beach/2gulls.wav", "*/beach/*"))
        self.assertTrue(ax.matches("vags/ambient/beach/2gulls.wav", "beach"))
        self.assertFalse(ax.matches("vags/ambient/beach/2gulls.wav", "*/character/*"))
        self.assertTrue(ax.matches("_unnamed/xbox002_0007.wav", "_unnamed"))


class EndToEndTest(unittest.TestCase):
    def test_extract_synthetic_banks(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/xact_extract",
                        "build/xact_extract_test"], check=True)
        with tempfile.TemporaryDirectory() as tmp:
            res = subprocess.run([os.path.join(BUILD, "xact_extract_test"), tmp], capture_output=True, text=True)
            self.assertIn("ALL OK", res.stdout, res.stdout + res.stderr)
            disc, extracted, out = (os.path.join(tmp, d) for d in ("disc", "extracted", "out"))
            os.makedirs(os.path.join(disc, "audio"))
            os.makedirs(os.path.join(extracted, "lua_src"))
            os.replace(os.path.join(tmp, "synth.xwb"), os.path.join(disc, "audio", "synth.xwb"))
            os.replace(os.path.join(tmp, "synth.xsb"), os.path.join(disc, "audio", "xbox000.xsb"))
            # cue 0 (PCM mono) and cue 1 (ADPCM stereo) are named; cue 2 stays unnamed
            recs = [("vags/ambient/beach/2gulls", 0, 0), ("vags/character/ajax/fight/hit_01", 1, 0)]
            write_wad(disc, {ax.wad_hash("warriors.glr"): glr_blob(recs, False), ax.TWIN_HASH: glr_blob(recs, True)})
            with open(os.path.join(extracted, "lua_src", "a.lua"), "w") as f:
                f.write('PlaySound("ambient/beach/2gulls") PlaySound("vags/character/ajax/fight/hit_01")\n')
            base = ["--disc", disc, "--extracted", extracted, "--out", out, "--helper",
                    os.path.join(BUILD, "xact_extract")]

            self.assertEqual(ax.main(base + ["--filter", "*/beach/*"]), 0)
            self.assertTrue(os.path.isfile(os.path.join(out, "vags/ambient/beach/2gulls.wav")))
            self.assertFalse(os.path.exists(os.path.join(out, "vags/character")))

            self.assertEqual(ax.main(base + ["-j", "2"]), 0)
            with wave.open(os.path.join(out, "vags/character/ajax/fight/hit_01.wav")) as w:
                self.assertEqual((w.getnchannels(), w.getframerate(), w.getnframes()), (2, 44100, 128))
            self.assertTrue(os.path.isfile(os.path.join(out, "_unnamed", "synth_0000.wav")))
            with open(os.path.join(out, "catalog.csv"), newline="") as f:
                rows = list(csv.DictReader(f))
            self.assertEqual(len(rows), 3)
            by = {r["name"]: r for r in rows}
            self.assertEqual(by["vags/ambient/beach/2gulls"]["category"], "ambient")
            self.assertEqual(by["vags/ambient/beach/2gulls"]["rate"], "24000")
            self.assertAlmostEqual(float(by["vags/ambient/beach/2gulls"]["duration"]), 100 / 24000, places=4)
            hit = by["vags/character/ajax/fight/hit_01"]
            self.assertEqual((hit["bank"], hit["index"], hit["channels"], hit["category"]), ("synth", "1", "2", "voice"))
            unnamed = by[""]
            self.assertEqual((unnamed["bank"], unnamed["index"], unnamed["file"]), ("synth", "0", "_unnamed/synth_0000.wav"))


if __name__ == "__main__":
    unittest.main()
