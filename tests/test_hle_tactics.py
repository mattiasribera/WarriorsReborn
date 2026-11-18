"""Runs the game-independent --enemy-tactics test (runtime/build/hle_tactics_test).

It checks the turn, flanking, guard, weight, passivity and score rules and the option parser,
then plants the call sites (gang level, attack choice, target score, ring action; the attack gate
filter through its hook) at their guest addresses with stand-in callees and runs them: enemies on
the player or the crew get the new answers, the crew and other fights keep the title's, --ai-debug
alone only observes, and with the option off no byte changes. It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_tactics_test")


class HleTacticsTest(unittest.TestCase):
    def test_hle_tactics(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_tactics_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=60)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
