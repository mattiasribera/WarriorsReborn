"""Live game control (runtime/src/gamectl.h, tools/xbctl).

Unit part: runtime/build/gamectl_test (the line protocol, JSON replies, live options, the request
queue between threads, BMP/PNG captures), no game data needed; and xbctl's own usage paths.

Integration part (one headless game through tools/xbrun-slot): xbctl start with --fast-boot
--goto m8, `lua return GetCheckPoint()`, an input fragment, a capture, then quit. It needs the
extracted game (XBCTL_XBE / XBCTL_DISC or ~/Personal/warriors-port/extracted), a save folder
(XBCTL_SAVE), a built runtime/build/xbrun and a GPU (/dev/dri); it is skipped without them, or
with WARRIORS_NO_GAME_TESTS=1.
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEST_BIN = os.path.join(ROOT, "runtime", "build", "gamectl_test")
XBCTL = os.path.join(ROOT, "tools", "xbctl")
XBRUN = os.path.join(ROOT, "runtime", "build", "xbrun")
DATA = os.path.join(os.path.expanduser("~"), "Personal", "warriors-port", "extracted")
XBE = os.environ.get("XBCTL_XBE") or os.path.join(DATA, "default.xbe")
DISC = os.environ.get("XBCTL_DISC") or os.path.join(DATA, "disc")
SAVE = os.environ.get("XBCTL_SAVE", "")


class GamectlUnitTest(unittest.TestCase):
    def test_protocol_and_queue(self):
        subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "runtime"), "build/gamectl_test"], check=True)
        res = subprocess.run([TEST_BIN], capture_output=True, text=True, timeout=120)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual([l for l in res.stdout.splitlines() if l.startswith("FAIL")], [])
        self.assertIn("ALL OK", res.stdout)


class XbctlClientTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.env = dict(os.environ, XBCTL_HOME=self.tmp.name, XDG_RUNTIME_DIR=self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def xbctl(self, *args):
        return subprocess.run([XBCTL, *args], env=self.env, capture_output=True, text=True, timeout=30)

    def test_no_session(self):
        r = self.xbctl("--name", "none", "ping")
        self.assertEqual(r.returncode, 3)
        reply = json.loads(r.stdout)
        self.assertFalse(reply["ok"])
        self.assertIn("no session none", reply["error"])

    def test_stop_without_session(self):
        r = self.xbctl("--name", "none", "stop")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(json.loads(r.stdout), {"ok": True, "cmd": "stop", "running": False})

    def test_usage(self):
        r = self.xbctl()
        self.assertEqual(r.returncode, 2)
        self.assertIn("xbctl start", r.stdout)
        r = self.xbctl("--name", "bad/name", "ping")
        self.assertEqual(r.returncode, 2)

    def test_reply_relay(self):
        """A fake game socket: xbctl sends one line and prints the reply; exit 0/1 by "ok"."""
        import socket
        import threading
        os.makedirs(os.path.join(self.tmp.name, "xbctl"), exist_ok=True)
        path = os.path.join(self.tmp.name, "xbctl", "fake.sock")
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        srv.bind(path)
        srv.listen(2)
        seen = []

        def serve():
            for ok in (True, False):
                c, _ = srv.accept()
                line = c.makefile().readline().strip()
                seen.append(line)
                c.sendall((json.dumps({"ok": ok, "cmd": line.split()[0]}) + "\n").encode())
                c.close()

        t = threading.Thread(target=serve)
        t.start()
        r1 = self.xbctl("--name", "fake", "lua", "return", "GetCheckPoint()")
        r2 = self.xbctl("--name", "fake", "warp", "m9")
        t.join(10)
        srv.close()
        self.assertEqual(seen, ["lua return GetCheckPoint()", "warp m9"])
        self.assertEqual((r1.returncode, json.loads(r1.stdout)["ok"]), (0, True))
        self.assertEqual((r2.returncode, json.loads(r2.stdout)["ok"]), (1, False))


def game_available():
    if os.environ.get("WARRIORS_NO_GAME_TESTS") == "1":
        return False, "WARRIORS_NO_GAME_TESTS=1"
    for path, what in ((XBE, "XBE"), (DISC, "disc"), (XBRUN, "runtime/build/xbrun"), ("/dev/dri", "GPU")):
        if not os.path.exists(path):
            return False, "no " + what
    if not SAVE or not os.path.isdir(SAVE):
        return False, "no save folder (XBCTL_SAVE)"
    return True, ""


@unittest.skipUnless(*game_available())
class GamectlGameTest(unittest.TestCase):
    """One short session: start (fast boot to mission 8), Lua, input, capture, quit."""

    def test_session(self):
        tmp = tempfile.mkdtemp(prefix="gamectl-it-")
        env = dict(os.environ, XBCTL_HOME=tmp)
        name = "it%d" % os.getpid()

        def xbctl(*args, timeout=120):
            r = subprocess.run([XBCTL, "--name", name, *args], env=env, capture_output=True, text=True, timeout=timeout)
            last = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
            return r, (json.loads(last) if last.startswith("{") else None)   # start prints the socket path

        try:
            r, _ = xbctl("start", "--label", "gamectl-test", "--idle", "120", "--timeout", "1500", "--", "--goto", "m8", timeout=1600)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            r, reply = xbctl("wait", "level", timeout=300)
            self.assertTrue(reply and reply["ok"], r.stdout)
            self.assertEqual(reply["level"], "level81")
            r, reply = xbctl("lua", "return GetCheckPoint()")
            self.assertTrue(reply["ok"], r.stdout)
            self.assertEqual(reply["result"], "1")   # --goto m8: checkpoint 1
            r, reply = xbctl("input", "0:a:6,10:x:6")
            self.assertTrue(reply["ok"], r.stdout)
            self.assertEqual(reply["until"] - reply["from"], 16)
            r, reply = xbctl("wait", "frames", "30")
            self.assertTrue(reply["ok"], r.stdout)
            shot = os.path.join(tmp, "shot.png")
            r, reply = xbctl("capture", shot)
            self.assertTrue(reply["ok"], r.stdout)
            self.assertTrue(os.path.getsize(shot) > 1000)
            self.assertEqual(len(reply["hash"]), 16)
            r, reply = xbctl("opt", "bloom-strength", "2")
            self.assertFalse(reply["ok"])
            self.assertIn("--bloom-strength", reply["error"])
            r, reply = xbctl("stop", timeout=60)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            with open(os.path.join(tmp, name, "game.log"), errors="replace") as f:
                log = f.read()
            self.assertIn("[fast-boot] level81 at frame", log)
            self.assertIn("[control] quit command: quitting", log)
        finally:
            subprocess.run([XBCTL, "--name", name, "stop"], env=env, capture_output=True, timeout=60)
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
