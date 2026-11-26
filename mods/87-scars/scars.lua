-- Lasting scars (boot and after.level*; docs/MODDING.md, "Lasting scars").
--
-- During a level the mod watches every Warrior the level creates (the player and the crew, by the
-- character id the game gives them) and keeps, per Warrior, his lowest health and how often he was
-- knocked out, with ModSave (per save profile, mods-data/scars.json): key r.<who> = "level low ko".
-- When the next level starts, that record becomes a scar: s.<who> = "severity age". The scar shows
-- through the runtime's ModScar: the game's own blood overlay, turned into healed marks (bruises,
-- closed cuts, a plaster on the worst ones). It fades over the next missions: full in the hideout and
-- the next mission, then 2/3, 1/3, gone. Marks only follow the story: a record becomes a scar once
-- its mission is completed (UM_IsLevelComplete), and a level outside the story (a mission already
-- completed, replayed from the hideout's level menu, or Armies of the Night) shows no marks and
-- records nothing. Every number is a field of Scars.cfg.
if not Scars then
  Scars = {}
  Scars.cfg = {
    poll = 250,        -- ms between health checks
    from = 80,         -- lowest health % that starts leaving a mark ...
    full = 20,         -- ... and the one that leaves the worst marks
    per_ko = 0.25,     -- severity added per knock-out (health 0)
    min = 0.15,        -- severity below this leaves nothing
    missions = 3,      -- missions over which a mark fades after the first one
    aotn_first = 60,   -- Armies of the Night levels (outside the story) ...
    aotn_last = 64,
    hideout = 95,
  }
  -- The Warriors' character ids (CfgChar) by person; flashback ghosts (3, 8, 14, 25) are left out:
  -- they are the Warriors of the past.
  Scars.people = {
    cleon = {1, 2, 4}, swan = {5, 6, 7, 9}, ajax = {11, 12, 13}, cochise = {15, 16, 17},
    cowboy = {18, 19, 20}, fox = {21, 22, 23, 24}, vermin = {26, 27, 28, 29}, rembrandt = {30, 31, 32},
    snow = {33, 34, 35, 36}, ash = {38, 40}, w41 = {41}, w43 = {43}, w45 = {45},
    rhino = {52}, w49 = {49}, w50 = {50}, w51 = {51}, w53 = {53}, w54 = {54},
  }
  Scars.who = {}
  for p, ids in Scars.people do
    local i = 1
    while ids[i] do Scars.who[ids[i]] = p i = i + 1 end
  end
end

Scars.Num = function(s, n)
  -- the n-th number of a "a b c" string, or nil
  local i = 0
  local rest = s or ""
  while 1 do
    local a, b = strfind(rest, "[%-%d%.]+")
    if not a then return nil end
    i = i + 1
    if i == n then return tonumber(strsub(rest, a, b)) end
    rest = strsub(rest, b + 1)
  end
end

Scars.Shown = function(sev, age)
  -- what is left of a mark after `age` missions (the first one shows it whole)
  local f = 1 - max(0, age - 1) / Scars.cfg.missions
  if f <= 0 then return 0 end
  return sev * f
end

Scars.Outside = function(num)
  -- why level num is outside the story, or nil: Armies of the Night, or a mission the profile has
  -- already completed (a replay from the hideout's level menu); the hideout is always in the story
  local c = Scars.cfg
  if num >= c.aotn_first and num <= c.aotn_last then return "Armies of the Night, outside the story" end
  if num ~= c.hideout and UM_IsLevelComplete and UM_IsLevelComplete(num) then return "a replay (a mission already completed)" end
  return nil
end

Scars.Finished = function(lev)
  -- whether the mission of a record was completed (true without UM_IsLevelComplete: older games)
  return lev == Scars.cfg.hideout or not UM_IsLevelComplete or UM_IsLevelComplete(lev)
end

Scars.Severity = function(low, ko)
  local c = Scars.cfg
  local s = (c.from - low) / (c.from - c.full)
  if s < 0 then s = 0 end
  s = s + ko * c.per_ko
  if s > 1 then s = 1 end
  if s < c.min then s = 0 end
  return s
end

-- start of a level state: the runtime's marks start empty, the level's watch list too
if MOD_SCRIPT ~= "enum_preload" and not Scars.level_state then
  Scars.level_state = 1
  Scars.watch = {}
  Scars.rec = {}
  Scars.level = nil
  if ModScar then ModScar() end

  if HuCreate and not Scars.hucreate then
    Scars.hucreate = HuCreate
    HuCreate = function(...)
      local h = call(Scars.hucreate, arg)
      if h and Scars.who[arg[2]] then
        Scars.watch[h] = {Scars.who[arg[2]], 100, arg[2]}
        if not Scars.ticking and Scars.level and not Scars.outside then
          Scars.ticking = 1
          ScheduleFunc("Scars.Tick", Scars.cfg.poll)
        end
      end
      return h
    end
  end
end

Scars.Tick = function()
  local S = Scars
  if S.outside then return end   -- a level outside the story records nothing
  local gone = {}
  for h, w in S.watch do
    if HuGetCharType(h) ~= w[3] then   -- deleted (the game gives 0), or the handle was reused
      tinsert(gone, h)
    else
      local hp = HuGetHealthPercent(h) or 100
      local r = S.rec[w[1]]
      if not r then
        r = {100, 0, -1}
        S.rec[w[1]] = r
      end
      local ko = hp <= 0.5 and w[2] > 0.5
      if hp < r[1] or ko then
        if hp < r[1] then r[1] = hp end
        if ko then r[2] = r[2] + 1 end
        -- written when the lowest health crosses a 5 % step or a knock-out happened
        local step = floor(r[1] / 5)
        if step ~= r[3] or ko then
          r[3] = step
          S.Save("r." .. w[1], format("%d %d %d", S.level, floor(r[1] + 0.5), r[2]))
          if ko then S.Log(format("%s knocked out (%d so far this level)", w[1], r[2])) end
        end
      end
      w[2] = hp
    end
  end
  local i = 1
  while gone[i] do S.watch[gone[i]] = nil i = i + 1 end
  ScheduleFunc("Scars.Tick", S.cfg.poll)
end

-- the level's own script (levelNN): turn last level's records into scars, age them, show them
local _, _, num = strfind(MOD_SCRIPT or "", "^level(%d+)$")
num = tonumber(num)
if num and num < 100 and not Scars.level then
  local S = Scars
  S.level = num
  S.Save = ModSave
  S.Load = ModLoad
  S.Log = function(text) ModLog("scars: " .. text) end
  S.outside = S.Outside(num)
end
if num and num < 100 and Scars.outside and not Scars.told then
  -- outside the story: the runtime's marks stay cleared, records and marks on disk stay as they are
  Scars.told = 1
  ModLog(format("level %d: %s: no marks, nothing recorded", num, Scars.outside))
elseif num and num < 100 and not Scars.told then
  local S = Scars
  S.told = 1
  local last = tonumber(S.Load("last") or "") or -1
  local aging = num ~= 95 and num ~= last
  local lines = {}
  for p, _ in S.people do
    local sev = tonumber(Scars.Num(S.Load("s." .. p), 1) or 0) or 0
    local age = Scars.Num(S.Load("s." .. p), 2) or 0
    if sev > 0 and aging then age = age + 1 end
    local r = S.Load("r." .. p)
    local rlev = Scars.Num(r, 1)
    if rlev == num then   -- this level again (a checkpoint reload): its record goes on
      S.rec[p] = {Scars.Num(r, 2) or 100, Scars.Num(r, 3) or 0, -1}
    elseif rlev and not S.Finished(rlev) then
      -- that mission was left (quit) before completing it: no marks; the record waits in the hideout
      -- for that mission to be played again (it goes on there), any other level drops it
      tinsert(lines, format("%s: level %d was not completed: no marks", p, rlev))
      if num ~= S.cfg.hideout then S.Save("r." .. p, nil) end
    elseif rlev then
      local new = S.Severity(Scars.Num(r, 2) or 100, Scars.Num(r, 3) or 0)
      if new > 0 and new >= S.Shown(sev, age) then
        sev, age = new, 0
        tinsert(lines, format("%s ended level %d at %d %% (%d knock-outs): new marks %.2f", p, rlev, Scars.Num(r, 2) or 100,
                              Scars.Num(r, 3) or 0, new))
      end
      S.Save("r." .. p, nil)
    end
    local shown = S.Shown(sev, age)
    if sev > 0 and shown <= 0 then
      S.Save("s." .. p, nil)
      tinsert(lines, p .. ": healed")
    elseif sev > 0 then
      S.Save("s." .. p, format("%.2f %d", sev, age))
      local ids = S.people[p]
      local i = 1
      while ids[i] do
        if ModScar then ModScar(ids[i], shown) end
        i = i + 1
      end
      tinsert(lines, format("%s: marks %.2f (severity %.2f, %d missions)", p, shown, sev, age))
    end
  end
  if aging then S.Save("last", num) end
  local i = 1
  while lines[i] do ModLog(lines[i]) i = i + 1 end
  if not ModScar then ModLog("this game has no ModScar (an older WarriorsReborn): marks are recorded but not shown") end
  local n = 0
  for h, w in S.watch do n = n + 1 end
  ModLog(format("level %d: watching %d Warriors%s", num, n, (aging and ", a mission later") or ""))
  if not S.ticking then
    S.ticking = 1
    ScheduleFunc("Scars.Tick", S.cfg.poll)
  end
end
