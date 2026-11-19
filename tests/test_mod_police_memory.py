"""Runs the police-memory mod (mods/89-police-memory/police_memory.lua) under a Lua 4.0 interpreter
with stand-in game functions, the way the runtime runs it: `boot` of a level's Lua state, then the
level script's calls and the mod's scheduled ticks. It needs no game data. The interpreter is $LUA4,
else ~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "89-police-memory", "police_memory.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

# Stand-ins: every engine call is recorded in CALLS as one line of text. DATA is the mod's data
# file for the loaded profile (ModSave / ModLoad); it survives "level loads" (new boots) in a test.
STUBS = r"""
true = 1   -- global.lua: true = 1, false = nil
CALLS = {}
function rec(s) tinsert(CALLS, s) end
function show(v) if type(v) == "number" then return format("%.4g", v) end return type(v) == "table" and "table" or tostring(v) end
function line(name, a) local s = name local i = 1 while i <= a.n do s = s .. " " .. show(a[i]) i = i + 1 end return s end
ModLog = function(s) rec("log " .. s) end
DATA = {}
ModSave = function(k, v) if v == nil then DATA[k] = nil else DATA[k] = tostring(v) end return 1 end
ModLoad = function(k) return DATA[k] end
CfgCrimeResponders = function(c, n) rec("CfgCrimeResponders " .. show(c) .. " " .. show(n)) end
GetCurrentLevelIndex = function() return 0 end
GetLevelId = function() return LEVEL end
NilHandle = -1
NOW = 0
GetGameTime = function() return NOW end
SCHED = {}
ScheduleFunc = function(name, ms) tinsert(SCHED, name .. " " .. ms) end
FlushScheduledFuncs = function(...) SCHED = {} end
NEXTGANG = 10
GangCreate = function(kind, name) NEXTGANG = NEXTGANG + 1 rec("GangCreate " .. kind .. " " .. name) return NEXTGANG end
GangSetRespondPercentage = function(g, p) rec("GangSetRespondPercentage " .. g .. " " .. p) end
GangAddSpawner = function(...) rec(line("GangAddSpawner", arg)) LASTSPAWNCB = arg[14] end
GangGetHeadCount = function(g) return 2 end
WANTED = nil
GangIsWanted = function(g, b) return WANTED end
CHARS = {}
HP = {}
HuCreate = function(...) local h = 100 + getn(CHARS) tinsert(CHARS, h) return h end
HuGetCharType = function(h) return 253 end
HuGetHealthPercent = function(h) return HP[h] or 100 end
HuIsDead = function(h) return nil end
HUDSetTutorialText = function(text, kind) rec("HUDSetTutorialText " .. show(text) .. " " .. show(kind)) SHOWN = text end
HUDCheckTutorialText = function(text) if SHOWN == text then return 1 end end
INSCENE = nil
HuIsInScene = function(h) return INSCENE end
LANG = 0
GetLanguage = function() return LANG end
player = 1
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
-- a new level: a new Lua state (globals of the old one gone, DATA kept), then the mod's boot
ORIG = {CfgCrimeResponders = CfgCrimeResponders, GangCreate = GangCreate, GangAddSpawner = GangAddSpawner,
        HuCreate = HuCreate, FlushScheduledFuncs = FlushScheduledFuncs}
function level(lvl, script)
  for k, v in ORIG do setglobal(k, v) end
  PoliceMemory = nil LEVEL = lvl SCHED = {} NOW = 0
  MOD_ID = "police-memory" MOD_SCRIPT = script or "config_normal"
  dofile(MODFILE)
end
-- n ticks of 500 ms of game time
function ticks(n) for i = 1, n do NOW = NOW + 500 PoliceMemory.Tick() end end
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


def grep(out, prefix):
    return [l for l in out if l.startswith(prefix)]


