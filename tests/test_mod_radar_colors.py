"""Runs the radar-colors mod (mods/92-radar-colors/radar_colors.lua) under a Lua 4.0 interpreter with
stand-in game functions, the way the runtime runs it: `boot` of the front end's Lua state, then the
game's CfgChar calls, then `after.config_preload2`. It needs no game data. The interpreter is $LUA4,
else ~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "92-radar-colors", "radar_colors.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
ModLog = function(s) rec("log " .. s) end
ModRadarColor = function(id, c) rec("ModRadarColor " .. id .. " " .. c) return 1 end
CfgChar = function(id, a, b, c, d, e, f, g, h, model) rec("CfgChar " .. id .. " " .. model) end
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
function hook(script) MOD_ID = "radar-colors" MOD_SCRIPT = script dofile(MODFILE) end
function char(id, model) CfgChar(id, 2, 13, 1, 4, 800, {}, {}, 1, model, "none", 0, 0, {}, "none", 0, "none") end
"""


def run_lua(body):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.lua")
        with open(path, "w") as f:
            f.write('MODFILE = "%s"\n' % MOD + STUBS + body + "\ndump()\n")
        res = subprocess.run([LUA4, path], capture_output=True, encoding="latin-1", timeout=30)
        if res.returncode != 0 or "error" in res.stderr:
            raise AssertionError(res.stdout + res.stderr)
        return res.stdout.splitlines()


@unittest.skipUnless(os.path.exists(LUA4), "no Lua 4.0 interpreter")
class RadarColorsTest(unittest.TestCase):
    def test_gang_models_get_their_colour(self):
        out = run_lua("""
hook("enum_preload")
char(62, "riff_cy") char(180, "dest_lt") char(245, "cops_ty_va1") char(259, "cops_vc1")
char(253, "cops_va1") char(1, "warr_cl") char(300, "lizz_so") char(301, "jsbs_lt")
hook("config_preload2")
""")
        self.assertIn("ModRadarColor 62 FF8A1E", out)       # Riffs orange
        self.assertIn("ModRadarColor 180 2F5BFF", out)      # Destroyers dark blue (lifted)
        self.assertIn("ModRadarColor 245 39FF3C", out)      # transit cops bright green
        self.assertIn("ModRadarColor 259 5A5A6E", out)      # riot cops black (lifted)
        self.assertIn("ModRadarColor 300 FF5FC8", out)      # Lizzies pink
        self.assertIn("ModRadarColor 301 5A5A6E", out)      # Jones Street Boys black (lifted)
        self.assertFalse(any(l.startswith("ModRadarColor 253 ") or l.startswith("ModRadarColor 1 ") for l in out), out)
        self.assertIn("CfgChar 62 riff_cy", out)            # the game's own call still runs
        self.assertEqual(out[-1], "log gang colours: 6 character ids coloured")

    def test_every_gang_of_the_request(self):
        out = run_lua("""
hook("enum_preload")
local models = {"moon_so", "orph_so", "fury_so1", "hiha_so", "hurr_so", "sava_so", "turn_so", "elec_so",
                "rogu_so", "samo_so", "punk_so", "bopp_so", "panz_so", "sara_so"}
local i = 1 while models[i] do char(i, models[i]) i = i + 1 end
""")
        got = [l.split()[2] for l in out if l.startswith("ModRadarColor ")]
        self.assertEqual(got, ["FFE21F", "8C9A2B", "A6A6A6", "E8261E", "2F5BFF", "8C9A2B", "E8261E", "FFE21F",
                               "A6A6A6", "E8261E", "FFE21F", "A23CF0", "8C9A2B", "5A5A6E"])

    def test_wraps_once_per_state(self):
        out = run_lua("""
hook("enum_preload") hook("enum_preload")
char(62, "riff_cy")
""")
        self.assertEqual([l for l in out if l.startswith("ModRadarColor")], ["ModRadarColor 62 FF8A1E"])

    def test_older_runtime(self):
        out = run_lua("""
ModRadarColor = nil
hook("enum_preload")
char(62, "riff_cy")
""")
        self.assertIn("log ModRadarColor missing (mods version 1.9 needed): radar colours off", out)
        self.assertIn("CfgChar 62 riff_cy", out)


if __name__ == "__main__":
    unittest.main()
