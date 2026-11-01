"""Console + log output layer of the Windows build (runtime/src/platform/outlog.c), on Linux.

The Windows build must not redirect stdout (a pipe tee echoed its own output forever on the
user's PC); outlog writes each formatted line once to stdout/stderr and once to the log.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME = os.path.join(ROOT, "runtime")


class OutlogTest(unittest.TestCase):
    def test_each_line_once(self):
        subprocess.run(["make", "-s", "-C", RUNTIME, "build/tee_test"], check=True)
        with tempfile.TemporaryDirectory() as d:
            log, out, err = (os.path.join(d, n) for n in ("t.log", "out.txt", "err.txt"))
            with open(out, "w") as fo, open(err, "w") as fe:
                res = subprocess.run([os.path.join(RUNTIME, "build", "tee_test"), log], stdout=fo, stderr=fe, timeout=20)
            self.assertEqual(res.returncode, 0)
            out_lines = open(out).read().splitlines()
            err_lines = open(err).read().splitlines()
            log_lines = open(log).read().splitlines()
        want_out = {"out t%d line %d" % (t, i) for t in range(4) for i in range(200) if i % 4 != 3}
        want_err = {"err t%d line %d" % (t, i) for t in range(4) for i in range(200) if i % 4 == 3}
        want_err.add("perror check: No such file or directory")
        long_line = "long " + "7".zfill(5000)
        want_out.add(long_line)
        self.assertEqual(sorted(out_lines), sorted(want_out))
        self.assertEqual(sorted(err_lines), sorted(want_err))
        self.assertEqual(sorted(log_lines), sorted(want_out | want_err))


if __name__ == "__main__":
    unittest.main()
