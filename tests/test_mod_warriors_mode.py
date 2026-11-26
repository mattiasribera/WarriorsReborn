"""Runs the Warriors Mode mod (mods/60-warriors-mode/warriors_mode.lua) under a Lua 4.0 interpreter
with stand-in game and runtime functions, the way the runtime runs it: boot of the front end (the
WARRIORS choice, gang wrapper), then after.config_* of a level (per profile: gangs, difficulty
re-applied, police, healing and money). It needs no game data. The interpreter is $LUA4, else ~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "60-warriors-mode", "warriors_mode.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

# Stand-ins: every engine call is recorded in CALLS as one line of text.
STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
function show(v) if type(v) == "number" then return format("%.4g", v) end return type(v) == "table" and "table" or tostring(v) end
function line(name, a) local s = name local i = 1 while i <= a.n do s = s .. " " .. show(a[i]) i = i + 1 end return s end
ModLog = function(s) rec("log " .. s) end
-- the runtime's mod data (the loaded profile's), options, run memory and difficulty list
DATA = {difficulty = "warriors"}
OPTIONS = {apply_to_all = "no"}
KEPT = {}
CHOSEN = nil
ModSave = function(k, v) rec("ModSave " .. k .. " " .. tostring(v)) DATA[k] = v return 1 end
ModLoad = function(k) return DATA[k] end
ModOption = function(k) return OPTIONS[k] end
ModKeep = function(k, v) KEPT[k] = v return 1 end
ModKept = function(k) return KEPT[k] end
ModDifficultyOption = function(l, q, y, n)
  rec("ModDifficultyOption " .. tostring(l) .. " | " .. tostring(q) .. " | " .. tostring(y) .. " | " .. tostring(n)) return 1 end
ModDifficultyChosen = function() local c = CHOSEN CHOSEN = nil return c end
GetLanguage = function() return LANG or 0 end
CfgGang = function(...) rec(line("CfgGang", arg)) end
CfgPowerClass = function(...) rec(line("CfgPowerClass", arg)) end
CfgBreakAndEnterDelay = function(n) rec("CfgBreakAndEnterDelay " .. show(n)) end
CfgCrimeResponders = function(c, n) rec("CfgCrimeResponders " .. show(c) .. " " .. show(n)) end
SetDifficulty = function(n) DIFF = n end
GetDifficulty = function() return DIFF end
GetProfileDifficulty = function() return PROFILE end
GetCurrentLevelIndex = function() return 0 end
GetLevelId = function() return LEVEL end
NilHandle = 0
HP = 50
MONEY = {10, 0}
HuIsAlive = function(h) return h == 1 end
HuGetHealthPercent = function(h) return HP end
HuSetHealthPercent = function(h, p) rec("HuSetHealthPercent " .. show(h) .. " " .. show(p)) HP = p end
HuSetHealth = function(h, v) rec("HuSetHealth " .. show(h) .. " " .. show(v)) end
HuSetMaxHealth = function(h, v) rec("HuSetMaxHealth " .. show(h) .. " " .. show(v)) end
InvGetMoney = function(p) return MONEY[p or 1] end
InvSetMoney = function(m, p) rec("InvSetMoney " .. show(m) .. " " .. show(p or 1)) MONEY[p or 1] = m end
SCHED = {}
ScheduleFunc = function(name, ms) tinsert(SCHED, name .. " " .. ms) end
FlushScheduledFuncs = function(...) rec(line("FlushScheduledFuncs", arg)) SCHED = {} end
-- a difficulty config like the game's: class 0 an enemy, 7 and 58 the Warriors' own
HARD = function()
  SetDifficulty(2)
  CfgPowerClass(0, 400, 1, 0.5, 4, 2, 0.6, 0.25, 0.5, 0.25, 1, 1, 1, 1, 10, 8, 4)
  CfgPowerClass(7, 400, 1, 0.5, 4, 2, 0.6, 0.25, 1.5, 0.25, 1, 1, 1, 1, 10, 8, 4)
  CfgPowerClass(58, 500, 1, 0.5, 4, 2, 0.6, 0.25, 1.0, 0.25, 1, 1, 1, 1, 10, 8, 4)
  CfgBreakAndEnterDelay(7)
  BaseChanceToBlock = 70
end
doFile = function(name) rec("doFile " .. name) if name == "config_hard" then CfgDifficulty = HARD CfgDifficulty() end end
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
function hook(script) MOD_ID = "warriors-mode" MOD_SCRIPT = script dofile(MODFILE) end
S = {1, 2, 3, 4, 5, 6, 7}   -- a gang strategy table (7 numbers)
CfgGang = function(...) rec(line("CfgGang", arg)) if type(arg[6]) == "table" and arg[6] ~= S then
  rec(format("strategy %s %s %s %s %s %s %s", arg[6][1], arg[6][2], arg[6][3], arg[6][4], arg[6][5], arg[6][6], arg[6][7])) end end
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


@unittest.skipUnless(shutil.which(LUA4) or os.path.exists(LUA4), "no Lua 4.0 interpreter")
class WarriorsModeTest(unittest.TestCase):
    def test_gangs_at_boot_for_every_profile(self):
        out = run_lua("""
            OPTIONS.apply_to_all = "yes"
            hook("enum_preload")
            CfgGang(0, 2, 2, 4, 10, S, 3, 4, 0)
            CfgGang(5, 2, 2, 1, 0, S, 0, 1, 0)
            CfgGang(7, 4, 4, 4, 6, S, 3, 2, 0)
            CfgGang(9, 2, 1, 4, 0, S, 4, 0, 0)""")
        calls = [l for l in out if l.startswith("CfgGang")]
        self.assertEqual(calls[0], "CfgGang 0 2 2 4 10 table 3 4 0", "the Warriors' gang is unchanged")
        self.assertTrue(calls[1].startswith("CfgGang 5 2 3 1 0 table 2 1 0"), calls[1])
        self.assertTrue(calls[2].startswith("CfgGang 7 4 4 4 6 table 4 2 0"), "4 attackers stay 4: " + calls[2])
        self.assertTrue(calls[3].startswith("CfgGang 9 2 2 4 0 table 4 0 0"), calls[3])
        self.assertIn("log gang 5: attackers on the player 2 -> 3, guard-break level 0 -> 2", out)
        self.assertFalse(any(l.startswith("ModDifficultyOption WARRIORS") for l in out), "no choice: every profile has it")

    def test_offer_in_the_difficulty_list(self):
        out = run_lua("""hook("enum_preload")""")
        self.assertIn("ModDifficultyOption WARRIORS | Warriors Mode: truly hard. Are you sure? | "
                      "YES - stronger enemies, less healing and money | NO", out)
        out = run_lua("""LANG = 1 hook("enum_preload")""")
        q = [l for l in out if l.startswith("ModDifficultyOption")][0]
        self.assertEqual(q, "ModDifficultyOption WARRIORS | Modo Warriors: dif\xedcil de verdad. \xbfSeguro? | "
                            "S\xcd - enemigos m\xe1s fuertes, menos curaci\xf3n y dinero | NO")

    def test_gangs_per_profile(self):
        # Start-up keeps each gang's numbers and passes the game's; a Warriors profile's level puts
        # the mode's values in, another profile's level the game's back.
        out = run_lua("""
            hook("enum_preload")
            CfgGang(0, 2, 2, 4, 10, S, 3, 4, 0)
            CfgGang(5, 2, 2, 1, 0, S, 0, 1, 0)
            dump() print("--boot")
            PROFILE = 2 LEVEL = 3 CfgDifficulty = HARD
            WarriorsMode.done = nil WarriorsMode.checked = nil hook("config_hard") dump() print("--warriors")
            WarriorsMode.done = nil WarriorsMode.checked = nil hook("config_hard") dump() print("--again")
            DATA = {} PROFILE = 1 LEVEL = 4
            WarriorsMode.done = nil WarriorsMode.checked = nil hook("config_normal") dump() print("--normal")""")
        boot = out[:out.index("--boot")]
        self.assertEqual([l for l in boot if l.startswith("CfgGang")],
                         ["CfgGang 0 2 2 4 10 table 3 4 0", "CfgGang 5 2 2 1 0 table 0 1 0"], "the game's values")
        w = out[out.index("--boot"):out.index("--warriors")]
        self.assertEqual([l for l in w if l.startswith("CfgGang")],
                         ["CfgGang 0 2 2 4 10 table 3 4 0", "CfgGang 5 2 3 1 0 table 2 1 0"])
        self.assertIn("log gangs: 2 set to the Warriors Mode values", w)
        self.assertIn("strategy 1 2 3 4 5 6 7", w)
        again = out[out.index("--warriors"):out.index("--again")]
        self.assertFalse(any(l.startswith("CfgGang") for l in again), "already set: not again")
        n = out[out.index("--again"):out.index("--normal")]
        self.assertEqual([l for l in n if l.startswith("CfgGang")],
                         ["CfgGang 0 2 2 4 10 table 3 4 0", "CfgGang 5 2 2 1 0 table 0 1 0"])
        self.assertIn("log gangs: 2 set to the game's values", n)

    def test_other_profiles_play_the_game(self):
        out = run_lua("""
            DATA = {} PROFILE = 1 LEVEL = 5 CfgDifficulty = function() SetDifficulty(1) BaseChanceToBlock = 60 end
            CfgDifficulty() player = 1
            hook("config_normal")
            CfgCrimeResponders(2, 2)
            print("diff " .. GetDifficulty() .. " block " .. BaseChanceToBlock .. " sched " .. getn(SCHED))""")
        self.assertIn("diff 1 block 60 sched 0", out)
        self.assertIn("CfgCrimeResponders 2 2", out)
        self.assertNotIn("doFile config_hard", out)
        self.assertIn("log level 5: not a Warriors profile (profile difficulty 1); mode off", out)

    def test_new_profile_choice_is_stored(self):
        out = run_lua("""
            DATA = {} CHOSEN = 1 PROFILE = 2 LEVEL = 1 CfgDifficulty = HARD
            hook("config_hard")
            print("stored " .. tostring(DATA.difficulty) .. " block " .. BaseChanceToBlock)""")
        self.assertIn("ModSave difficulty warriors", out)
        self.assertIn("log this new profile chose WARRIORS: stored for it", out)
        self.assertIn("stored warriors block 80", out)
        self.assertTrue(any(l.startswith("log applied after config_hard (Warriors profile, profile difficulty 2")
                            for l in out), out)
        # a new profile with one of the game's difficulties drops a stale mark (same save folder)
        out = run_lua("""
            DATA = {difficulty = "warriors"} CHOSEN = 0 PROFILE = 2 LEVEL = 1 CfgDifficulty = HARD BaseChanceToBlock = 70
            hook("config_hard")
            print("stored " .. tostring(DATA.difficulty) .. " block " .. BaseChanceToBlock)""")
        self.assertIn("ModSave difficulty nil", out)
        self.assertIn("stored nil block 70", out)

    def test_choice_waits_for_a_story_level(self):
        out = run_lua("""
            DATA = {} CHOSEN = 1 PROFILE = 2 LEVEL = 100 CfgDifficulty = HARD
            hook("config_normal")
            print("chosen " .. tostring(CHOSEN) .. " stored " .. tostring(DATA.difficulty))""")
        self.assertIn("chosen 1 stored nil", out)

    def test_apply_to_all(self):
        out = run_lua("""
            OPTIONS.apply_to_all = "yes" DATA = {} PROFILE = 1 LEVEL = 5
            CfgDifficulty = function() SetDifficulty(1) BaseChanceToBlock = 60 end
            CfgDifficulty() hook("config_normal")
            print("diff " .. GetDifficulty() .. " block " .. BaseChanceToBlock)""")
        self.assertIn("diff 2 block 80", out)
        self.assertTrue(any(l.startswith("log applied after config_normal (apply_to_all = yes") for l in out), out)

    def test_hard_profile_reapplies_hard_with_changes(self):
        out = run_lua("""
            PROFILE = 2 LEVEL = 3 CfgDifficulty = HARD
            CfgDifficulty() dump() print("--")
            hook("config_hard")
            print("block " .. BaseChanceToBlock)
            hook("config_hard")  -- the second hook in the same state does nothing""")
        after = out[out.index("--") + 1:]
        power = [l for l in after if l.startswith("CfgPowerClass")]
        self.assertEqual(len(power), 3, after)
        self.assertEqual(power[0], "CfgPowerClass 0 400 1 0.5 4 2 0.6 0.25 0.625 0.25 1 1 1 1 10 6.4 3.2")
        self.assertEqual(power[1], "CfgPowerClass 7 400 1 0.5 4 2 0.6 0.25 1.5 0.25 1 1 1 1 10 8 4")
        self.assertEqual(power[2], "CfgPowerClass 58 500 1 0.5 4 2 0.6 0.25 1 0.25 1 1 1 1 10 8 4")
        self.assertIn("CfgBreakAndEnterDelay 5", after)
        self.assertIn("block 80", after)
        self.assertNotIn("doFile config_hard", after)
        self.assertEqual(sum(1 for l in after if l.startswith("log applied after config_hard")), 1)

    def test_hard_profile_after_the_config_removed_cfgdifficulty(self):
        # The game's config_*.lua define CfgDifficulty, call it and set it to nil at its end, so it
        # is gone when the after.config_hard hook runs (the user's "attempt to call global
        # `CfgDifficulty' (a nil value)"): the mod reloads config_hard instead, and its wrappers go.
        out = run_lua("""
            PROFILE = 2 LEVEL = 3
            CfgDifficulty = function() HARD() CfgDifficulty = nil end
            CfgDifficulty() dump() print("--")
            local p = CfgPowerClass local b = CfgBreakAndEnterDelay
            hook("config_hard")
            print(format("restored %s %s", tostring(CfgPowerClass == p), tostring(CfgBreakAndEnterDelay == b)))
            print("block " .. BaseChanceToBlock)""")
        after = out[out.index("--") + 1:]
        self.assertIn("doFile config_hard", after)
        self.assertIn("CfgPowerClass 0 400 1 0.5 4 2 0.6 0.25 0.625 0.25 1 1 1 1 10 6.4 3.2", after)
        self.assertIn("CfgBreakAndEnterDelay 5", after)
        self.assertEqual(sum(1 for l in after if l.startswith("log applied after config_hard")), 1)
        self.assertIn("restored 1 1", after)
        self.assertIn("block 80", after)

    def test_wrappers_are_restored(self):
        out = run_lua("""
            PROFILE = 2 LEVEL = 3 CfgDifficulty = HARD local p = CfgPowerClass local b = CfgBreakAndEnterDelay
            hook("config_hard")
            print(format("restored %s %s", tostring(CfgPowerClass == p), tostring(CfgBreakAndEnterDelay == b)))""")
        self.assertIn("restored 1 1", out)

    def test_normal_profile_loads_hard(self):
        out = run_lua("""
            PROFILE = 1 LEVEL = 5 CfgDifficulty = function() SetDifficulty(1) BaseChanceToBlock = 60 end
            CfgDifficulty()
            hook("config_normal")
            print("diff " .. GetDifficulty() .. " block " .. BaseChanceToBlock)""")
        self.assertIn("doFile config_hard", out)
        self.assertIn("diff 2 block 80", out)
        self.assertTrue(any(l.startswith("CfgPowerClass 0 ") and " 0.625 " in l for l in out), out)

    def test_left_alone_where_the_game_forces_normal_and_in_menus(self):
        for level in (60, 64, 100, 105):
            out = run_lua("""
                PROFILE = 2 LEVEL = %d CfgDifficulty = HARD BaseChanceToBlock = 70
                hook("config_hard")
                CfgCrimeResponders(2, 2)
                print("block " .. BaseChanceToBlock .. " sched " .. getn(SCHED))""" % level)
            self.assertIn("block 70 sched 0", out, level)
            self.assertIn("CfgCrimeResponders 2 2", out, level)
            self.assertFalse(any(l.startswith("CfgPowerClass") for l in out), level)

    def test_police(self):
        out = run_lua("""
            PROFILE = 2 LEVEL = 9 CfgDifficulty = HARD
            hook("config_hard") dump()
            CfgCrimeResponders(2, 3) CfgCrimeResponders(1, 0) CfgCrimeResponders(5, 4) CfgCrimeResponders(6, 1)""")
        self.assertEqual([l for l in out if l.startswith("CfgCrimeResponders")],
                         ["CfgCrimeResponders 2 4", "CfgCrimeResponders 1 0", "CfgCrimeResponders 5 4",
                          "CfgCrimeResponders 6 2"])

    def test_healing_and_money(self):
        out = run_lua("""
            PROFILE = 2 LEVEL = 9 CfgDifficulty = HARD player = 1
            hook("config_hard") dump()
            print("first " .. SCHED[1])
            WarriorsMode.tick()            -- records 50 %% and $10
            HP = 90 MONEY[1] = 20
            WarriorsMode.tick()            -- +40 %% -> +20 %%, +$10 -> +$6
            print("hp " .. HP .. " money " .. MONEY[1]) dump()
            HP = 60 MONEY[1] = 5           -- damage and spending stay as they are
            WarriorsMode.tick()
            print("hp " .. HP .. " money " .. MONEY[1]) dump()
            HuSetHealthPercent(1, 100) InvSetMoney(100, 1)   -- scripts set them: kept
            WarriorsMode.tick()
            print("hp " .. HP .. " money " .. MONEY[1]) dump()
            FlushScheduledFuncs() print("after flush " .. getn(SCHED))
            SCHED = {} FlushScheduledFuncs("Other.Func") print("after named flush " .. getn(SCHED))""")
        self.assertIn("first WarriorsMode.tick 2000", out)
        self.assertIn("hp 70 money 16", out)
        self.assertIn("HuSetHealthPercent 1 70", out)
        self.assertIn("InvSetMoney 16 1", out)
        self.assertIn("hp 60 money 5", out)
        self.assertIn("hp 100 money 100", out)
        self.assertIn("after flush 1", out)
        self.assertIn("after named flush 0", out)

    def test_no_player_no_changes(self):
        out = run_lua("""
            PROFILE = 2 LEVEL = 9 CfgDifficulty = HARD player = nil
            hook("config_hard") dump()
            WarriorsMode.tick() HP = 90 MONEY[1] = 50 WarriorsMode.tick()
            print("hp " .. HP .. " money " .. MONEY[1])""")
        self.assertIn("hp 90 money 50", out)


if __name__ == "__main__":
    unittest.main()
