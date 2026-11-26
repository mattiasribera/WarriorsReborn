-- Visible wounds (boot hook, every Lua state; see mod.ini and docs/MODDING.md "Visible wounds").
-- Low on health, the player cannot sprint (ModSprint): he runs at his normal run, which the game
-- plays at its own pace. His speed is never capped: the game animates him from his speed, so even
-- x0.85 made his run a slower, shorter cycle that read as slow motion.
-- His crew (AI) limps through the runtime's ModSpeed cap: the cap never goes under cfg.floor and
-- moves toward its new value at cfg.ramp_per_s, so a hit never brakes them at once. Healing (flash)
-- and a new level give sprint and speed back.
-- Lua 4.0: varargs arrive in `arg`; call(f, arg) calls f with them; outer locals are upvalues (%x).

if not Wounds then
  Wounds = {
    cfg = {
      -- health % below which a fighter moves at `warriors` (player and crew) or `others` (every
      -- other fighter) of his speed; the lowest step that applies wins. `others` is 1 (off): an AI
      -- fighter does not run a little slower, he drops from running to a slower gait (measured:
      -- caps 0.65-0.9 all gave about half his running speed, 0.95 nothing), so hurt enemies would
      -- reach you and flee at half speed, which makes fights easier. 0.9 turns them on.
      steps = {
        {below = 40, warriors = 0.9, others = 1},
        {below = 20, warriors = 0.85, others = 1},
      },
      no_sprint_below = 40,     -- the player (player, player2) cannot sprint under this health %
      floor = 0.85,             -- no step goes under this (the user saw x0.65 as slow motion)
      ramp_per_s = 0.05,        -- the cap moves toward its new value by this per second of game time
      warriors_gang_type = 0,   -- HuGetGangType of the Warriors (global.lua's SuperRunScene test)
      tick_ms = 200,            -- check period (game time)
    },
    watch = {},     -- handle -> 1 for every character the level creates (HuCreate, gang spawners)
    cap = {},       -- handle -> the cap set with ModSpeed (absent = 1)
    goal = {},      -- handle -> the cap he is ramping toward (logged once per change)
    seen = {},      -- handle -> 1 once his runtime cap was read (a cap left from before this state)
    exempt = {},    -- handle -> 1 while a script runs it in a chase (HuSetTireless on, chase goals)
    blocked = {},   -- player handle -> 1 while ModSprint blocks his sprint (as the runtime holds it)
    spawned = {},   -- spawner callback wrappers: index -> original callback
    stats = {set = 0, released = 0},
  }

  -- ---- pure helpers (tests/test_mod_wounds.py runs them without the game) ----

  -- The speed scale for a health percent: 1, or the lowest step below which hp lies.
  Wounds.scale = function(hp, warrior)
    local s = 1
    if type(hp) ~= "number" then return s end
    local steps = Wounds.cfg.steps
    local i = 1
    while steps[i] do
      if hp < steps[i].below then
        local v = (warrior and steps[i].warriors) or steps[i].others
        if v < Wounds.cfg.floor then v = Wounds.cfg.floor end
        if v < s then s = v end
      end
      i = i + 1
    end
    return s
  end

  -- 1 when a player at this health % may sprint, 0 when not.
  Wounds.sprints = function(hp)
    if type(hp) == "number" and hp < Wounds.cfg.no_sprint_below then return 0 end
    return 1
  end

  -- One ramp step: from the cap `had` toward `want`, at most `step`.
  Wounds.toward = function(had, want, step)
    if want < had then
      if had - want <= step then return want end
      return had - step
    end
    if want - had <= step then return want end
    return had + step
  end

  -- ---- game side ----

  Wounds.modlog = ModLog
  -- code the game calls later logs as "[mods] log: ...": name the mod in the line
  Wounds.log = function(s) Wounds.modlog("wounds: " .. s) end
  Wounds.speed = ModSpeed
  Wounds.sprint = ModSprint

  Wounds.IsPlayer = function(h)
    return (player and h == player) or (player2 and h == player2)
  end

  Wounds.Add = function(h)
    if h and h ~= NilHandle then Wounds.watch[h] = 1 end
  end

  Wounds.SetExempt = function(h, on)
    if not h or h == NilHandle then return end
    if on and on ~= 0 then Wounds.exempt[h] = 1 else Wounds.exempt[h] = nil end
  end

  -- What one character should move at now (1 = his own speed; always 1 for a player).
  Wounds.want = function(h)
    if Wounds.exempt[h] or Wounds.IsPlayer(h) then return 1 end
    if HuIsInScene and HuIsInScene(h) then return 1 end
    local warrior = HuGetGangType and HuGetGangType(h) == Wounds.cfg.warriors_gang_type
    return Wounds.scale(HuGetHealthPercent(h), warrior)
  end

  -- A player's sprint: blocked under cfg.no_sprint_below, allowed in a chase, a scene or when healed.
  Wounds.SprintCheck = function(h)
    if not Wounds.sprint then return end
    local may = 1
    if not Wounds.exempt[h] and not (HuIsInScene and HuIsInScene(h)) then
      may = Wounds.sprints(HuGetHealthPercent(h))
    end
    if Wounds.blocked[h] == nil then
      -- first sight in this Lua state: what the runtime still holds (a block from an earlier level)
      local now = Wounds.sprint(h)
      if not now then return end
      Wounds.blocked[h] = 1 - now
    end
    if 1 - may ~= Wounds.blocked[h] then
      if Wounds.sprint(h, may) then
        Wounds.blocked[h] = 1 - may
        local what = "sprint again"
        if may == 0 then what = "no sprint" end
        Wounds.log(format("character %d (player): health %.0f %% -> %s", HuGetCharType(h) or -1,
                          HuGetHealthPercent(h) or -1, what))
      end
    end
  end

  Wounds.Tick = function()
    local c = Wounds.cfg
    if player and player ~= NilHandle then Wounds.Add(player) end
    if player2 and player2 ~= NilHandle then Wounds.Add(player2) end
    local now = (GetGameTime and GetGameTime()) or 0
    local dt = now - (Wounds.last_tick or now)
    Wounds.last_tick = now
    if dt <= 0 or dt > 2000 then dt = c.tick_ms end   -- the first check, or a reload of the scheduler
    local step = c.ramp_per_s * dt / 1000
    local gone = {}
    for h, _ in Wounds.watch do
      local alive = HuIsAlive and HuIsAlive(h)
      local want = 1
      if alive then want = Wounds.want(h) end
      if alive and Wounds.IsPlayer(h) then Wounds.SprintCheck(h) end
      if not Wounds.seen[h] then
        -- a cap the runtime still holds for this handle (set in an earlier Lua state) ramps from there
        Wounds.seen[h] = 1
        local old = alive and Wounds.speed(h)
        if old and old < 1 then Wounds.cap[h] = old end
      end
      local had = Wounds.cap[h] or 1
      if want ~= had then
        if want ~= Wounds.goal[h] then
          Wounds.goal[h] = want
          Wounds.log(format("character %d%s: health %.0f %% -> speed x%.2f (from x%.2f, %.2f per s)",
                            HuGetCharType(h) or -1, (Wounds.IsPlayer(h) and " (player)") or "",
                            HuGetHealthPercent(h) or -1, want, had, c.ramp_per_s))
        end
        local nxt = Wounds.toward(had, want, step)
        local m = Wounds.speed(h, nxt)
        if m then
          if nxt < 1 then
            if had >= 1 then Wounds.stats.set = Wounds.stats.set + 1 end
            Wounds.cap[h] = nxt
          else
            Wounds.cap[h] = nil
            Wounds.stats.released = Wounds.stats.released + 1
          end
        else
          tinsert(gone, h)   -- not a live character any more
        end
      elseif not alive and had == 1 then
        tinsert(gone, h)
      end
    end
    local i = 1
    while gone[i] do
      local h = gone[i]
      Wounds.watch[h] = nil Wounds.cap[h] = nil Wounds.exempt[h] = nil Wounds.goal[h] = nil
      Wounds.seen[h] = nil Wounds.blocked[h] = nil
      i = i + 1
    end
    ScheduleFunc("Wounds.Tick", c.tick_ms)
  end

  -- "a.b.c" -> the function the game would call for that name
  Wounds.Resolve = function(name)
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

  Wounds.Wrap = function()
    Wounds.orig = {HuCreate = HuCreate, GangAddSpawner = GangAddSpawner, HuSetTireless = HuSetTireless,
                   GoalDevilRun = GoalDevilRun, GoalRunCarrotRun = GoalRunCarrotRun,
                   GoalLeadChase = GoalLeadChase, FlushScheduledFuncs = FlushScheduledFuncs}
    if HuCreate then
      HuCreate = function(...)
        local h = call(Wounds.orig.HuCreate, arg)
        Wounds.Add(h)
        return h
      end
    end
    -- argument 14 of GangAddSpawner names the function called with each character it spawns
    if GangAddSpawner then
      GangAddSpawner = function(...)
        if arg.n >= 14 then
          local n = getn(Wounds.spawned) + 1
          Wounds.spawned[n] = arg[14]
          Wounds["Spawned" .. n] = function(...)
            Wounds.Add(arg[1])
            local f = Wounds.spawned[%n]
            if type(f) == "string" then f = Wounds.Resolve(f) end
            if type(f) == "function" then return call(f, arg) end
          end
          arg[14] = "Wounds.Spawned" .. n
        end
        return call(Wounds.orig.GangAddSpawner, arg)
      end
    end
    -- scripted chases: the game makes the chasers tireless (HuSetTireless(h, 1)) and runs the
    -- runners with chase goals; those keep their full speed whatever their health
    if HuSetTireless then
      HuSetTireless = function(...)
        Wounds.SetExempt(arg[1], arg[2])
        return call(Wounds.orig.HuSetTireless, arg)
      end
    end
    for _, name in {"GoalDevilRun", "GoalRunCarrotRun", "GoalLeadChase"} do
      if Wounds.orig[name] then
        setglobal(name, function(...)
          Wounds.SetExempt(arg[1], 1)
          return call(Wounds.orig[%name], arg)
        end)
      end
    end
    -- a level script that flushes every scheduled function also stops the check: start it again
    if FlushScheduledFuncs then
      FlushScheduledFuncs = function(...)
        local r = call(Wounds.orig.FlushScheduledFuncs, arg)
        if arg.n == 0 or arg[1] == nil then ScheduleFunc("Wounds.Tick", Wounds.cfg.tick_ms) end
        return r
      end
    end
  end

  if not Wounds.speed then
    ModLog("this game has no ModSpeed (mod runtime older than 1.3): nothing changed")
  elseif HuCreate and ScheduleFunc and HuGetHealthPercent then
    Wounds.Wrap()
    ScheduleFunc("Wounds.Tick", 1000)
  end
end