@unittest.skipUnless(shutil.which(LUA4) or os.path.exists(LUA4), "no Lua 4.0 interpreter")
class PoliceMemoryTest(unittest.TestCase):
    def test_helpers(self):
        out = run_lua("""
            level(95)
            local p = PoliceMemory
            print(format("tiers %d %d %d %d %d", p.tier(0), p.tier(19.9), p.tier(20), p.tier(45), p.tier(100)))
            print(format("decay %s %s %s %s", p.decay(80, 0), p.decay(80, 1), p.decay(80, 3), p.decay(0.9, 1)))
            print(format("resp %d %d %d %d %d", p.responders(2, 1), p.responders(4, 3), p.responders(0, 3),
                         p.responders(2, 0), p.responders(5, 3)))
            print(p.format(p.parse("12.5,30,4,2,7")))""")
        self.assertIn("tiers 0 0 1 2 3", out)
        self.assertIn("decay 80 40 10 0", out)
        self.assertIn("resp 3 5 0 2 5", out)
        self.assertIn("12.5,30.0,4,2,7", out)

    def test_first_visit_changes_nothing(self):
        out = run_lua("""
            level(95)
            CfgCrimeResponders(2, 3)
            local g = GangCreate(1, "BeatCop", 0, 0)
            GangAddSpawner(g, "BeatCop", 0, {}, "cops_va1", {0, 0, 0}, 0, -1, 3000, 1, 7, NilHandle, 0, "CopFlagNet", 40, NilHandle, "nothing")""")
        self.assertEqual(grep(out, "CfgCrimeResponders"), ["CfgCrimeResponders 2 3"])
        self.assertEqual(grep(out, "GangSetRespondPercentage"), [])
        self.assertTrue(grep(out, "GangAddSpawner")[0].startswith(
            "GangAddSpawner 11 BeatCop 0 table cops_va1 table 0 -1 3000 1 7 -1 0 PoliceMemory.Spawned1 40"))
        self.assertIn("log level 95 (coney): notoriety 0.0 (was 0.0, 0 level(s) away; peak 0.0, "
                      "0 crimes, 0 cops beaten) -> tier 0: nothing changes", out)

    def test_spawner_callback_name_stays_referenced(self):
        # The engine gets the callback name as a char pointer; the name the wrapper builds must stay
        # referenced by the mod (not only by the call's temporary arg table) after the call.
        out = run_lua("""
            level(95)
            local g = GangCreate(1, "BeatCop", 0, 0)
            GangAddSpawner(g, "BeatCop", 0, {}, "cops_va1", {0, 0, 0}, 0, -1, 3000, 1, 7, NilHandle, 0, "CopFlagNet", 40, NilHandle, "nothing")
            print("kept " .. tostring(PoliceMemory.names and PoliceMemory.names[1]))""")
        self.assertIn("kept PoliceMemory.Spawned1", out)

    def test_heat_is_earned_kept_and_applied_next_visit(self):
        out = run_lua("""
            level(95)
            GangWarriors = 3
            WANTED = 1 ticks(40)                -- 20 s wanted: 8 + 10
            WANTED = nil ticks(2)
            local cop = HuCreate("cops_va1") ticks(1) HP[cop] = 60 ticks(1) HP[cop] = 0.2 ticks(1)   -- one cop beaten: + 6
            local gone = HuCreate("cops_va1") ticks(1) HP[gone] = 0 ticks(1)   -- removed from full health: no points
            WANTED = 1 ticks(1) WANTED = nil ticks(1)                          -- a second incident: + 8.25
            print("stored " .. DATA["d.coney"])
            dump() print("--")
            level(86, "config_hard")            -- Coney again (mission 9): no decay
            print("stored " .. DATA["d.coney"])
            CfgCrimeResponders(2, 1) CfgCrimeResponders(1, 0)
            local g = GangCreate(1, "RespondCop", 0, 0)
            GangAddSpawner(g, "RespondCop", 0, {}, "cops_va2", {0, 0, 0}, 0, -1, 1000, 2, 4, NilHandle, 0, "CopResponse", 40, NilHandle, "nothing")
            local civ = GangCreate(23, "Civilians", 0, 0)
            GangAddSpawner(civ, "Civilians", 0, {}, "warr_sw", {0, 0, 0}, 0, -1, 3000, 10, 7, NilHandle, 0, "CivFlagNet", 40, NilHandle, "nothing")
            PoliceMemory.Hint()""")
        first = out[:out.index("--")]
        self.assertIn("log police-memory: wanted (incident 1 here): notoriety 8.0", first)
        self.assertTrue(any(l.startswith("log police-memory: no longer wanted after 20.0 s: notoriety 17.8") for l in first), first)
        self.assertIn("log police-memory: cop beaten (1 here): notoriety 23.8", first)
        self.assertIn("stored 31.8,31.8,2,1,1", first)
        second = out[out.index("--") + 1:]
        self.assertIn("stored 31.8,31.8,2,1,1", second)
        resp = grep(second, "CfgCrimeResponders")
        self.assertEqual(resp[:14], ["CfgCrimeResponders %d %d" % (c, n + 1) for c, n in
                                     enumerate([2, 3, 3, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2])])
        self.assertEqual(resp[14:], ["CfgCrimeResponders 2 2", "CfgCrimeResponders 1 0"], "levels' calls +1, 0 stays 0")
        self.assertIn("GangSetRespondPercentage 11 45", second)
        sp = grep(second, "GangAddSpawner")
        self.assertTrue(sp[0].startswith("GangAddSpawner 11 RespondCop 0 table cops_va2 table 0 -1 750 3 4"), sp[0])
        self.assertTrue(sp[1].startswith("GangAddSpawner 12 Civilians 0 table warr_sw table 0 -1 3000 10 7 -1 0 CivFlagNet"), sp[1])
        self.assertIn("HUDSetTutorialText The cops are looking for you here 1", second)
        self.assertTrue(any("tier 1: responders +1 (at most 5), patrols +1" in l for l in second), second)

    def test_decay_away_and_calm(self):
        out = run_lua("""
            DATA["d.coney"] = "80.0,80.0,5,2,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(2)      -- Tremont
            level(14)     -- Bensonhurst
            level(95, "config_easy")     -- back in Coney: 2 levels away, 80 -> 20
            ticks(121)    -- 60 s calm: -2
            print("stored " .. DATA["d.coney"] .. " n " .. DATA["n"])
            LANG = 1 PoliceMemory.Hint()""")
        self.assertTrue(any("level 95 (coney): notoriety 20.0 (was 80.0, 2 level(s) away" in l for l in out), out)
        resp = grep(out, "CfgCrimeResponders")
        self.assertEqual(resp[0], "CfgCrimeResponders 0 2", "config_easy's own 1 + 1")
        self.assertIn("stored 18.0,80.0,5,2,4 n 4", out)
        self.assertIn("HUDSetTutorialText La polic\xeda te busca por ac\xe1 1", out)

    def test_caps_with_high_notoriety(self):
        out = run_lua("""
            DATA["d.coney"] = "100,100,9,9,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(95)
            CfgCrimeResponders(9, 4) CfgCrimeResponders(2, 5)
            local g = GangCreate(1, "BeatCop", 0, 0)
            GangAddSpawner(g, "BeatCop", 0, {}, "cops_va1", {0, 0, 0}, 0, -1, 600, 1, 7, NilHandle, 0, "CopFlagNet", 40, NilHandle, "nothing")""")
        resp = grep(out, "CfgCrimeResponders")
        self.assertEqual(resp[14:], ["CfgCrimeResponders 9 5", "CfgCrimeResponders 2 5"])
        self.assertTrue(grep(out, "GangAddSpawner")[0].startswith(
            "GangAddSpawner 11 BeatCop 0 table cops_va1 table 0 -1 500 3 7"))
        self.assertIn("GangSetRespondPercentage 11 75", out)
        self.assertEqual(grep(out, "HUDSetTutorialText"), [], "the hint waits for the level's game time")
        out = run_lua('''
            DATA["d.coney"] = "100,100,9,9,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(95) ticks(15) dump() print("--") ticks(1) ticks(11) dump() print("--") ticks(1) ticks(20)''')
        a = out.index("--")
        b = out.index("--", a + 1)
        self.assertEqual(grep(out[:a], "HUDSetTutorialText"), [])
        self.assertEqual(grep(out[a:b], "HUDSetTutorialText"),
                         ["HUDSetTutorialText The cops are looking for you here 1"], "once, at 8 s")
        self.assertEqual(grep(out[b:], "HUDSetTutorialText"), ["HUDSetTutorialText nil nil"], "gone 6 s later")
        self.assertIn("log police-memory: hint gone", out[b:])

    def test_no_hint_or_patrol_inside_the_hideout_or_a_scene(self):
        out = run_lua('''
            DATA["d.coney"] = "52,52,1,1,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(95)
            coney = {TurnOnCops = function() end}
            AddBeatCop = function() rec("AddBeatCop") GangBeatCop = 7 end
            InsideClubhouse = true ticks(40) dump() print("--hideout")
            InsideClubhouse = false INSCENE = 1 ticks(40) dump() print("--scene")
            INSCENE = nil ticks(3) dump() print("--street1")
            ticks(2) dump() print("--street2")
            InsideClubhouse = true ticks(1) dump() print("--back inside")
            InsideClubhouse = false ticks(40)''')
        hideout = out[:out.index("--scene")]
        street1 = out[out.index("--scene"):out.index("--street1")]
        street2 = out[out.index("--street1"):out.index("--street2")]
        inside = out[out.index("--street2"):out.index("--back inside")]
        after = out[out.index("--back inside"):]
        self.assertEqual(grep(hideout, "HUDSetTutorialText"), [], "no hint in the hideout or a scene")
        self.assertEqual(grep(hideout, "AddBeatCop"), [], "no patrol while inside")
        self.assertEqual(grep(street1, "AddBeatCop"), ["AddBeatCop"], "tier 2 on the street: the hub's patrol")
        self.assertIn("log police-memory: free roam: the hub's beat-cop patrol is on (tier 2)", street1)
        self.assertEqual(grep(street1, "HUDSetTutorialText"), [], "not before 2 s on the street")
        self.assertEqual(grep(street2, "HUDSetTutorialText"), ["HUDSetTutorialText The cops are looking for you here 1"])
        self.assertEqual(grep(inside, "HUDSetTutorialText"), ["HUDSetTutorialText nil nil"], "gone when he walks in")
        self.assertEqual(grep(after, "HUDSetTutorialText"), [], "shown once per level")
        self.assertEqual(grep(after, "AddBeatCop"), [], "turned on once")

    def test_free_patrol_needs_tier_2_and_keeps_the_hub_choices(self):
        out = run_lua('''
            DATA["d.coney"] = "30,30,1,1,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(95)
            coney = {TurnOnCops = function() end}
            AddBeatCop = function() rec("AddBeatCop") end
            ticks(20) dump() print("--tier1")
            DATA["d.coney"] = "52,52,1,1,1"
            level(95)
            coney = {TurnOnCops = function() end, CopsOnNow = true}
            AddBeatCop = function() rec("AddBeatCop") end
            ticks(20) dump() print("--retail on")
            level(95)
            coney = {TurnOnCops = function() end}
            AddBeatCop = function() rec("AddBeatCop") end
            roam = {currentGame = 3}
            ticks(20) dump() print("--mini game")''')
        self.assertEqual(grep(out, "AddBeatCop"), [], "tier 1, a checkpoint that has them, a mini game: nothing")

    def test_a_tip_the_game_shows_meanwhile_stays(self):
        out = run_lua('''
            DATA["d.coney"] = "52,52,1,1,1" DATA["n"] = "1" DATA["last"] = "coney"
            level(95) ticks(17) HUDSetTutorialText("a tip of the game", 1) dump() print("--") ticks(20)''')
        self.assertEqual(grep(out[out.index("--"):], "HUDSetTutorialText"), [], "our timeout does not clear the game's tip")

    def test_left_alone_in_menus_and_armies_of_the_night(self):
        for lvl in (60, 64, 100, 105):
            out = run_lua("""
                DATA["d.level%d"] = "100,100,9,9,1"
                level(%d)
                CfgCrimeResponders(2, 2)
                print("sched " .. getn(SCHED) .. " here " .. tostring(PoliceMemory.here))""" % (lvl, lvl))
            self.assertIn("CfgCrimeResponders 2 2", out, lvl)
            self.assertIn("sched 0 here nil", out, lvl)


if __name__ == "__main__":
    unittest.main()
