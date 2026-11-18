-- More hats fly, part 1 (boot: runs before the game's config scripts).
-- Wraps CfgObj and CfgHat while config_preload2 sets up the hats:
--   * the police caps (dyn_cops_va/vb/vc) become hats that fly: the game sets them up as "riot
--     hats" (OBJECT.TYPE_RIOT_HAT, no physics), the kind it uses for hair and helmets that never
--     leave the head, so no cop ever lost his cap. Here they get the settings of the game's own
--     flying hats (the pimps' hats: OBJECT.TYPE_HAT, PHYS.OBB on AXIS.Z, picked up with
--     ANIM.LeftHandHatPickUp), so a cop loses his cap when he is knocked down or killed, like the
--     gang members do, and the cap lies on the ground and can be picked up;
--   * we remember each model's hat object (its box, offset, rotation and type), and every "hat"
--     pose the game registers: CfgHat(anim, anim2, model, offset, rotation).
-- Every call still goes to the game. extend.lua uses what was recorded.
-- Lua 4.0: varargs arrive in the table `arg`; call(f, arg) calls f with them.
if not ExtraHats then
  ExtraHats = {geometry = {}, poses = {}, caps = 0, cfgobj = CfgObj, cfghat = CfgHat}
  ExtraHats.police = {dyn_cops_va = 1, dyn_cops_vb = 1, dyn_cops_vc = 1}

  ExtraHats.key = function(t)
    if type(t) ~= "table" then return "?" end
    local s, i = "", 1
    while t[i] do s = s .. format("%.6g,", t[i]) i = i + 1 end
    return s
  end

  CfgObj = function(...)
    if arg[2] == "hat_object" and type(arg[1]) == "string" and arg.n >= 22 then
      -- arg 8 = box, 9 = physics, 10 = axis, 14 = pick-up animation, 19 = object type
      -- (hat or riot hat), 21 = offset, 22 = rotation
      if ExtraHats.police[arg[1]] and OBJECT and arg[19] == OBJECT.TYPE_RIOT_HAT and PHYS and AXIS and ANIM then
        arg[9] = PHYS.OBB
        arg[10] = AXIS.Z
        arg[14] = ANIM.LeftHandHatPickUp
        arg[19] = OBJECT.TYPE_HAT
        ExtraHats.caps = ExtraHats.caps + 1
      end
      ExtraHats.geometry[arg[1]] = ExtraHats.key(arg[8]) .. "|" .. tostring(arg[19]) .. "|" ..
                                   ExtraHats.key(arg[21]) .. "|" .. ExtraHats.key(arg[22])
    end
    return call(ExtraHats.cfgobj, arg)
  end

  CfgHat = function(a, b, model, offset, rotation)
    if type(model) == "string" then
      local p = ExtraHats.poses[model] or {count = 0}
      ExtraHats.poses[model] = p
      local k = tostring(a) .. ":" .. tostring(b)
      if not p[k] then
        p[k] = {a, b, offset, rotation}
        p.count = p.count + 1
      end
    end
    return ExtraHats.cfghat(a, b, model, offset, rotation)
  end

  -- Police officers (CfgChar kind 1: street, rookie and transit cops; ids from config_preload2).
  -- Even with a flying cap the game never knocks it off them: in the game's fight code a cop
  -- is never knocked out of his cap (a cop with the same model and cap under a non-police
  -- character id, 266, loses it; 253 never does), and a beaten cop is knocked out (health
  -- stays just above 0) and later removed with his cap still on. So this mod watches every cop
  -- the level creates (HuCreate and gang spawners) and knocks the cap off with the game's own
  -- HuDrop(cop, 1, 0, 0) (the same code that knocks a gang member's hat off) when
  --   * a hit takes the cop under the hat threshold of his power class (CfgPowerClass argument
  --     10, the rule the game applies to gang members: 20 % for cops on Normal), or
  --   * one hit takes 10 % or more of his health (a heavy blow that knocks him down), or
  --   * he is knocked out or dies.
  ExtraHats.cops = {[245] = 1, [246] = 1, [247] = 1, [248] = 1, [249] = 1, [250] = 1,
                    [251] = 1, [252] = 1, [253] = 1, [254] = 1, [255] = 1, [256] = 1,
                    [257] = 1, [258] = 1, [263] = 1, [264] = 1}
  ExtraHats.copclass = 9   -- the power class of every cop above
  ExtraHats.heavy = 10     -- % of health taken by one hit that knocks the cap off
  ExtraHats.ko = 0.5       -- % of health at which a cop counts as knocked out
  ExtraHats.watch = {}     -- cop handle -> last health %
  ExtraHats.tick = -100000
  ExtraHats.spawned = {}   -- spawner callback wrappers: index -> original callback name
  ExtraHats.drops = 0
  ExtraHats.cfgpower = CfgPowerClass
  ExtraHats.hucreate = HuCreate
  ExtraHats.gangaddspawner = GangAddSpawner

  -- threshold of the cops' power class, as the difficulty config sets it up (config_normal...)
  if CfgPowerClass then
    CfgPowerClass = function(...)
      if arg[1] == ExtraHats.copclass and type(arg[10]) == "number" then ExtraHats.threshold = arg[10] * 100 end
      return call(ExtraHats.cfgpower, arg)
    end
  end

  ExtraHats.Add = function(h)
    if not h or h == NilHandle or not HuGetCharType or not HuDrop then return end
    if not ExtraHats.cops[HuGetCharType(h)] then return end
    ExtraHats.watch[h] = -1
    ExtraHats.seen = (ExtraHats.seen or 0) + 1
    if GetGameTime() - ExtraHats.tick > 2000 then
      ExtraHats.tick = GetGameTime()
      ScheduleFunc("ExtraHats.Watch", 100)
    end
  end

  ExtraHats.Watch = function()
    ExtraHats.tick = GetGameTime()
    local thr = ExtraHats.threshold or 20
    local gone = {}
    for h, last in ExtraHats.watch do
      if HuHasHat(h) == NilHandle then
        -- no cap (not put on yet, already off, or the cop was removed): forget him once it is off
        if last >= 0 then tinsert(gone, h) end
      else
        local hp = HuGetHealthPercent(h) or 0
        local dead = HuIsDead and HuIsDead(h)
        if dead or hp <= ExtraHats.ko or
           (last >= 0 and hp < last and (hp < thr or last - hp >= ExtraHats.heavy)) then
          ExtraHats.Lost(h, HuHasHat(h))
          HuDrop(h, 1, 0, 0)
          ExtraHats.drops = ExtraHats.drops + 1
          ModLog(format("cap knocked off cop %d (health %.1f %%%s)", HuGetCharType(h), hp, (dead and ", dead") or ""))
        end
        ExtraHats.watch[h] = hp
      end
    end
    local i = 1
    while gone[i] do ExtraHats.watch[gone[i]] = nil i = i + 1 end
    ScheduleFunc("ExtraHats.Watch", 100)
  end

  -- After he gets up, a cop picks his cap up if it still lies near (within ExtraHats.reach m) and
  -- nobody is hitting him (no player within ExtraHats.clear m), with the game's own "get my hat"
  -- goal GoalGetItem(cop, 4, cap) (the goal the game's thug brain gives itself for a lost hat).
  -- Checked every 500 ms from 2.5 s after the knock-off (he is down meanwhile); two tries, then he
  -- leaves it. Picked up again, the cap can be knocked off again.
  ExtraHats.lost = {}      -- cop handle -> {cap, time, tries, last try}
  ExtraHats.reach = 5
  ExtraHats.clear = 3
  ExtraHats.fetched = 0
  ExtraHats.Quiet = function() end
  ExtraHats.Pos = function(h)
    local p = call(GetPosition, {h}, "x", ExtraHats.Quiet)
    if p == nil or type(p) == "number" or type(p.x) ~= "number" then return nil end
    return {p.x, p.y, p.z}
  end
  ExtraHats.Dist = function(a, b)
    local dx, dy = a[1] - b[1], a[2] - b[2]
    return sqrt(dx * dx + dy * dy)
  end
  ExtraHats.Lost = function(h, cap)
    if not GoalGetItem or cap == NilHandle then return end
    ExtraHats.lost[h] = {cap, GetGameTime(), 0, 0}
    if not ExtraHats.tending then
      ExtraHats.tending = 1
      ScheduleFunc("ExtraHats.Tend", 500)
    end
  end
  ExtraHats.Tend = function()
    local H, now, done, any = ExtraHats, GetGameTime(), {}, nil
    for h, e in H.lost do
      any = 1
      local p, c = H.Pos(h), H.Pos(e[1])
      local q = player and player ~= NilHandle and H.Pos(player)
      if HuHasHat(h) == e[1] then
        H.fetched = H.fetched + 1
        ModLog(format("cop %d picked his cap up", HuGetCharType(h)))
        H.watch[h] = -1   -- worn again: it can be knocked off again
        tinsert(done, h)
      elseif e[3] > 0 and now - e[4] <= 8000 and p and not (player and HuHasHat(player) == e[1]) then
        -- on his way: the cap in his hand has no world position until it is on his head
      elseif not p or not c or HuHasHat(h) ~= NilHandle or (HuIsDead and HuIsDead(h)) or (player and HuHasHat(player) == e[1]) or
             H.Dist(p, c) > H.reach + 1 or (e[3] >= 2 and now - e[4] > 8000) then
        tinsert(done, h)   -- gone, taken, too far, or he gave up
      elseif now - e[2] >= 2500 and e[3] < 2 and (e[3] == 0 or now - e[4] > 8000) and H.Dist(p, c) <= H.reach and
             (HuGetHealthPercent(h) or 0) > H.ko and not (q and H.Dist(p, q) < H.clear) then
        e[3], e[4] = e[3] + 1, now
        call(GoalGetItem, {h, 4, e[1]}, "x", H.Quiet)
        ModLog(format("cop %d goes back for his cap (%.1f m)", HuGetCharType(h), H.Dist(p, c)))
      end
    end
    local i = 1
    while done[i] do H.lost[done[i]] = nil i = i + 1 end
    if any then ScheduleFunc("ExtraHats.Tend", 500) else H.tending = nil end
  end

  -- "a.b.c" -> the function the game would call for that name
  ExtraHats.Resolve = function(name)
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

  if HuCreate then
    HuCreate = function(...)
      local h = call(ExtraHats.hucreate, arg)
      ExtraHats.Add(h)
      return h
    end
  end

  -- argument 14 of GangAddSpawner names the function called with each character it spawns
  if GangAddSpawner then
    GangAddSpawner = function(...)
      if arg.n >= 14 then
        local n = getn(ExtraHats.spawned) + 1
        ExtraHats.spawned[n] = arg[14]
        local name = "ExtraHats.Spawned" .. n
        ExtraHats["Spawned" .. n] = function(...)
          ExtraHats.spawns = (ExtraHats.spawns or 0) + 1
          ExtraHats.Add(arg[1])
          local f = ExtraHats.spawned[%n]
          if type(f) == "string" then f = ExtraHats.Resolve(f) end
          if type(f) == "function" then return call(f, arg) end
        end
        arg[14] = name
      end
      return call(ExtraHats.gangaddspawner, arg)
    end
  end
end
