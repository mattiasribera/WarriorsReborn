"""Runs the sparring-ring mod (mods/75-sparring) under a Lua 4.0 interpreter with stand-in game
functions, the way the runtime runs it: `boot` (sparring.lua) in each Lua state, `after.level95`
(hideout.lua) in the hideout, `after.level1*` (fight.lua) after a rumble arena's script. It needs no game
data. The interpreter is $LUA4, else ~/tools/lua-4.0.1/bin/lua; the test is skipped without one.
"""
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "75-sparring")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

# Stand-ins. CALLS records engine calls as text; DATA (ModSave), KEPT (ModKeep) and DONE (completed
# missions) survive "level loads" (new Lua states); PADS maps a pad mask to its handler's name.
STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
function dump() local i = 1 while CALLS[i] do print(CALLS[i]) i = i + 1 end CALLS = {} end
ModLog = function(s) rec("log " .. s) end
DATA = {}
ModSave = function(k, v) if v == nil then DATA[k] = nil else DATA[k] = tostring(v) end rec("save " .. k .. " " .. tostring(v)) return 1 end
ModLoad = function(k) return DATA[k] end
KEPT = {}
ModKeep = function(k, v) if v == nil then KEPT[k] = nil else KEPT[k] = tostring(v) end return 1 end
ModKept = function(k) return KEPT[k] end
ModRumbleGangs = function(a, b) rec("rumble gangs " .. a .. " " .. b) return 1 end
DONE = {}
UM_IsLevelComplete = function(n) return DONE[n] end
LANG = 0
GetLanguage = function() return LANG end
Platform = 2
CL = {wht = "<W>", gry = "<G>", warr = "<R>", bon = "<P>", but = "<B>"}
NOW = 0
GetGameTime = function() return NOW end
SCHED = {}
ScheduleFunc = function(name, ms) SCHED[name] = ms rec("sched " .. name .. " " .. ms) end
FlushScheduledFuncs = function(name) SCHED[name] = nil end
function run(name) SCHED[name] = nil dostring(name .. "()") end
AddFlag = function(name, pos) rec(format("AddFlag %s %.1f %.1f %.1f", name, pos[1], pos[2], pos[3])) return 77 end
PROMPT = nil
SetMsgHandlerEx = function(f, m, fn, title, line) PROMPT = {fn = fn, title = title, line = line} rec("prompt " .. tostring(fn)) end
SetMsgHandler = function(f, m, fn) PROMPT = nil rec("prompt off") end
PADS = {}
PadSetHandler = function(pad, mask, fn) PADS[mask] = fn end
function press(mask) local fn = PADS[mask] if fn then dostring(fn .. "()") end end
LEFT, RIGHT, UP, DOWN, A, B = 32768, 8192, 4096, 16384, 64, 32
HuEnableController = function(h, on) rec("controller " .. tostring(on)) end
TUT = nil
HUDSetTutorialText = function(t) if t then TUT = t end end
ANNOUNCE = nil
HUDSetAnnounceMsg = function(s, t) ANNOUNCE = t rec("announce") end
SoundPlay2D = function() end
SoundStopMusicTrack = function() end
SetCheckPoint = function(n) rec("checkpoint " .. n) end
MenuLoadLevel = function(l) rec("load " .. l) end
player = 1
-- the game's rumble setup (its Rumble menu's choice) and script loader
GetRumbleModeData = function(t) for i = 1, 23 do t[i] = -1 end rec("game data") end
GetRumbleModeGangName = function(i) return "GAME" .. i end
doFile = function(f) rec("doFile " .. f) end
-- a new Lua state: the old globals go (DATA, KEPT, DONE stay), the mod's boot runs
function state(first)
  for k, v in globals() do if not KEEP[k] then setglobal(k, nil) end end
  for k, v in KEEP do if type(v) == "function" then setglobal(k, v) end end
  Cameras = {follow = 1}
  MOD_ID = "sparring" MOD_SCRIPT = first or "config_normal"
  dofile(MODDIR .. "/sparring.lua")
