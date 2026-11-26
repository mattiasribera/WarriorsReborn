-- Sparring ring, part 2 (after.level95): the walk-up spot in the cage room and its menu.
--
-- The spot is the hideout's own walk-up prompt (AddFlag + SetMsgHandlerEx, as the game's menu and
-- Flashback spots, set up once the follow camera exists) in the middle of the caged room under the main
-- floor (walkable through this mod's ring.col, the same room as 20-hideout-trophies' cage.col). Only once
-- the story is finished (UM_IsLevelComplete(84), the gym-records rule).
--
-- The action button opens the menu. It is the flow of the game's unused dev script combatselect (which
-- cannot run: its combatselect_init / _objects files are not on the disc): pad handlers (PadSetHandler)
-- and the game's help-text box (HUDSetTutorialText), here with every choice on one page: the D-pad's
-- up/down picks a line, left/right changes it, A starts the fight, B goes back. The player stands still
-- meanwhile (HuEnableController), and the spot's prompt is off so the box shows the menu. The last
-- choice is remembered for the run (ModKeep "last").

Sparring.spot = {-170.0, 89.5, -197.5}
Sparring.tries = 0
Sparring.PADS = {up = 4096, down = 16384, left = 32768, right = 8192, a = 64, b = 32}

-- the menu: m.mode 1 fight / 2 survival; m.gang an index into the beaten gangs; m.lead a Warrior;
-- m.venue an index into the mode's venues
Sparring.Rows = function()
  if Sparring.m.mode == 2 then
    return {"mode", "gang", "you", "venue"}
  end
  return {"mode", "gang", "enemies", "you", "crew", "venue"}
end

Sparring.Kind = function()
  return (Sparring.m.mode == 2 and "s") or "f"
end

Sparring.Gang = function()
  return Sparring.beaten[Sparring.m.gang]
end

Sparring.Venue = function()
  return Sparring.VENUES[Sparring.Kind()][Sparring.m.venue]
end

-- the crew's names, the chosen Warrior first
Sparring.Crew = function()
  local W = Sparring.WARRIORS
  local out = {W.names[Sparring.m.lead]}
  local i = 1
  while W.names[i] do
    if i ~= Sparring.m.lead then
      tinsert(out, W.names[i])
    end
    i = i + 1
  end
  return out
end

-- the choice as the arena gets it
Sparring.Choice = function()
  local m = Sparring.m
  local c = {level = Sparring.Venue().level, kind = Sparring.Kind(), gang = Sparring.Gang().id,
             enemies = m.enemies, crew = m.crew, lead = m.lead}
  if c.kind == "s" then
    c.enemies = 1
    c.crew = 1
  end
  return c
end

-- one step from v to the next value of 1..n, wrapping
Sparring.Wrap = function(v, d, n)
  v = v + d
  if v < 1 then
    v = n
  elseif v > n then
    v = 1
  end
  return v
end

Sparring.Show = function()
  local S = Sparring
  local T = S.T()
  local m = S.m
  local c = S.Choice()
  local venue = S.Venue()
  local value = {
    mode = (m.mode == 2 and T.surv) or T.fight,
    gang = S.Gang().en,
    enemies = m.enemies,
    you = S.WARRIORS.names[m.lead],
    crew = m.crew,
    venue = (S.Es() and venue.es) or venue.en,
  }
  local s = CL.wht .. T.title
  local rows = S.Rows()
  local i = 1
  while rows[i] do
    local r = rows[i]
    local line = T[r] .. ": " .. value[r]
    if i == m.row then
      s = s .. "<CR>" .. CL.warr .. "> " .. line .. CL.wht
    else
      s = s .. "<CR>" .. line
    end
    i = i + 1
  end
  local best, bestko = S.Best(c)
  s = s .. "<CR>" .. CL.bon .. T.record .. ": " .. S.RecordText(c, best, bestko, T) .. CL.wht ..
      "<CR>" .. CL.but .. "<X>" .. CL.wht .. T.go .. CL.but .. "<O>" .. CL.wht .. T.cancel ..
      "<CR>" .. CL.gry .. T.pad
  S.menuText = s       -- the game keeps the pointer
  HUDSetTutorialText(nil)
  HUDSetTutorialText(S.menuText)
end

Sparring.Bind = function(on)
  local P = Sparring.PADS
  local names = {up = "Sparring.Up", down = "Sparring.Down", left = "Sparring.Left",
                 right = "Sparring.Right", a = "Sparring.Start", b = "Sparring.Cancel"}
  for k, mask in P do
    if on then
      PadSetHandler(0, mask, names[k])
    else
      PadSetHandler(0, mask, nil)
    end
  end
end

Sparring.Click = function()
  if SoundPlay2D then
    SoundPlay2D("vags/misc/menu_clink")
  end
end

Sparring.Up = function()
  Sparring.m.row = Sparring.Wrap(Sparring.m.row, -1, getn(Sparring.Rows()))
  Sparring.Click()
  Sparring.Show()
end

Sparring.Down = function()
  Sparring.m.row = Sparring.Wrap(Sparring.m.row, 1, getn(Sparring.Rows()))
  Sparring.Click()
  Sparring.Show()
