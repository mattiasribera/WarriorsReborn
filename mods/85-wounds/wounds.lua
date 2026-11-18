-- Visible wounds (boot hook, every Lua state; see mod.ini and docs/MODDING.md "Visible wounds").
-- A fighter low on health moves slower: walking, running and sprinting all lose speed, through the
-- runtime's ModSpeed cap on the game's own per-character speed multiplier. Healing (flash) gives the
-- speed back; the game raises it again at its own rate over a few seconds.
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
        {below = 40, warriors = 0.8, others = 1},
        {below = 20, warriors = 0.65, others = 1},
      },
      warriors_gang_type = 0,   -- HuGetGangType of the Warriors (global.lua's SuperRunScene test)
      tick_ms = 200,            -- check period (game time)
    },
    watch = {},     -- handle -> 1 for every character the level creates (HuCreate, gang spawners)
    cap = {},       -- handle -> the cap set with ModSpeed (absent = 1)
    exempt = {},    -- handle -> 1 while a script runs it in a chase (HuSetTireless on, chase goals)
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
        if v < s then s = v end
      end
      i = i + 1
    end
    return s
  end

  -- ---- game side ----

  Wounds.modlog = ModLog
  -- code the game calls later logs as "[mods] log: ...": name the mod in the line
  Wounds.log = function(s) Wounds.modlog("wounds: " .. s) end
  Wounds.speed = ModSpeed

  Wounds.Add = function(h)
    if h and h ~= NilHandle then Wounds.watch[h] = 1 end
  end

  Wounds.SetExempt = function(h, on)
    if not h or h == NilHandle then return end
    if on and on ~= 0 then Wounds.exempt[h] = 1 else Wounds.exempt[h] = nil end
  end

  -- What one character should move at now (1 = his own speed).
  Wounds.want = function(h)
    if Wounds.exempt[h] then return 1 end
    if HuIsInScene and HuIsInScene(h) then return 1 end
    local warrior = HuGetGangType and HuGetGangType(h) == Wounds.cfg.warriors_gang_type
    return Wounds.scale(HuGetHealthPercent(h), warrior)
  end

  Wounds.Tick = function()
    if player and player ~= NilHandle then Wounds.Add(player) end
    if player2 and player2 ~= NilHandle then Wounds.Add(player2) end
    local gone = {}
    for h, _ in Wounds.watch do
      local alive = HuIsAlive and HuIsAlive(h)
      local want = 1
      if alive then want = Wounds.want(h) end
      local had = Wounds.cap[h] or 1
      if want ~= had then
        local m = Wounds.speed(h, want)
        if m then
          if want < 1 then
            Wounds.cap[h] = want
            Wounds.stats.set = Wounds.stats.set + 1
          else
            Wounds.cap[h] = nil
            Wounds.stats.released = Wounds.stats.released + 1
          end
          Wounds.log(format("character %d%s: health %.0f %% -> speed x%.2f", HuGetCharType(h) or -1,
                            (h == player and " (player)") or "", HuGetHealthPercent(h) or -1, want))
        else
          tinsert(gone, h)   -- not a live character any more
        end
      elseif not alive and had == 1 then
        tinsert(gone, h)
      end
    end
    local i = 1
    while gone[i] do
      Wounds.watch[gone[i]] = nil Wounds.cap[gone[i]] = nil Wounds.exempt[gone[i]] = nil
      i = i + 1
    end
    ScheduleFunc("Wounds.Tick", Wounds.cfg.tick_ms)
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
