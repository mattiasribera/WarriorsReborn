"""Runs the game-independent audio test (runtime/build/audio_test) and compares its Xbox ADPCM
decoder with an independent Python port of vgmstream's decode_xbox_ima
(src/coding/ima_decoder.c) over pseudo-random blocks.
"""
import os
import random
import struct
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "audio_test")

STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def decode_block(block, channels):
    """vgmstream decode_xbox_ima: per channel the header sample, then 63 nibbles."""
    out = [[0] * 64 for _ in range(channels)]
    for ch in range(channels):
        hist, idx = struct.unpack_from("<hB", block, 4 * ch)
        idx = min(max(idx, 0), 88)
        out[ch][0] = hist
        for i in range(1, 64):
            if channels == 2:
                off = 4 * 2 + 4 * ch + 4 * 2 * ((i - 1) // 8) + ((i - 1) % 8) // 2
            else:
                off = 4 + (i - 1) // 2
            nib = (block[off] >> (4 if (i - 1) & 1 else 0)) & 0xF
            step = STEPS[idx]
            delta = step >> 3
            if nib & 1:
                delta += step >> 2
            if nib & 2:
                delta += step >> 1
            if nib & 4:
                delta += step
            if nib & 8:
                delta = -delta
            hist = max(-32768, min(32767, hist + delta))
            idx = min(max(idx + INDEX[nib], 0), 88)
            out[ch][i] = hist
    return [out[c][i] for i in range(64) for c in range(channels)]


class AudioTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/audio_test"], check=True)

    def test_audio_checks(self):
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([l for l in res.stdout.splitlines() if l.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)

    def test_adpcm_matches_reference(self):
        rng = random.Random(1234)
        for channels in (1, 2):
            blocks = []
            for _ in range(40):
                b = bytearray(rng.randrange(256) for _ in range(36 * channels))
                for c in range(channels):
                    b[4 * c + 2] = rng.randrange(89)   # valid step index
                blocks.append(bytes(b))
            res = subprocess.run([TEST_BIN, "adpcm", str(channels)], input=b"".join(blocks), capture_output=True, timeout=60)
            self.assertEqual(res.returncode, 0)
            got = list(struct.unpack("<%dh" % (len(res.stdout) // 2), res.stdout))
            want = [s for b in blocks for s in decode_block(b, channels)]
            self.assertEqual(len(got), len(want))
            self.assertEqual(got, want, "channels=%d" % channels)


if __name__ == "__main__":
    unittest.main()