end
function hideout()
  state()
  Level = 95
  MOD_SCRIPT = "level95"
  dofile(MODDIR .. "/hideout.lua")
end
-- a rumble arena: the level script (stand-ins of its functions), then fight.lua as after.levelNNN
function arena(n)
  state("config_rumble")
  Level = n
  RM = {}
  -- what level1NN.lua does in Main(): ParseLuaData, the mode's files
  local t = {}
  GetRumbleModeData(t)
  RM.data = t
  RM.names = GetRumbleModeGangName(1) .. "/" .. GetRumbleModeGangName(2)
  doFile("level" .. n .. "_" .. ((t[2] == 9 and "survival") or "brawl") .. "_init")
  Rumble = {gameMode = t[1], gameType = t[2], gangSize = t[3], gang1Table = {}, gang2Table = {}}
  AddRumbleGang1 = function() rec("gang1 " .. Rumble.gangSize) end
  AddRumbleGang2 = function() rec("gang2 " .. Rumble.gangSize) end
  StartIt = function() rec("StartIt") end
  RumbleOver = function(w) rec("RumbleOver " .. w) end
  GoToMenu = function() rec("game GoToMenu") end
  ExitToClubhouse = function() rec("ExitToClubhouse") end
  RestoreHud = function() end
  MOD_SCRIPT = "level" .. n
  dofile(MODDIR .. "/fight.lua")
end
-- the fight: start, then the end after ms
function fight(ms, winner, kills)
  AddRumbleGang1() AddRumbleGang2()
  NOW = 1000 StartIt()
  p1STATS = {kills = kills or 0}
  NOW = 1000 + ms RumbleOver(winner)
  GoToMenu()
