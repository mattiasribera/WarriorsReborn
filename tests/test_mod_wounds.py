"""Runs the Visible wounds mod (mods/85-wounds/wounds.lua) under a Lua 4.0 interpreter with stand-in
game functions, the way the runtime runs it (boot hook), then drives its check: speed caps by health
for the player, the crew and enemies, recovery on healing, the chase exemptions, characters that are
gone. It needs no game data. The interpreter is $LUA4, else ~/tools/lua-4.0.1/bin/lua; the test is
skipped without one.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "85-wounds", "wounds.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

# Stand-ins: handles are numbers; HP[h] health %, GANGTYPE[h] gang type, ALIVE[h], INSCENE[h].
STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
NilHandle = 0
HP = {} GANGTYPE = {} ALIVE = {} INSCENE = {} SPEED = {}
NEXT = 10
ModLog = function(s) rec("log " .. s) end
ModSpeed = function(h, s)
  if not ALIVE[h] then return nil end
  if s then rec(format("ModSpeed %d %.2f", h, s)) SPEED[h] = s end
  return SPEED[h] or 1
end
HuCreate = function(name, id, pos, hd, model, x, gang) NEXT = NEXT + 1 HP[NEXT] = 100 ALIVE[NEXT] = 1
  GANGTYPE[NEXT] = gang return NEXT end
GangAddSpawner = function(...) SPAWNCB = arg[14] end
HuSetTireless = function(h, on) rec(format("HuSetTireless %d %s", h, tostring(on))) end
GoalDevilRun = function(h, ...) rec("GoalDevilRun " .. h) end
HuIsAlive = function(h) return ALIVE[h] end
HuIsInScene = function(h) return INSCENE[h] end
HuGetHealthPercent = function(h) return HP[h] end
HuGetGangType = function(h) return GANGTYPE[h] end
HuGetCharType = function(h) return h end
SCHED = {}
ScheduleFunc = function(name, ms) tinsert(SCHED, name .. " " .. ms) end
FlushScheduledFuncs = function(...) SCHED = {} end
NOW = 0
GetGameTime = function() return NOW end
function tick() NOW = NOW + 200 Wounds.Tick() end
function ticks(n) for i = 1, n do tick() end end
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
function hook() MOD_ID = "wounds" MOD_SCRIPT = "config_normal" dofile(MODFILE) end
"""


def run_lua(body):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.lua")
        with open(path, "w") as f:
            f.write('MODFILE = "%s"\n' % MOD + STUBS + body + "\ndump()\n")
        res = subprocess.run([LUA4, path], capture_output=True, text=True, timeout=30)
        if res.returncode != 0 or "error" in res.stderr:
            raise AssertionError(res.stdout + res.stderr)
        return res.stdout.splitlines()


