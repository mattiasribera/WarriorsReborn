"""Runs the cheats mod (mods/98-cheats/cheats.lua) under a Lua 4.0 interpreter with stand-in game
functions and a stand-in mod menu: the menu drives the game's own cheat entry (DbgEnterCheat),
toggles switch on and off, rage ends unlimited health, save-changing cheats need a second A, and
kept toggles come back silently in the next level. It needs no game data (the stand-ins are ours,
not the game's scripts). The interpreter is $LUA4, else ~/tools/lua-4.0.1/bin/lua; the test is
skipped without one.
"""
import os
import subprocess
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOD = os.path.join(ROOT, "mods", "98-cheats", "cheats.lua")
LUA4 = os.environ.get("LUA4") or os.path.expanduser("~/tools/lua-4.0.1/bin/lua")

STUBS = r"""
CALLS = {}
function rec(s) tinsert(CALLS, s) end
true = 1
false = nil
NilHandle = 0
Level = 3
player = 7
LANG = 0
KEPT = KEPT or {}
GetLanguage = function() return LANG end
ModLog = function(s) rec("log " .. s) end
ModKeep = function(k, v) KEPT[k] = v return 1 end
ModKept = function(k) return KEPT[k] end
ScheduleFunc = function(...) end
HuIsAPlayer = function(h) return h == player end
HuIsAlive = function(h) return h == player end
HuSetGodMode = function(h, on) rec("HuSetGodMode " .. h .. " " .. tostring(on)) end
HuSetTireless = function(h, on) rec("HuSetTireless " .. h .. " " .. tostring(on)) end
-- the stand-in menu: what the mod shows, the next ModMenuState answers and the key
MENU = {open = nil, labels = "", values = "", notes = "", key = nil, answers = {}}
ModMenuOpen = function(title, items, footer, sel) MENU.open = 1 MENU.title = title MENU.labels = items MENU.sel = sel rec("ModMenuOpen " .. title) return 1 end
ModMenuValues = function(v, n) MENU.values = v if n then MENU.notes = n end return 1 end
ModMenuClose = function() MENU.open = nil rec("ModMenuClose") end
ModMenuKey = function() local k = MENU.key MENU.key = nil return k end
ModMenuState = function()
  local a = MENU.answers[1]
  if a then tremove(MENU.answers, 1) return a[1], a[2], a[3] end
  if MENU.open then return "open" end
end
-- stand-ins for the game's cheat entry and rage handlers (same contract, our own code)
function DoCheat(n, who, idx)
  rec("DoCheat " .. n .. " " .. idx)
  if n == 5 then
    if CheatRage then CheatRage = nil else CheatRage = true end
    return true
  end
  if BossFight and n >= 6 and n <= 11 then return false end
  return true
end
function DbgEnterCheat(n)
  rec("DbgEnterCheat " .. n)
  if DoCheat(n, player, 1) then rec("banner") end
end
function SetRageMode(h) rec("SetRageMode " .. h) HuSetGodMode(h, 1) end
function ClrRageMode(h) rec("ClrRageMode " .. h) HuSetGodMode(h, 0) end
function dump() local i = 1 while CALLS[i] do write(CALLS[i] .. "\n") i = i + 1 end CALLS = {} end
print = rec   -- the test's own lines in order with the calls
function value(i)
  local n, s = 1, MENU.values .. "\n"
  local b, e = strfind(s, "\n")
  while b do
    if n == i then return strsub(s, 1, b - 1) end
    s = strsub(s, e + 1)
    n = n + 1
    b, e = strfind(s, "\n")
  end
end
function row(label)   -- the 1-based row of an English label
  local n, s = 1, MENU.labels .. "\n"
  local b, e = strfind(s, "\n")
  while b do
    if strsub(s, 1, b - 1) == label then return n end
    s = strsub(s, e + 1)
    n = n + 1
    b, e = strfind(s, "\n")
  end
end
function choose(label, dir) tinsert(MENU.answers, {"chosen", row(label), dir or 0}) Cheats.Tick() end
function openmenu() MENU.key = 1 Cheats.Tick() end
"""


def run_lua(body, kept=""):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "t.lua")
        with open(path, "w", encoding="latin-1") as f:
            f.write("KEPT = {" + kept + "}\n" + STUBS + 'MOD_ID = "cheats"\ndofile("%s")\n' % MOD + body + "\ndump()\n")
        res = subprocess.run([LUA4, path], capture_output=True, encoding="latin-1", timeout=30)
        if res.returncode != 0 or "error" in res.stderr:
            raise AssertionError(res.stdout + res.stderr)
        return res.stdout.splitlines()