end
-- everything above survives a "level load"
KEEP = {}
for k, v in globals() do KEEP[k] = v end
"""


def line(out, prefix):
    """The first output line that starts with prefix."""
    return [l for l in out if l.startswith(prefix)][0]


def run_lua(body):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.lua")
        with open(path, "w", encoding="latin-1") as f:
            f.write('MODDIR = "%s"\n' % MOD + STUBS + body + "\ndump()\n")
        res = subprocess.run([LUA4, path], capture_output=True, encoding="latin-1", timeout=30)
        if res.returncode != 0 or "error" in res.stderr:
            raise AssertionError(res.stdout + res.stderr)
        return res.stdout.splitlines()


# The story finished, every gang of the trophy room beaten except the Boppers (Flashback E, level 11).
STORY = "for _, n in {2, 5, 81, 93, 31, 14, 9, 54, 55, 84, 82, 20} do DONE[n] = 1 end\n"


@unittest.skipUnless(shutil.which(LUA4) or os.path.exists(LUA4), "no Lua 4.0 interpreter")
class SparringTest(unittest.TestCase):
    def test_no_spot_before_the_story_is_finished(self):
        out = run_lua("DONE[2] = 1 hideout() print('flag ' .. tostring(PROMPT))")
        self.assertIn("log story not finished (mission level84 not complete): no sparring ring yet", out)
        self.assertFalse([l for l in out if l.startswith("AddFlag")])
        self.assertIn("flag nil", out)

    def test_spot_in_the_cage_room_after_the_story(self):
        out = run_lua(STORY + "hideout() print(PROMPT.fn) print(PROMPT.title) print(PROMPT.line)")
        self.assertIn("AddFlag fModSparring -170.0 89.5 -197.5", out)
        self.assertIn("Sparring.Open", out)
        self.assertIn("<W><CENTER><SIZE 2.0><BIGFONT>Sparring ring</BIGFONT>", out)
        self.assertIn("<G>press <W><MONEYFONT><ST></MONEYFONT><G> to spar", out)
        es = run_lua(STORY + "LANG = 1 hideout() print(PROMPT.title) print(PROMPT.line)")
        self.assertIn("<W><CENTER><SIZE 2.0><BIGFONT>Ring de entrenamiento</BIGFONT>", es)
        self.assertIn("<G>presionar <W><MONEYFONT><ST></MONEYFONT><G> para entrenar", es)

    def test_menu_lists_only_beaten_gangs_and_wraps_counts(self):
        out = run_lua(STORY + """
            hideout() Sparring.Open(77, player)
            print("gang " .. Sparring.Gang().en)
            press(DOWN) press(LEFT) print("gang " .. Sparring.Gang().en)
            press(RIGHT) press(RIGHT) print("gang " .. Sparring.Gang().en)
            press(DOWN) press(LEFT) print("enemies " .. Sparring.m.enemies)
            press(RIGHT) press(RIGHT) print("enemies " .. Sparring.m.enemies)
            press(DOWN) press(LEFT) print("you " .. Sparring.Crew()[1])
            press(DOWN) press(LEFT) press(LEFT) print("crew " .. Sparring.m.crew)
            press(DOWN) press(RIGHT) print("venue " .. Sparring.Venue().en)
            print("MENU " .. TUT)""")
        # beaten in story order; the Boppers (level 11 not complete) are not offered
        self.assertIn("gang Orphans", out)
        self.assertIn("gang Savage Huns", out)        # left from the first wraps to the last
        self.assertIn("gang Hurricanes", out)
        self.assertIn("enemies 9", out)               # 1 - 1 wraps to the game's 9 a side
        self.assertIn("enemies 2", out)
        self.assertIn("you Rembrandt", out)
        self.assertIn("crew 8", out)
        self.assertIn("venue Red Devil", out)
        menu = line(out, "MENU ")
        self.assertIn("Mode: Fight", menu)
        self.assertIn("Gang: Hurricanes", menu)
        self.assertIn("<R>> Venue: Red Devil", menu)
        self.assertIn("<B><X><W> fight", menu)
        self.assertIn("controller nil", out)

    def test_survival_is_solo_and_offers_its_own_venues(self):
        out = run_lua(STORY + """
            hideout() Sparring.Open(77, player)
            press(RIGHT) print("MENU " .. TUT)
            press(DOWN) press(DOWN) press(DOWN) press(RIGHT) print("venue " .. Sparring.Venue().en)""")
        menu = line(out, "MENU ")
        self.assertIn("Mode: Survival", menu)
        self.assertNotIn("Enemies:", menu)
        self.assertNotIn("Crew:", menu)
        self.assertIn("venue Tremont", out)

    def test_cancel_gives_the_controls_back(self):
        out = run_lua(STORY + "hideout() Sparring.Open(77, player) dump() press(B) print('fn ' .. PROMPT.fn) print('A ' .. tostring(PADS[A]))")
        self.assertIn("controller 1", out)
        self.assertIn("fn Sparring.Open", out)
        self.assertIn("A nil", out)

    def test_start_loads_the_arena_and_the_arena_gets_the_choice(self):
        out = run_lua(STORY + """
            hideout() Sparring.Open(77, player)
            press(DOWN) press(RIGHT)                       -- Hurricanes (gang 14)
            press(DOWN) press(RIGHT) press(RIGHT)          -- 3 enemies
            press(DOWN) press(RIGHT)                       -- you: Swan
            press(DOWN) press(RIGHT)                       -- crew of 2
            press(DOWN) press(LEFT)                        -- the last venue: Coney Drag
            press(A)
            print("kept " .. KEPT.fight)
            dump()
            arena(112)
            local s = "" for i = 1, 23 do s = s .. RM.data[i] .. " " end print("data " .. s)
            print("names " .. RM.names)
            print("pending " .. tostring(KEPT.fight))""")
        self.assertIn("kept 112 f 14 3 2 2", out)
        self.assertIn("checkpoint 1", out)
        self.assertIn("load level112", out)
        # 1P, brawl, 3 a side (the larger side), Warriors pak 1, Hurricanes pak 14; Swan first
        self.assertIn("data 3 1 3 0 13 6 2 12 16 34 22 19 27 31 119 122 123 125 126 127 124 125 126 ", out)
        self.assertIn("names WARRIORS/HURRICANES", out)
        # the native intro's DJ reads the Rumble menu's gangs: the Warriors (1) vs the Hurricanes (14)
        self.assertIn("rumble gangs 1 14", out)
        self.assertIn("doFile level112_mercy_init", out)  # Coney Drag has no brawl spawn file of its own
        self.assertIn("pending nil", out)                # used once
        self.assertNotIn("game data", out)

    def test_other_rumbles_get_the_game_data(self):
        out = run_lua("KEPT.fight = '112 f 14 3 2 2' arena(105) print('pending ' .. tostring(KEPT.fight))")
        self.assertIn("game data", out)
        self.assertIn("doFile level105_brawl_init", out)
        self.assertNotIn("rumble gangs", out)            # the game's own rumble keeps its menu's gangs
        out = run_lua("arena(112)")
        self.assertIn("game data", out)

    def test_sides_of_different_sizes(self):
        out = run_lua("KEPT.fight = '105 f 3 5 2 1' arena(105) AddRumbleGang1() AddRumbleGang2() print('size ' .. Rumble.gangSize)")
        self.assertIn("gang1 2", out)
        self.assertIn("gang2 5", out)
        self.assertIn("size 5", out)

    def test_fastest_win_is_kept_per_gang_and_size(self):
        out = run_lua("""
            KEPT.fight = '105 f 3 5 2 1' arena(105) fight(41200, 1)
            print("A " .. ANNOUNCE) dump()
            KEPT.fight = '105 f 3 5 2 1' arena(105) fight(50000, 1)
            print("B " .. ANNOUNCE) dump()
            KEPT.fight = '105 f 3 5 2 1' arena(105) fight(30000, 2)
            print("C " .. ANNOUNCE) dump()
            KEPT.fight = '105 f 3 5 2 1' arena(105) fight(39000, 1)
            print("D " .. ANNOUNCE)
            print("data " .. DATA["win.3.5.2"])""")
        self.assertIn("save win.3.5.2 41200", out)
        self.assertIn("log fight Orphans 5 vs 2 at Red Devil: won in 41.20 s, record 41.20 s (new)", out)
        a = line(out, "A ")
        self.assertIn("41.20 s", a)
        self.assertIn("New record!", a)
        b = line(out, "B ")
        self.assertIn("Record: 41.20 s", b)
        c = line(out, "C ")
        self.assertIn("Beaten", c)
        self.assertIn("Record: 41.20 s", c)
        self.assertIn("New record! <P>(before 41.20 s)", line(out, "D "))
        self.assertIn("data 39000", out)
        self.assertEqual(len([l for l in out if l.startswith("save win")]), 2)
        self.assertIn("sched Sparring.Back 7000", out)

    def test_longest_survival_with_its_knockouts(self):
        out = run_lua("""
            KEPT.fight = '134 s 5 1 1 1' arena(134) fight(151000, 1, 7)
            print("A " .. ANNOUNCE) dump()
            KEPT.fight = '134 s 5 1 1 1' arena(134) fight(90000, 1, 9)
            print("B " .. ANNOUNCE)
            print("data " .. DATA["surv.5"] .. " " .. DATA["survko.5"])""")
        self.assertIn("doFile level134_survival_init", out)
        a = line(out, "A ")
        self.assertIn("2:31", a)
        self.assertIn("7 KOs", a)
        self.assertIn("New record!", a)
        b = line(out, "B ")
        self.assertIn("Record: 2:31 (7 KOs)", b)
        self.assertIn("data 151000 7", out)

    def test_spanish_result_and_back_to_the_hideout(self):
        out = run_lua("""
            LANG = 1 KEPT.fight = '105 f 3 1 1 1' arena(105) fight(9530, 1)
            print("A " .. ANNOUNCE) run("Sparring.Back")""")
        a = line(out, "A ")
        self.assertIn("9,53 s", a)
        self.assertIn("¡Nuevo récord!", a)
        self.assertIn("ExitToClubhouse", out)
        self.assertNotIn("game GoToMenu", out)

    def test_menu_shows_the_record_of_the_choice(self):
        out = run_lua(STORY + "DATA['win.3.1.1'] = '9530' hideout() Sparring.Open(77, player) print('MENU ' .. TUT)")
        self.assertIn("Record: 9.53 s", line(out, "MENU "))


if __name__ == "__main__":
    unittest.main()