end

Sparring.Change = function(d)
  local S = Sparring
  local m = S.m
  local r = S.Rows()[m.row]
  if r == "mode" then
    m.mode = S.Wrap(m.mode, d, 2)
    m.venue = 1
  elseif r == "gang" then
    m.gang = S.Wrap(m.gang, d, getn(S.beaten))
  elseif r == "enemies" then
    m.enemies = S.Wrap(m.enemies, d, S.MAX)
  elseif r == "you" then
    m.lead = S.Wrap(m.lead, d, getn(S.WARRIORS.names))
  elseif r == "crew" then
    m.crew = S.Wrap(m.crew, d, S.MAX)
  elseif r == "venue" then
    m.venue = S.Wrap(m.venue, d, getn(S.VENUES[S.Kind()]))
  end
  S.Click()
  S.Show()
end

Sparring.Left = function()
  Sparring.Change(-1)
end

Sparring.Right = function()
  Sparring.Change(1)
end

-- the spot's action button
Sparring.Open = function(flag, who)
  local S = Sparring
  if S.open or (who and HuIsAPlayer and not HuIsAPlayer(who)) then
    return 1
  end
  S.open = 1
  SetMsgHandler(S.flag, 0, nil)
  HuEnableController(player, nil)
  S.Bind(1)
  S.Click()
  S.Show()
  return 1
end

Sparring.Close = function()
  local S = Sparring
  S.Bind(nil)
  HUDSetTutorialText(nil)
  S.open = nil
end

Sparring.Cancel = function()
  local S = Sparring
  S.Close()
  HuEnableController(player, 1)
  SetMsgHandlerEx(S.flag, 0, "Sparring.Open", S.prompt.title, S.prompt.line)
end

Sparring.Start = function()
  local S = Sparring
  local c = S.Choice()
  local m = S.m
  S.Close()
  S.Keep("last", format("%d %d %d %d %d %d %d", m.mode, S.Gang().id, m.enemies, m.lead, m.crew, m.venue, m.row))
  S.Keep("fight", S.Encode(c))
  ModLog("fight chosen: " .. S.Encode(c))
  if SoundStopMusicTrack then
    SoundStopMusicTrack()
  end
  -- as the hideout's Rumble menu starts a rumble (menu.BeginRumbleMode), without its autosave:
  -- the way back (the arena's ExitToClubhouse -> runNextMission) puts the hub checkpoint back
  SetCheckPoint(1)
  MenuLoadLevel("level" .. c.level)
end

-- the menu as last left in this run, else the first choices
Sparring.Restore = function()
  local S = Sparring
  S.m = {mode = 1, gang = 1, enemies = 1, lead = 1, crew = 1, venue = 1, row = 1}
  local s = S.Kept("last")
  if not s then
    return
  end
  local a, b, mode, gang, enemies, lead, crew, venue, row = strfind(s, "^(%d+) (%d+) (%d+) (%d+) (%d+) (%d+) (%d+)$")
  if not a then
    return
  end
  local m = S.m
  m.mode = min(max(tonumber(mode), 1), 2)
  m.enemies = min(max(tonumber(enemies), 1), S.MAX)
  m.lead = min(max(tonumber(lead), 1), getn(S.WARRIORS.names))
  m.crew = min(max(tonumber(crew), 1), S.MAX)
  m.venue = min(max(tonumber(venue), 1), getn(S.VENUES[S.Kind()]))
  m.row = min(max(tonumber(row), 1), getn(S.Rows()))
  local i = 1
  while S.beaten[i] do
    if S.beaten[i].id == tonumber(gang) then
      m.gang = i
    end
    i = i + 1
  end
end

Sparring.Spot = function()
  local S = Sparring
  if not (Cameras and Cameras.follow and CL and CL.wht) then
    S.tries = S.tries + 1
    if S.tries < 60 then
      ScheduleFunc("Sparring.Spot", 1000)
    else
      ModLog("no sparring spot (the hideout camera never came up)")
    end
    return
  end
  local T = S.T()
  local press = T.press
  if S.Es() and Platform == 2 then
    press = "presionar "      -- level95's own word where Platform == 2 (this port)
  end
  -- the game's own action hint (level95's LEVEL95.ACTC): the action button drawn by <ST>
  S.prompt = {title = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT>" .. T.title .. "</BIGFONT>",
              line = CL.gry .. press .. CL.wht .. "<MONEYFONT><ST></MONEYFONT>" .. CL.gry .. T.verb}
  S.flag = AddFlag("fModSparring", S.spot, 0, 0, 0)
  SetMsgHandlerEx(S.flag, 0, "Sparring.Open", S.prompt.title, S.prompt.line)
  ModLog("sparring spot added (" .. getn(S.beaten) .. " gangs to fight)")
end

if not Sparring.On() then
  ModLog("story not finished (mission level" .. Sparring.STORY_LEVEL .. " not complete): no sparring ring yet")
else
  Sparring.beaten = Sparring.Beaten()
  if getn(Sparring.beaten) == 0 then
    ModLog("no gang beaten on this profile: no sparring ring")
  else
    Sparring.Restore()
    Sparring.Spot()
  end
end