@unittest.skipUnless(os.path.exists(LUA4), "no Lua 4.0 interpreter")
class CheatsMenuTest(unittest.TestCase):
    def test_opens_only_on_the_key_with_a_living_player(self):
        out = run_lua('Cheats.Tick() print("open " .. tostring(MENU.open)) Level = 100 MENU.key = 1 Cheats.Tick() '
                      'print("frontend " .. tostring(MENU.open)) Level = 3 openmenu() print("open " .. tostring(MENU.open)) '
                      'print(MENU.title) print(value(1))')
        self.assertEqual([l for l in out if not l.startswith(("log", "ModMenuOpen"))], ["open nil", "frontend nil", "open 1", "Cheats", "Off"], out)
        self.assertIn("log cheats menu ready: hold Back + LB or press F8", out)

    def test_health_toggle_uses_the_game_cheat_and_switches_off(self):
        out = run_lua('openmenu() dump() choose("Unlimited health") print("v " .. value(1)) print("kept " .. tostring(KEPT.health)) '
                      'choose("Unlimited health") print("v " .. value(1)) print("kept " .. tostring(KEPT.health))')
        self.assertIn("log Unlimited health on", out)
        self.assertIn("log Unlimited health off", out)
        self.assertEqual([l for l in out if not l.startswith(("log", "ModMenuOpen"))],
                         ["DbgEnterCheat 1", "DoCheat 1 1", "banner", "v On", "kept 1", "HuSetGodMode 7 0", "v Off", "kept nil"], out)

    def test_rage_ends_unlimited_health(self):
        out = run_lua('openmenu() choose("Unlimited health") SetRageMode(player) choose("Unlimited health") '
                      'print("in rage v " .. value(1)) choose("Unlimited health") ClrRageMode(player) print("after rage v " .. value(1)) '
                      'print("kept " .. tostring(KEPT.health))')
        # in rage, switching it off leaves god mode to the game (rage ends it); on again, the rage's end ends it
        self.assertNotIn("HuSetGodMode 7 0", out[:out.index("in rage v Off")], out)
        self.assertIn("log rage ended: unlimited health off (the game's rule)", out)
        self.assertEqual(out[-2:], ["after rage v Off", "kept nil"], out)

    def test_rage_is_the_games_own_toggle(self):
        out = run_lua('openmenu() choose("Unlimited rage") print("v " .. value(2) .. " " .. tostring(CheatRage)) '
                      'choose("Unlimited rage") print("v " .. value(2) .. " " .. tostring(CheatRage)) print("kept " .. tostring(KEPT.rage))')
        self.assertEqual([l for l in out if l.startswith(("Dbg", "v ", "kept"))],
                         ["DbgEnterCheat 5", "v On 1", "DbgEnterCheat 5", "v Off nil", "kept nil"], out)

    def test_sprint_off_clears_tireless(self):
        out = run_lua('openmenu() choose("Unlimited sprint") choose("Unlimited sprint")')
        self.assertEqual([l for l in out if l.startswith(("Dbg", "HuSet"))], ["DbgEnterCheat 2", "HuSetTireless 7 0"], out)

    def test_one_shot_on_a_only(self):
        out = run_lua('openmenu() choose("Get a knife", 1) choose("Get a knife", -1) print("--") choose("Get a knife")')
        self.assertEqual(out[out.index("--") + 1:][:3], ["DbgEnterCheat 6", "DoCheat 6 1", "banner"], out)
        self.assertNotIn("DbgEnterCheat 6", out[:out.index("--")], out)

    def test_refused_cheat_says_not_now(self):
        out = run_lua('BossFight = 1 openmenu() choose("Get a bat") print("v " .. value(row("Get a bat")))')
        self.assertIn("log Get a bat: the game refused it here", out)
        self.assertEqual(out[-1], "v Not now", out)

    def test_save_cheats_need_a_second_a(self):
        out = run_lua('openmenu() choose("100% completion") print("v " .. value(row("100% completion"))) print("--") '
                      'choose("100% completion") print("open " .. tostring(MENU.open))')
        before = out[:out.index("--")]
        self.assertNotIn("DbgEnterCheat 13", before, out)
        self.assertEqual(before[-1], "v A again: yes", out)
        after = out[out.index("--") + 1:]
        self.assertEqual([l for l in after if not l.startswith("log")], ["DbgEnterCheat 13", "DoCheat 13 1", "banner", "ModMenuClose", "open nil"], out)

    def test_choosing_another_row_disarms(self):
        out = run_lua('openmenu() choose("Brass knuckles") choose("Get a knife") choose("Brass knuckles")')
        self.assertNotIn("DbgEnterCheat 14", out, out)

    def test_kept_toggles_come_back_silently_next_level(self):
        out = run_lua('Cheats.Tick()', kept='health = "1", sprint = "1", rage = "1"')
        self.assertEqual([l for l in out if not l.startswith("log")], ["DoCheat 1 1", "DoCheat 2 1", "DoCheat 5 1"], out)
        self.assertIn("log kept cheats set again", out)
        self.assertNotIn("banner", out)

    def test_typed_code_updates_the_menu(self):
        out = run_lua('DbgEnterCheat(1) openmenu() print("v " .. value(1))')
        self.assertEqual(out[-1], "v On", out)

    def test_spanish(self):
        out = run_lua('LANG = 1 openmenu() print(MENU.title) print(value(1)) print(value(row("Perder a la polic\\237a")))')
        self.assertEqual(out[-3:], ["Trucos", "No", "Usar"], out)

    def test_closing_keeps_the_selection(self):
        out = run_lua('openmenu() tinsert(MENU.answers, {"closed", 5}) MENU.open = nil Cheats.Tick() openmenu() print("sel " .. MENU.sel)')
        self.assertEqual(out[-1], "sel 5", out)


if __name__ == "__main__":
    unittest.main()
