"""Runs the lasting-scars mod (mods/87-scars/scars.lua) under a Lua 4.0 interpreter with stand-in game
functions, the way the runtime runs it: `boot` of a level's Lua state, the level script's HuCreate
calls, the mod again as `after.levelNN`, then its scheduled health ticks. It needs no game data. The
interpreter is $LUA4, else ~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "87-scars", "scars.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

# Stand-ins: engine calls are recorded in CALLS as text. DATA is the mod's data for the loaded profile
# (ModSave / ModLoad) and DONE the profile's completed missions (UM_IsLevelComplete); both survive
# "level loads" (new Lua states) in a test.
STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
ModLog = function(s) rec("log " .. s) end
DATA = {}
ModSave = function(k, v) if v == nil then DATA[k] = nil else DATA[k] = tostring(v) end return 1 end
ModLoad = function(k) return DATA[k] end
ModScar = function(id, lv) if id then rec(format("ModScar %d %.2f", id, lv)) else rec("ModScar") end end
DONE = {}
UM_IsLevelComplete = function(n) return DONE[n] end
SCHED = {}
ScheduleFunc = function(name, ms) tinsert(SCHED, name) end
TYPES = {}
HP = {}
NEXT = 100
HuCreate = function(name, char) NEXT = NEXT + 1 TYPES[NEXT] = char return NEXT end
HuGetCharType = function(h) return TYPES[h] or 0 end
HuGetHealthPercent = function(h) return HP[h] or 100 end
ORIG = {HuCreate = HuCreate}
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
-- a level: a new Lua state (globals of the old one gone, DATA and DONE kept), the mod's boot, the
-- level script creating the Warriors (char ids), then the mod as after.levelNN
function level(n, chars)
  for k, v in ORIG do setglobal(k, v) end
  Scars = nil SCHED = {} TYPES = {} HP = {}
  MOD_ID = "scars" MOD_SCRIPT = "global"
  dofile(MODFILE)
  local hs = {}
  local i = 1
  while chars and chars[i] do hs[i] = HuCreate("w", chars[i]) i = i + 1 end
  MOD_SCRIPT = "level" .. n
  dofile(MODFILE)
  return hs
end
-- a fight: handle h drops to hp, then ticks
function hurt(h, hp) HP[h] = hp Scars.Tick() end
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


def marks(out):
    """ModScar calls that give marks (the clearing ModScar() left out)."""
    return [l for l in out if l.startswith("ModScar ")]


@unittest.skipUnless(shutil.which(LUA4) or os.path.exists(LUA4), "no Lua 4.0 interpreter")
class ScarsTest(unittest.TestCase):
    # Cleon (ids 1, 2, 4) knocked out in story mission 81, which he then completes.
    BEATEN = """
        DONE[80] = 1
        local h = level(81, {1})
        hurt(h[1], 40) hurt(h[1], 0)
        dump()
        DONE[81] = 1
    """

    def test_story_continues_marks_carry_and_fade(self):
        out = run_lua(self.BEATEN + """
            print("-- hideout") level(95) dump()
            print("-- next mission") level(86, {1}) dump()
            DONE[86] = 1
            print("-- then") level(93, {1}) dump()
            DONE[93] = 1
            print("-- then") level(31, {1}) dump()
            DONE[31] = 1
            print("-- then") level(14, {1})""")
        hide = out.index("-- hideout")
        self.assertIn("log cleon ended level 81 at 0 % (1 knock-outs): new marks 1.00", out[hide:])
        self.assertIn("log cleon: marks 1.00 (severity 1.00, 0 missions)", out[hide:])
        blocks = "\n".join(out).split("-- ")[1:]
        self.assertEqual(marks(blocks[0].splitlines()), ["ModScar 1 1.00", "ModScar 2 1.00", "ModScar 4 1.00"])
        self.assertEqual(marks(blocks[1].splitlines()), ["ModScar 1 1.00", "ModScar 2 1.00", "ModScar 4 1.00"])
        self.assertEqual(marks(blocks[2].splitlines())[0], "ModScar 1 0.67")
        self.assertEqual(marks(blocks[3].splitlines())[0], "ModScar 1 0.33")
        self.assertEqual(marks(blocks[4].splitlines()), [])
        self.assertIn("log cleon: healed", blocks[4])

    def test_replay_from_the_hideout_starts_clean(self):
        # Back in the hideout with marks, the player replays mission 80 (already completed) from the
        # level menu: no marks there, nothing recorded, the story's marks untouched for later.
        out = run_lua(self.BEATEN + """
            level(95) dump()
            print("-- replay")
            local h = level(80, {1})
            hurt(h[1], 5)
            print("data " .. tostring(DATA["s.cleon"]) .. " / " .. tostring(DATA["r.cleon"]))
            dump()
            print("-- story again") level(86, {1})""")
        blocks = "\n".join(out).split("-- ")
        replay = blocks[1].splitlines()
        self.assertIn("ModScar", replay)   # the level's marks are cleared
        self.assertEqual(marks(replay), [])
        self.assertIn("log level 80: a replay (a mission already completed): no marks, nothing recorded", replay)
        self.assertIn("data 1.00 0 / nil", replay)   # the story's marks kept, no record of the replay
        self.assertEqual(marks(blocks[2].splitlines()), ["ModScar 1 1.00", "ModScar 2 1.00", "ModScar 4 1.00"])

    def test_other_jumps_outside_the_story_start_clean(self):
        out = run_lua(self.BEATEN + """
            level(95) dump()
            print("-- aotn") level(60, {1})""")
        aotn = "\n".join(out).split("-- ")[1].splitlines()
        self.assertEqual(marks(aotn), [])
        self.assertIn("log level 60: Armies of the Night, outside the story: no marks, nothing recorded", aotn)

    def test_unfinished_mission_leaves_no_marks(self):
        # Knocked out in mission 81 and back in the hideout without completing it (quit): no marks;
        # playing 81 again continues its record.
        out = run_lua("""
            DONE[80] = 1
            local h = level(81, {1})
            hurt(h[1], 0)
            dump()
            print("-- hideout") level(95) dump()
            print("-- again") level(81, {1})
            print("rec " .. tostring(Scars.rec.cleon and Scars.rec.cleon[2]))""")
        blocks = "\n".join(out).split("-- ")
        self.assertEqual(marks(blocks[1].splitlines()), [])
        self.assertEqual(marks(blocks[2].splitlines()), [])
        self.assertIn("rec 1", blocks[2])

    def test_story_level_records_health(self):
        out = run_lua("""
            local h = level(81, {2})
            hurt(h[1], 33)
            print("r " .. DATA["r.cleon"])""")
        self.assertIn("r 81 33 0", out)


if __name__ == "__main__":
    unittest.main()
