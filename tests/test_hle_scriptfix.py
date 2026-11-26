"""Runs the script-fix test (runtime/build/hle_scriptfix_test).

The test binary is tools/hle_scriptfix_test.c linked with src/hle_scriptfix.c and the dev-Lua
runner, against a fake Lua C API: the --no-script-fixes / --script-fixes options, the chunk name
normalization, that the set-up fix runs after level14 only (ScriptFixLog bound first, the stack
and the game's error handler restored), nothing while the pass is off, and the set-up chunk's
contract (the title functions it wraps by global name, the 15 s check, the calls that make the
machete guy hostile and the log line). It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "hle_scriptfix_test")


class HleScriptfixTest(unittest.TestCase):
    def test_hle_scriptfix(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/hle_scriptfix_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=120)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([line for line in res.stdout.splitlines() if line.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)
        self.assertIn("[scriptfix] disabled (--no-script-fixes)", res.stdout)


if __name__ == "__main__":
    unittest.main()
