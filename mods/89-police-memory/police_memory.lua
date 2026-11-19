-- The police remember you (docs/MODDING.md, "Police memory"). One file, hook `boot` (every Lua
-- state: once at start-up, which it ignores, and at the start of every level).
--
-- Each level belongs to a district (the hub and the Coney missions are "coney"). While you play, the
-- mod measures the trouble you cause there with the game's own wanted state (GangIsWanted: every
-- crime the police notice makes the Warriors wanted for 10 s) and the cops you beat. That notoriety
-- is kept per district and per save profile (ModSave). At the next level load in the same district it
-- turns into more patrols (more beat cops, spawned sooner), more responders per crime and a larger
-- share of the cops reacting. Notoriety goes down while you stay out of trouble there and halves for
-- every level played in another district. Nothing changes at low notoriety.
--
-- Every number is a field of PoliceMemory.cfg. Nothing of the game is copied: the mod changes the
-- arguments of the game's own calls (GangAddSpawner, CfgCrimeResponders, GangCreate) on their way.
-- Lua 4.0: varargs arrive in `arg`; call(f, arg) calls f with them; floor/min/max are globals.

if not PoliceMemory then
  PoliceMemory = {
    cfg = {
      -- notoriety (0..max) earned in the district you are in
      wanted_start = 8,         -- each time the Warriors become wanted (a new incident)
      wanted_per_s = 0.5,       -- per second of being wanted
      cop_beaten = 6,           -- each cop knocked out or killed ...
      hurt = 95,                -- ... after he was seen hurt (health % under this)
      max = 100,
      -- decay
      calm_ms = 30000,          -- every 30 s of game time not wanted in the district ...
      calm_loss = 1,            -- ... notoriety goes down by this
      away_factor = 0.5,        -- times this for every level played in another district
      -- tiers: notoriety at the start of a visit >= tiers[t] gives tier t (0 = nothing changes)
      tiers = {20, 45, 70},
      responders = {1, 1, 2},   -- CfgCrimeResponders(type, n): n + this (a 0 stays 0) ...
      responders_max = 5,       -- ... at most this (retail 2-4; Warriors Mode adds 1, at most 4)
      patrol_extra = {1, 1, 2}, -- police patrol / responder spawners (GangAddSpawner arg 10) + this
      interval = {0.75, 0.6, 0.5},   -- their spawn interval (arg 9) times this
      interval_min = 500,       -- ms
      respond = {45, 60, 75},   -- GangSetRespondPercentage of police gangs (retail 30)
      hint_ms = 8000,           -- the HUD hint (tier >= 1) not before this much of the level's game time ...
      street_ms = 2000,         -- ... and once the player has been out on the street this long
      hint_show_ms = 6000,      -- the hint goes away after this (or when he leaves the street)
      free_patrol_tier = 2,     -- from this tier the hub's beat-cop patrol walks in free roam too
      tick_ms = 500,
      save_ms = 3000,           -- ModSave at most this often while the value changes
      status_ms = 0,            -- > 0: log the police head counts this often (tests)
      front_end_level = 100,    -- level100 = the menus; Quick Rumble levels are above it
      aotn_from = 60,           -- Armies of the Night levels 60-64: left alone
      aotn_to = 64,
      police_gang = 1,          -- GangCreate(1, ...): the game's police gangs
      patrol_kinds = {[7] = 1, [4] = 1},   -- GangAddSpawner arg 11: 7 patrol, 4 responder
    },
    -- retail responders per crime type, as the engine resets them at every level load (0x338bb0)
    retail = {[0] = 2; 3, 3, 2, 2, 2, 2, 2, 2, 4, 2, 2, 2, 2},
    -- config_easy's own calls (Normal and Hard make none)
    easy = {[0] = 1; 2, 2, 2, 2, 2, 2, 2, 1, 4, 1, 1, 2, 1},
    -- level -> district (the game's own mission place names; the hub is Coney)
    districts = {
      [95] = "coney", [99] = "coney", [80] = "coney", [81] = "coney", [86] = "coney",
      [84] = "coney", [92] = "coney",
      [87] = "east_coney", [93] = "east_coney", [82] = "east_coney", [83] = "east_coney",
      [34] = "riverside", [54] = "riverside", [2] = "tremont", [52] = "tremont",
      [3] = "soho", [5] = "east_harlem", [31] = "pelham", [14] = "bensonhurst",
      [9] = "trainyard", [51] = "gunhill", [55] = "bowery", [20] = "chinatown",
      [11] = "harlem", [7] = "prospect_park",
    },
    cops = {[245] = 1, [246] = 1, [247] = 1, [248] = 1, [249] = 1, [250] = 1, [251] = 1,
            [252] = 1, [253] = 1, [254] = 1, [255] = 1, [256] = 1, [257] = 1, [258] = 1,
            [263] = 1, [264] = 1},   -- CfgChar ids of the police characters
    text = {
      en = "The cops are looking for you here",
      es = "La polic\237a te busca por ac\225",   -- Latin-1 escapes: the game font
    },
  }

  -- ---- pure helpers (tests/test_mod_police_memory.py runs them without the game) ----

  PoliceMemory.tier = function(score)
    local t = 0
    local i = 1
    while PoliceMemory.cfg.tiers[i] do
      if score >= PoliceMemory.cfg.tiers[i] then t = i end
      i = i + 1
    end
    return t
  end

  -- notoriety after `away` levels played in other districts
  PoliceMemory.decay = function(score, away)
    local s = score
    for i = 1, away do s = s * PoliceMemory.cfg.away_factor end
    if s < 0.5 then s = 0 end
    return floor(s * 10 + 0.5) / 10
  end

  PoliceMemory.responders = function(n, t)
    local c = PoliceMemory.cfg
    if type(n) ~= "number" or n < 1 or t < 1 then return n end
    return min(n + c.responders[t], max(n, c.responders_max))
  end

  -- "score,peak,crimes,cops,seen" <-> record
  PoliceMemory.parse = function(s)
    local r = {score = 0, peak = 0, crimes = 0, cops = 0, seen = 0}
    if type(s) ~= "string" then return r end
    local f = {}
    local rest = s
    while rest do
      local p = strfind(rest, ",", 1, 1)
      if p then tinsert(f, tonumber(strsub(rest, 1, p - 1)) or 0) rest = strsub(rest, p + 1)
      else tinsert(f, tonumber(rest) or 0) rest = nil end
    end
    r.score, r.peak, r.crimes, r.cops, r.seen = f[1] or 0, f[2] or 0, f[3] or 0, f[4] or 0, f[5] or 0
    return r
  end

  PoliceMemory.format = function(r)
    return format("%.1f,%.1f,%d,%d,%d", r.score, r.peak, r.crimes, r.cops, r.seen)
  end

  PoliceMemory.add = function(r, points)
    r.score = min(PoliceMemory.cfg.max, max(0, r.score + points))
    if r.score > r.peak then r.peak = r.score end
  end

  -- ---- game side ----

  PoliceMemory.log = function(s)
    -- during the hook ModLog names the mod itself; later (scheduled code) it says "log"
    if PoliceMemory.Log then PoliceMemory.Log(((PoliceMemory.booted and "police-memory: ") or "") .. s) end
  end

  PoliceMemory.level_id = function()
    if GetLevelId and GetCurrentLevelIndex then return GetLevelId(GetCurrentLevelIndex()) end
    return -1
  end

  PoliceMemory.district = function(lvl)
    return PoliceMemory.districts[lvl] or ("level" .. lvl)
  end

  PoliceMemory.store = function(force)
    local now = GetGameTime and GetGameTime() or 0
    if not force and PoliceMemory.saved_at and now - PoliceMemory.saved_at < PoliceMemory.cfg.save_ms then
      PoliceMemory.dirty = 1
      return
    end
    PoliceMemory.saved_at = now
    PoliceMemory.dirty = nil
    if PoliceMemory.Save then PoliceMemory.Save("d." .. PoliceMemory.here, PoliceMemory.format(PoliceMemory.rec)) end
  end

  -- At the level's start: decay for the levels played elsewhere, the tier for this visit.
  PoliceMemory.enter = function(lvl)
    local c = PoliceMemory.cfg
    local d = PoliceMemory.district(lvl)
    local load = PoliceMemory.Load or function() return nil end
    local n = tonumber(load("n")) or 0
    if load("last") ~= d then n = n + 1 end
    local r = PoliceMemory.parse(load("d." .. d))
    local away = 0
    if r.seen > 0 then away = max(0, n - r.seen - 1) end
    local before = r.score
    r.score = PoliceMemory.decay(r.score, away)
    r.seen = n
    PoliceMemory.here, PoliceMemory.rec, PoliceMemory.level = d, r, lvl
    PoliceMemory.t = PoliceMemory.tier(r.score)
    if PoliceMemory.Save then
      PoliceMemory.Save("n", n)
      PoliceMemory.Save("last", d)
      PoliceMemory.Save("d." .. d, PoliceMemory.format(r))
    end
    local t = PoliceMemory.t
    local fx = "nothing changes"
    if t >= 1 then
      fx = format("responders +%d (at most %d), patrols +%d spawned every x%.2f, %d%% of the cops react",
                  c.responders[t], c.responders_max, c.patrol_extra[t], c.interval[t], c.respond[t])
    end
    PoliceMemory.log(format("level %d (%s): notoriety %.1f (was %.1f, %d level(s) away; peak %.1f, %d crimes, %d cops beaten) -> tier %d: %s",
                            lvl, d, r.score, before, away, r.peak, r.crimes, r.cops, t, fx))
  end

  -- Responders: the retail numbers for this level start (or config_easy's), then the levels' own calls.
  PoliceMemory.wrap_responders = function(script)
    if not CfgCrimeResponders or PoliceMemory.orig_resp then return end
    PoliceMemory.orig_resp = CfgCrimeResponders
    CfgCrimeResponders = function(crime, n)
      return PoliceMemory.orig_resp(crime, PoliceMemory.responders(n, PoliceMemory.t))
    end
    if PoliceMemory.t >= 1 then
      local base = PoliceMemory.retail
      if script == "config_easy" then base = PoliceMemory.easy end
      for crime = 0, 13 do CfgCrimeResponders(crime, base[crime]) end
    end
  end

  -- "a.b.c" -> the function the game would call for that name
  PoliceMemory.resolve = function(name)
    local obj, rest = nil, name
    while 1 do
      local p = strfind(rest, ".", 1, 1)
      local key = rest
      if p then key = strsub(rest, 1, p - 1) end
      if obj == nil then obj = getglobal(key)
      elseif type(obj) == "table" then obj = obj[key]
      else return nil end
      if not p then return obj end
      rest = strsub(rest, p + 1)
    end
  end

  PoliceMemory.police = {}      -- police gang handle -> name
  PoliceMemory.watch = {}       -- cop handle -> his health % at the last tick
  PoliceMemory.spawned = {}     -- spawner callback wrappers: index -> original callback name
  PoliceMemory.names = {}       -- index -> the wrapper name handed to GangAddSpawner (kept referenced)
  PoliceMemory.spawns = {}      -- police gang -> characters its spawners made

  PoliceMemory.add_cop = function(h)
    if not h or h == NilHandle or not HuGetCharType then return end
    if PoliceMemory.cops[HuGetCharType(h)] then PoliceMemory.watch[h] = 100 end
  end

  PoliceMemory.wrap_gangs = function()
    if PoliceMemory.orig_create then return end
    local c = PoliceMemory.cfg
    PoliceMemory.orig_create = GangCreate
    PoliceMemory.orig_spawner = GangAddSpawner
    PoliceMemory.orig_hucreate = HuCreate
    if GangCreate then
      GangCreate = function(...)
        local g = call(PoliceMemory.orig_create, arg)
        if arg[1] == PoliceMemory.cfg.police_gang and g and g ~= NilHandle then
          PoliceMemory.police[g] = tostring(arg[2])
          local t = PoliceMemory.t
          if t >= 1 and GangSetRespondPercentage then GangSetRespondPercentage(g, PoliceMemory.cfg.respond[t]) end
        end
        return g
      end
    end
    if GangAddSpawner then
      -- GangAddSpawner(gang, name, ?, chars, model, pos, heading, ?, interval ms, count, kind, ..., callback (14), ...)
      GangAddSpawner = function(...)
        local c = PoliceMemory.cfg
        local t = PoliceMemory.t
        if PoliceMemory.police[arg[1]] and c.patrol_kinds[arg[11]] and arg.n >= 14 then
          if t >= 1 and type(arg[9]) == "number" and type(arg[10]) == "number" and arg[10] >= 1 then
            local n0, i0 = arg[10], arg[9]
            arg[10] = arg[10] + c.patrol_extra[t]
            arg[9] = max(c.interval_min, floor(arg[9] * c.interval[t] + 0.5))
            PoliceMemory.log(format("%s spawner %s (%s): %d at a time -> %d, every %d -> %d ms",
                                    (arg[11] == 4 and "responder") or "patrol", tostring(arg[2]),
                                    PoliceMemory.police[arg[1]], n0, arg[10], i0, arg[9]))
          end
          local k = getn(PoliceMemory.spawned) + 1
          PoliceMemory.spawned[k] = arg[14]
          local gang = arg[1]
          PoliceMemory["Spawned" .. k] = function(...)
            PoliceMemory.spawns[%gang] = (PoliceMemory.spawns[%gang] or 0) + 1
            PoliceMemory.add_cop(arg[1])
            local f = PoliceMemory.spawned[%k]
            if type(f) == "string" then f = PoliceMemory.resolve(f) end
            if type(f) == "function" then return call(f, arg) end
          end
          PoliceMemory.names[k] = "PoliceMemory.Spawned" .. k   -- kept: the engine gets a char pointer
          arg[14] = PoliceMemory.names[k]
        end
        return call(PoliceMemory.orig_spawner, arg)
      end
    end
    if HuCreate then
      HuCreate = function(...)
        local h = call(PoliceMemory.orig_hucreate, arg)
        PoliceMemory.add_cop(h)
        return h
      end
    end
  end

  -- 1 while the player is out on a street: not inside the hideout (the hub's InsideClubhouse), not in a
  -- scene (HuIsInScene) and not in one of the hub's mini games.
  PoliceMemory.on_street = function()
    if not player or player == NilHandle then return nil end
    if InsideClubhouse then return nil end
    if HuIsInScene and HuIsInScene(player) then return nil end
    if MINIGAME or (type(roam) == "table" and (roam.currentGame or roam.miniLoading)) then return nil end
    return 1
  end

  -- The hub keeps its beat-cop patrol off in free roam except at two story checkpoints
  -- (coney.CopsOnNow, level95_coney); a hot Coney turns it on: AddBeatCop is the hub's own spawner
  -- (the GangAddSpawner wrapper above adds the tier's extra cops), and the hub's own TurnOffCops /
  -- TurnOnCops take it away for a mini game and bring it back after.
  PoliceMemory.free_patrols = function()
    if PoliceMemory.patrols_on or type(coney) ~= "table" or not coney.TurnOnCops then return end
    PoliceMemory.patrols_on = 1
    if coney.CopsOnNow then return end
    coney.CopsOnNow = true
    if AddBeatCop and not GangBeatCop then AddBeatCop() end
    PoliceMemory.log("free roam: the hub's beat-cop patrol is on (tier " .. PoliceMemory.t .. ")")
  end

  -- The hint: the game's tutorial line (HUDSetTutorialText, the small text the game uses for tips),
  -- taken away after hint_show_ms.
  PoliceMemory.Hint = function()
    if not player or player == NilHandle or not HUDSetTutorialText then return end
    PoliceMemory.hinted = 1
    if GetLanguage and GetLanguage() == 1 then PoliceMemory.msg = PoliceMemory.text.es
    else PoliceMemory.msg = PoliceMemory.text.en end
    HUDSetTutorialText(PoliceMemory.msg, 1)   -- the game keeps the pointer: the string stays referenced
    PoliceMemory.hint_until = (GetGameTime and GetGameTime() or 0) + PoliceMemory.cfg.hint_show_ms
    PoliceMemory.log("hint shown: " .. PoliceMemory.msg)
  end

  PoliceMemory.HideHint = function()
    PoliceMemory.hint_until = nil
    -- a tip the game showed since then stays (HUDCheckTutorialText: is this text the one shown)
    if HUDCheckTutorialText and not HUDCheckTutorialText(PoliceMemory.msg) then return end
    if HUDSetTutorialText then HUDSetTutorialText(nil) end
    PoliceMemory.log("hint gone")
  end

  PoliceMemory.Status = function()
    local s = ""
    for g, name in PoliceMemory.police do
      s = s .. format(" %s=%d (spawned %d)", name, (GangGetHeadCount and GangGetHeadCount(g)) or -1,
                      PoliceMemory.spawns[g] or 0)
    end
    PoliceMemory.log(format("status t=%d s: notoriety %.1f, wanted %s, police%s",
                            floor((GetGameTime() or 0) / 1000), PoliceMemory.rec.score,
                            (PoliceMemory.wanted and "yes") or "no", s))
  end

  -- Every tick_ms of game time: the wanted state, beaten cops, calm time.
  PoliceMemory.Tick = function()
    local c = PoliceMemory.cfg
    local r = PoliceMemory.rec
    local now = GetGameTime()
    local dt = now - (PoliceMemory.last_tick or now)
    PoliceMemory.last_tick = now
    if dt < 0 or dt > 5000 then dt = 0 end      -- a pause or a reload of the scheduler
    local changed = nil
    local w = GangWarriors and GangIsWanted and GangIsWanted(GangWarriors, true)
    if w and not PoliceMemory.wanted then
      PoliceMemory.wanted = now
      r.crimes = r.crimes + 1
      PoliceMemory.add(r, c.wanted_start)
      changed = 1
      PoliceMemory.log(format("wanted (incident %d here): notoriety %.1f", r.crimes, r.score))
    elseif w then
      PoliceMemory.add(r, c.wanted_per_s * dt / 1000)
      changed = 1
    elseif PoliceMemory.wanted then
      PoliceMemory.log(format("no longer wanted after %.1f s: notoriety %.1f (peak %.1f)",
                              (now - PoliceMemory.wanted) / 1000, r.score, r.peak))
      PoliceMemory.wanted = nil
      PoliceMemory.calm = 0
      PoliceMemory.store(1)
    else
      PoliceMemory.calm = (PoliceMemory.calm or 0) + dt
      if PoliceMemory.calm >= c.calm_ms then
        PoliceMemory.calm = PoliceMemory.calm - c.calm_ms
        if r.score > 0 then
          PoliceMemory.add(r, -c.calm_loss)
          changed = 1
        end
      end
    end
    -- a cop counts as beaten when he goes down after being hurt (a cop the game removes reads 0 % at
    -- once, from full health, and must not count)
    local gone = {}
    for h, last in PoliceMemory.watch do
      local dead = HuIsDead and HuIsDead(h)
      local hp = HuGetHealthPercent and HuGetHealthPercent(h)
      if dead or not hp or hp <= 0.5 then
        tinsert(gone, h)
        if last < c.hurt then
          r.cops = r.cops + 1
          PoliceMemory.add(r, c.cop_beaten)
          changed = 1
          PoliceMemory.log(format("cop beaten (%d here): notoriety %.1f", r.cops, r.score))
        end
      else
        PoliceMemory.watch[h] = hp
      end
    end
    local i = 1
    while gone[i] do PoliceMemory.watch[gone[i]] = nil i = i + 1 end
    if changed or PoliceMemory.dirty then PoliceMemory.store() end
    local street = PoliceMemory.on_street()
    if street then PoliceMemory.street = (PoliceMemory.street or 0) + dt else PoliceMemory.street = 0 end
    if PoliceMemory.t >= c.free_patrol_tier and street then PoliceMemory.free_patrols() end
    if PoliceMemory.hint_until and (now >= PoliceMemory.hint_until or not street) then PoliceMemory.HideHint() end
    if PoliceMemory.t >= 1 and not PoliceMemory.hinted and now >= c.hint_ms and PoliceMemory.street >= c.street_ms then
      PoliceMemory.Hint()
    end
    if c.status_ms > 0 and now - (PoliceMemory.status_at or -100000) >= c.status_ms then
      PoliceMemory.status_at = now
      PoliceMemory.Status()
    end
    ScheduleFunc("PoliceMemory.Tick", c.tick_ms)
  end

  PoliceMemory.start = function()
    ScheduleFunc("PoliceMemory.Tick", 2000)
    -- a level script that flushes every scheduled function also stops ours: start it again
    if FlushScheduledFuncs and not PoliceMemory.orig_flush then
      PoliceMemory.orig_flush = FlushScheduledFuncs
      FlushScheduledFuncs = function(...)
        local r = call(PoliceMemory.orig_flush, arg)
        if arg.n == 0 or arg[1] == nil then ScheduleFunc("PoliceMemory.Tick", PoliceMemory.cfg.tick_ms) end
        return r
      end
    end
  end
end

-- ---- what this hook does (boot of a Lua state) ----

PoliceMemory.Log = ModLog
PoliceMemory.Save = ModSave
PoliceMemory.Load = ModLoad
if not PoliceMemory.here then
  local lvl = PoliceMemory.level_id()
  local c = PoliceMemory.cfg
  if lvl > 0 and lvl < c.front_end_level and not (lvl >= c.aotn_from and lvl <= c.aotn_to) then
    PoliceMemory.enter(lvl)
    PoliceMemory.wrap_responders(MOD_SCRIPT)
    PoliceMemory.wrap_gangs()
    PoliceMemory.start()
  end
end
PoliceMemory.booted = 1
