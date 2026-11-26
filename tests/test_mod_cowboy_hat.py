"""Runs the cowboy-hat mod (mods/96-cowboy-hat/cowboyhat.lua) under a Lua 4.0 interpreter with
stand-in game functions, alone and booted after Dirty Fights and More Hats Fly in folder order (as
the runtime loads them). It needs no game data. The interpreter is $LUA4, else
~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = os.path.join(ROOT, "mods")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
NilHandle = 0
TYPES = {}
NEXT = 100
ModLog = function(s) rec("log " .. s) end
HuCreate = function(name, id) NEXT = NEXT + 1 TYPES[NEXT] = id rec("HuCreate " .. name .. " " .. id) return NEXT end
HuGetCharType = function(h) return TYPES[h] or 0 end
HuSetKeepHat = function(h, on) rec("HuSetKeepHat " .. TYPES[h] .. " " .. tostring(on)) end
HuHasHat = function(h) return NilHandle end
CfgObj = function(...) end
CfgHat = function(...) end
GangAddSpawner = function(...) end
ScheduleFunc = function(...) end
GetGameTime = function() return 0 end
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
function boot(folder, file) MOD_ID = folder MOD_SCRIPT = "boot" dofile(MODSDIR .. "/" .. folder .. "/" .. file) end
"""


def run_lua(body):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.lua")
        with open(path, "w") as f:
            f.write('MODSDIR = "%s"\n' % MODS + STUBS + body + "\ndump()\n")
        res = subprocess.run([LUA4, path], capture_output=True, encoding="latin-1", timeout=30)
        if res.returncode != 0 or "error" in res.stderr:
            raise AssertionError(res.stdout + res.stderr)
        return res.stdout.splitlines()


LEVEL = """
HuCreate("Cowboy", 19, {0, 0, 0}, 0, "warr_sw", 0, 1)
HuCreate("Cochise", 15, {0, 0, 0}, 0, "warr_sw", 1, 1)
local cb = HuCreate("Cowboy", 18, {0, 0, 0}, 0, "warr_sw", 0, 1)
HuCreate("Cowboy", 20, {0, 0, 0}, 0, "warr_sw", 0, 1)
HuSetKeepHat(cb, false)
local co = HuCreate("Cochise", 16, {0, 0, 0}, 0, "warr_sw", 1, 1)
HuSetKeepHat(co, 1)
HuSetKeepHat(co, false)
"""


@unittest.skipUnless(os.path.exists(LUA4), "no Lua 4.0 interpreter")
class CowboyHatTest(unittest.TestCase):
    def check(self, out):
        keeps = [l for l in out if l.startswith("HuSetKeepHat")]
        # every Cowboy (18, 19, 20) keeps his hat; a script's "false" (nil in Lua 4.0) for him becomes 1;
        # other characters' calls go to the game unchanged, and nobody else gets one
        self.assertEqual(keeps, ["HuSetKeepHat 19 1", "HuSetKeepHat 18 1", "HuSetKeepHat 20 1",
                                 "HuSetKeepHat 18 1", "HuSetKeepHat 16 1", "HuSetKeepHat 16 nil"], out)
        self.assertEqual(len([l for l in out if l.startswith("HuCreate ")]), 5, out)   # the game's own call, once each

    def test_alone(self):
        out = run_lua('boot("96-cowboy-hat", "cowboyhat.lua")\n' + LEVEL)
        self.assertIn("log Cowboy keeps his hat", out)
        self.check(out)

    def test_with_dirty_fights_and_more_hats_fly(self):
        out = run_lua('boot("10-extra-hats", "record.lua") boot("80-dirty-fights", "dirty.lua") '
                      'boot("96-cowboy-hat", "cowboyhat.lua")\n' + LEVEL)
        self.check(out)

    def test_boot_twice_wraps_once(self):
        out = run_lua('boot("96-cowboy-hat", "cowboyhat.lua") boot("96-cowboy-hat", "cowboyhat.lua")\n' + LEVEL)
        self.check(out)

    def test_hat_object_untouched_for_dirty_fights(self):
        # the cowboy's hat stays out of Dirty Fights' own knock-off list (it would HuDrop it)
        with open(os.path.join(MODS, "80-dirty-fights", "dirty.lua")) as f:
            self.assertNotIn("dyn_warr_cb", f.read())


if __name__ == "__main__":
    unittest.main()
