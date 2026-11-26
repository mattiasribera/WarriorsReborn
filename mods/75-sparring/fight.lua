-- Sparring ring, part 3 (after.level1*: after a rumble arena's script; only in the arena whose first
-- GetRumbleModeData took the player's choice, see sparring.lua). The arena's script has run its Main()
-- (setup, spawn flags, brawl.lua or survival.lua); the fight itself starts later from the game's start
-- callback (DoRules -> StartRumble -> AddRumbleGang1/2, the intro, FinishCountdown -> StartIt), ends in
-- RumbleOver(winner) (1 the Warriors, 2 the other gang, 3 a draw; in survival, when the player goes
-- down) and 3 s later GoToMenu shows the game's Replay / Quit screen. Wrapped here:
--   * AddRumbleGang1 / AddRumbleGang2: each side gets its own size (the game gives both the same);
--   * StartIt: the clock starts (the intro is over, the fight begins);
--   * RumbleOver: the time (and in survival the game's KO count, p1STATS.kills) becomes a result;
--   * GoToMenu: instead of the Replay / Quit screen, the result and the record in the announce slot,
--     then back to the hideout the way the game's own Rumble menu goes (ExitToClubhouse).

local c = Sparring.c
if c and not Sparring.wrapped and MOD_SCRIPT == "level" .. Level and AddRumbleGang1 and AddRumbleGang2 and
   StartIt and RumbleOver and GoToMenu and ExitToClubhouse then
  Sparring.wrapped = 1
  Sparring.orig = {AddRumbleGang1 = AddRumbleGang1, AddRumbleGang2 = AddRumbleGang2, StartIt = StartIt,
                   RumbleOver = RumbleOver, GoToMenu = GoToMenu}
  Sparring.size = Rumble.gangSize
  Sparring.SHOW = 6500     -- ms the result stays (<DISPLAYTIME>)
  Sparring.BACK = 7000     -- ms from the result to the way back

  AddRumbleGang1 = function()
    local S = Sparring
    if S.c.kind == "f" then
      Rumble.gangSize = S.c.crew
    end
    local a, b = S.orig.AddRumbleGang1()
    Rumble.gangSize = S.size
    return a, b
  end

  AddRumbleGang2 = function()
    local S = Sparring
    if S.c.kind == "f" then
      Rumble.gangSize = S.c.enemies
    end
    local a, b = S.orig.AddRumbleGang2()
    Rumble.gangSize = S.size
    return a, b
  end

  StartIt = function()
    Sparring.t0 = GetGameTime()
    return Sparring.orig.StartIt()
  end

  RumbleOver = function(p0, p1)
    local S = Sparring
    if S.t0 and not S.r then
      local ms = GetGameTime() - S.t0
      local kos = (S.c.kind == "s" and p1STATS and p1STATS.kills) or 0
      S.r = S.Result(S.c, p0 == 1, ms, kos)
      local g = S.GangById(S.c.gang).en
      local line
      if S.c.kind == "s" then
        line = format("survival vs %s at %s: %s, %d KOs", g, S.c.venue.en, S.Clock(ms), kos)
      else
        line = format("fight %s %d vs %d at %s: %s in %.2f s", g, S.c.enemies, S.c.crew, S.c.venue.en,
                      (p0 == 1 and "won") or ((p0 == 3 and "draw") or "lost"), ms / 1000)
      end
      if S.r.new then
        line = line .. ", record " .. ((S.c.kind == "s" and S.Clock(ms)) or format("%.2f s", ms / 1000)) .. " (new)"
      elseif S.r.best then
        line = line .. ", record " .. ((S.c.kind == "s" and S.Clock(S.r.best)) or format("%.2f s", S.r.best / 1000))
      end
      if S.r.unsaved then
        line = line .. " (record not saved)"
      end
      ModLog(line)
    end
    return S.orig.RumbleOver(p0, p1)
  end

  GoToMenu = function()
    local S = Sparring
    local r = S.r
    RestoreHud()
    if not r then
      return S.orig.GoToMenu()
    end
    local T = S.T()
    local c = S.c
    local s = "<CENTER><SIZE 2.0><BIGFONT><DISPLAYTIME " .. S.SHOW .. ">" .. CL.wht
    if c.kind == "s" then
      s = s .. T.survived .. ": " .. S.Clock(r.ms) .. "  " .. format(T.kos, r.kos)
    elseif r.won then
      s = s .. T.won .. ": " .. S.Secs(r.ms)
    else
      s = s .. T.lost
    end
    if r.new then
      s = s .. "<CR3 0.02>" .. CL.warr .. T.new
      if r.best then
        s = s .. " " .. CL.bon .. "(" .. T.was .. " " .. S.RecordText(c, r.best, r.bestko, T) .. ")"
      end
    else
      s = s .. "<CR3 0.02>" .. CL.bon .. T.record .. ": " .. S.RecordText(c, r.best, r.bestko, T)
    end
    S.text = s .. "</BIGFONT>"      -- kept referenced while the HUD shows it
    HUDSetAnnounceMsg(5, S.text)
    ScheduleFunc("Sparring.Back", S.BACK)
  end

  Sparring.Back = function()
    ExitToClubhouse()
  end

  ModLog("arena level" .. Level .. " set up for the sparring fight")
end