@unittest.skipUnless(shutil.which(LUA4) or os.path.exists(LUA4), "no Lua 4.0 interpreter")
class WoundsTest(unittest.TestCase):
    def test_scale_steps(self):
        out = run_lua("""
            hook()
            for _, hp in {100, 40, 39.9, 20, 19.9, 0} do
              print(format("%g w %.2f o %.2f", hp, Wounds.scale(hp, 1), Wounds.scale(hp, nil)))
            end
            print(format("nil %.2f", Wounds.scale(nil, 1)))
            Wounds.cfg.steps[2].warriors = 0.5
            print(format("floor %.2f", Wounds.scale(5, 1)))""")
        self.assertIn("100 w 1.00 o 1.00", out)
        self.assertIn("40 w 1.00 o 1.00", out)
        self.assertIn("39.9 w 0.90 o 1.00", out, "enemies are off by default")
        self.assertIn("20 w 0.90 o 1.00", out)
        self.assertIn("19.9 w 0.85 o 1.00", out)
        self.assertIn("0 w 0.85 o 1.00", out)
        self.assertIn("nil 1.00", out)
        self.assertIn("floor 0.85", out, "no step goes under cfg.floor: deeper reads as slow motion")

    def test_caps_ramp_down_and_back_up(self):
        out = run_lua("""
            hook()
            Wounds.cfg.steps[1].others = 0.9 Wounds.cfg.steps[2].others = 0.8   -- enemies switched on
            player = HuCreate("p", 1, {}, 0, "warr", 0, 0)      -- 11, gang type 0
            local crew = HuCreate("c", 2, {}, 0, "warr", 0, 0)  -- 12
            local foe = HuCreate("e", 3, {}, 0, "dest", 0, 5)   -- 13
            tick() dump() print("--full")
            HP[player] = 35 HP[crew] = 15 HP[foe] = 15
            tick() dump() print("--first")
            ticks(14) dump() print("--low")
            tick() dump() print("--again")
            HP[player] = 80 HP[crew] = 30
            ticks(16) dump() print("--healed")""")
        first = out[out.index("--full") + 1:out.index("--first")]
        low = out[out.index("--first") + 1:out.index("--low")]
        again = out[out.index("--low") + 1:out.index("--again")]
        healed = out[out.index("--again") + 1:out.index("--healed")]
        self.assertEqual(out.index("--full"), 0, "full health: nothing set")
        self.assertIn("ModSpeed 11 0.99", first, "0.05 per s: one 200 ms check moves 0.01")
        self.assertIn("ModSpeed 12 0.99", first)
        self.assertIn("ModSpeed 13 0.99", first)
        self.assertIn("log wounds: character 11 (player): health 35 % -> speed x0.90 (from x1.00, 0.05 per s)", first)
        self.assertEqual([l for l in low if l.startswith("log")], [], "one log line per change, not per step")
        self.assertEqual([l for l in low if l.startswith("ModSpeed 11")][-1], "ModSpeed 11 0.90")
        self.assertEqual([l for l in low if l.startswith("ModSpeed 12")][-1], "ModSpeed 12 0.85")
        self.assertEqual([l for l in low if l.startswith("ModSpeed 13")][-1], "ModSpeed 13 0.85",
                         "an enemy step of 0.8 stops at the floor")
        self.assertFalse(any(l < "ModSpeed 11 0.90" and l.startswith("ModSpeed 11") for l in low), low)
        self.assertEqual([l for l in again if l.startswith("ModSpeed")], [], "no change: no call")
        up = [l for l in healed if l.startswith("ModSpeed 11")]
        self.assertEqual(up[0], "ModSpeed 11 0.91")
        self.assertEqual(up[-1], "ModSpeed 11 1.00", "healed: the cap goes back to none")
        self.assertEqual([l for l in healed if l.startswith("ModSpeed 12")][-1], "ModSpeed 12 0.90")
        self.assertNotIn("ModSpeed 13 1.00", healed)

    def test_cap_left_from_an_earlier_level_is_released(self):
        out = run_lua("""
            hook()
            player = HuCreate("p", 1, {}, 0, "warr", 0, 0)   -- 11
            SPEED[player] = 0.85                            -- the runtime still holds an old cap
            ticks(20)""")
        calls = [l for l in out if l.startswith("ModSpeed 11")]
        self.assertEqual(calls[0], "ModSpeed 11 0.86", "it rises from the old cap, smoothly")
        self.assertEqual(calls[-1], "ModSpeed 11 1.00")

    def test_enemies_off_by_default(self):
        out = run_lua("""
            hook()
            player = HuCreate("p", 1, {}, 0, "warr", 0, 0)    -- 11
            local foe = HuCreate("e", 3, {}, 0, "dest", 0, 5) -- 12
            HP[player] = 10 HP[foe] = 10
            ticks(20)""")
        self.assertIn("ModSpeed 11 0.85", out)
        self.assertFalse(any(l.startswith("ModSpeed 12") for l in out), out)

    def test_spawned_characters_are_watched(self):
        out = run_lua("""
            hook()
            Wounds.cfg.steps[2].others = 0.8
            Orig = function(h) rec("orig " .. h) end
            GangAddSpawner(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, "Orig")
            local h = 99 ALIVE[h] = 1 HP[h] = 10 GANGTYPE[h] = 5
            call(Wounds.Resolve(SPAWNCB), {h})
            ticks(20)""")
        self.assertIn("orig 99", out)
        self.assertIn("ModSpeed 99 0.85", out)

    def test_chases_scenes_and_gone(self):
        out = run_lua("""
            hook()
            Wounds.cfg.steps[2].others = 0.8
            player = HuCreate("p", 1, {}, 0, "warr", 0, 0)     -- 11
            local runner = HuCreate("r", 2, {}, 0, "dest", 0, 5) -- 12
            local actor = HuCreate("a", 3, {}, 0, "dest", 0, 5)  -- 13
            HP[player] = 10 HP[runner] = 10 HP[actor] = 10
            HuSetTireless(player, 1) GoalDevilRun(runner, "path") INSCENE[actor] = 1
            tick() dump() print("--exempt")
            HuSetTireless(player, nil) INSCENE[actor] = nil
            ticks(20) dump() print("--back")
            ALIVE[actor] = nil
            tick() tick() dump() print("--gone")
            print(format("watched %s", tostring(Wounds.watch[actor])))""")
        exempt = out[:out.index("--exempt")]
        back = out[out.index("--exempt") + 1:out.index("--back")]
        gone = out[out.index("--back") + 1:out.index("--gone")]
        self.assertIn("HuSetTireless 11 1", exempt, "the game's own call still runs")
        self.assertIn("GoalDevilRun 12", exempt)
        self.assertEqual([l for l in exempt if l.startswith("ModSpeed")], [], "chasers, runners, scene actors: full speed")
        self.assertIn("ModSpeed 11 0.85", back)
        self.assertIn("ModSpeed 13 0.85", back)
        self.assertFalse(any(l.startswith("ModSpeed 12") for l in back), "a chase runner stays exempt for the level")
        self.assertEqual([l for l in gone if l.startswith("ModSpeed")], [], "a gone character is not called")
        self.assertIn("watched nil", out)

    def test_tick_survives_a_flush(self):
        out = run_lua("""
            hook()
            FlushScheduledFuncs()
            print("sched " .. (SCHED[1] or "none"))""")
        self.assertIn("sched Wounds.Tick 200", out)

    def test_without_modspeed_changes_nothing(self):
        out = run_lua("""
            ModSpeed = nil
            local hc = HuCreate
            hook()
            print(format("same %s sched %d", tostring(HuCreate == hc), getn(SCHED)))""")
        self.assertIn("same 1 sched 0", out)
        self.assertTrue(any("no ModSpeed" in l for l in out), out)


if __name__ == "__main__":
    unittest.main()
