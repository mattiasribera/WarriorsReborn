"""Runs the game-independent kernel object test (runtime/build/kobj_test).

The test binary builds runtime/src/kobj.c with its own main and kernel stubs, and drives the
wait and signal handlers from host threads that stand in for guest threads. It checks that
events, mutants, semaphores, critical sections, thread objects and timers wake their waiters,
that wait-any/wait-all and timeouts behave, and that a thread blocked on one object is not
woken by a busy mutex on another object (the Bink movie player polls one about 520,000 times
a second). It needs no game data.
"""
import os
import subprocess
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "kobj_test")


class KernelObjectTest(unittest.TestCase):
    def test_waits_and_targeted_wakeups(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/kobj_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=120)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        failed = [line for line in res.stdout.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [])
        self.assertIn("ALL OK", res.stdout)


if __name__ == "__main__":
    unittest.main()
