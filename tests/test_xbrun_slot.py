"""tools/xbrun-slot: the run queue for headless game runs (one at a time, flock based).

Each test uses its own lock file (XBRUN_SLOT_LOCK in a temporary directory) and --no-scope, so it
never meets a real run's slot and needs no systemd. Payloads are short shell commands.
"""
import os
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SLOT = os.path.join(ROOT, "tools", "xbrun-slot")


class XbrunSlotTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = self.tmp.name
        self.env = dict(os.environ, XBRUN_SLOT_LOCK=os.path.join(self.dir, "slot.lock"))
        self.procs = []

    def tearDown(self):
        for p in self.procs:
            if p.poll() is None:
                p.kill()
            p.wait()
            p.stderr.close()
        self.tmp.cleanup()

    def run_slot(self, *args, timeout=20):
        return subprocess.run([SLOT, *args], env=self.env, capture_output=True, text=True, timeout=timeout)

    def start_holder(self, label, payload):
        """Starts a slot run in the background and returns once it holds the slot."""
        p = subprocess.Popen([SLOT, "--no-scope", "--label", label, "--", "sh", "-c", payload],
                             env=self.env, stderr=subprocess.PIPE, text=True)
        self.procs.append(p)
        line = p.stderr.readline()          # blocks until the holder prints its acquired line
        self.assertIn("acquired for " + label, line)
        return p

    def stamp(self, name):
        return "date +%%s.%%N > %s" % os.path.join(self.dir, name)

    def read_stamp(self, name):
        with open(os.path.join(self.dir, name)) as f:
            return float(f.read())

    def test_two_runs_serialize(self):
        first = self.start_holder("first", "%s; sleep 1; %s" % (self.stamp("a_start"), self.stamp("a_end")))
        second = self.run_slot("--no-scope", "--label", "second", "--", "sh", "-c", self.stamp("b_start"))
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertIn("waiting for first (pid %d) since " % first.pid, second.stderr)
        self.assertIn("acquired for second", second.stderr)
        self.assertEqual(first.wait(timeout=20), 0)
        self.assertGreaterEqual(self.read_stamp("b_start"), self.read_stamp("a_end"))

    def test_killed_holder_releases_slot(self):
        holder = self.start_holder("doomed", "exec sleep 60")
        holder.send_signal(signal.SIGKILL)  # the command itself holds the lock fd (exec'd)
        holder.wait(timeout=20)
        t0 = time.monotonic()
        res = self.run_slot("--no-scope", "--timeout", "5", "--", "true")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertLess(time.monotonic() - t0, 4)

    def test_timeout_gives_up_without_running(self):
        self.start_holder("long", "exec sleep 60")
        marker = os.path.join(self.dir, "ran")
        res = self.run_slot("--no-scope", "--timeout", "0.3", "--", "touch", marker)
        self.assertEqual(res.returncode, 75)
        self.assertIn("gave up after 0.3s; still held by long", res.stderr)
        self.assertFalse(os.path.exists(marker))

    def test_status(self):
        res = self.run_slot("--status")
        self.assertEqual((res.returncode, res.stdout), (0, "xbrun-slot: free\n"))
        holder = self.start_holder("rate-diff m8", "exec sleep 60")
        res = self.run_slot("--status")
        self.assertEqual(res.returncode, 1)
        self.assertRegex(res.stdout, r"^xbrun-slot: held by rate-diff m8 \(pid %d\) since "
                                     r"\d\d:\d\d:\d\d, \d+s\n$" % holder.pid)
        holder.kill()
        holder.wait(timeout=20)
        res = self.run_slot("--status")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_exit_code_passthrough(self):
        res = self.run_slot("--no-scope", "--", "sh", "-c", "echo out; exit 7")
        self.assertEqual((res.returncode, res.stdout), (7, "out\n"))
        self.assertIn("acquired for sh", res.stderr)

    def test_usage_errors(self):
        self.assertEqual(self.run_slot("--no-scope").returncode, 2)
        self.assertEqual(self.run_slot("--timeout", "soon", "--", "true").returncode, 2)


if __name__ == "__main__":
    unittest.main()
