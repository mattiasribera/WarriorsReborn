-- Dirty fights: falling accessories (boot: runs before the game's config scripts and at the start
-- of every level, before the level script creates its characters).
--
-- 1. Headgear the game keeps on the head. The game sets beanies, caps and scarves up like hair
--    and helmets: OBJECT.TYPE_RIOT_HAT, no physics, never leaves the head. In the front end
--    (config_preload2) the mod gives the headgear in DirtyFights.headgear the settings of the
--    game's own flying hats (the pimps': OBJECT.TYPE_HAT, PHYS.OBB on AXIS.Z, picked up with
--    ANIM.LeftHandHatPickUp), as mods/10-extra-hats does for the police caps.
-- 2. Sunglasses modelled into a few heads (with the meshes built by tools/mod_meshes): each pair is
--    cut out as the mesh of a spare hat object (a hat no character of the game wears), put on its
--    wearer with HuPlaceHatOnHead a moment after the level script creates him. The spare's frame is
--    the head joint's (attach offset and knock-off poses made identity). The runtime's instance
--    group "<mod>/glasses-..." draws, per character, the face without the glasses and the hat as
--    the glasses once it has left his head (docs/MODDING.md, "Instance groups"). While he has lost
--    them, the mod gives that group his position every 100 ms (ModMeshPoint), so he is still drawn
--    without them after he leaves the view and comes back.
-- 3. Knock-off, the rule of the police caps (mods/10-extra-hats): every 100 ms each watched
--    character that still wears one of these hats loses it with the game's own
--    HuDrop(h, 1, 0, 0) when a hit takes him under 20 % of his health (the hat threshold the game
--    gives most fighting classes on Normal, CfgPowerClass argument 10; the game also applies each
--    class's own threshold to these hats now that they are TYPE_HAT), when one hit takes 10 % of
--    his health or more, and when he is knocked out or dies. The hat falls, stays on the ground
--    and can be picked up and worn.
-- 4. Glasses can break (with the meshes): 1.5 s after a pair fell, if it still lies where it fell,
--    it breaks with a chance that grows with the hit (DirtyFights.BreakChance): the pair is removed
--    (ObjDestroy) and DirtyFights.host, a newspaper nothing in the game places (a plain object:
--    it cannot be picked up), is spawned in its place, drawn by the placed group "<mod>/broken-..."
--    as the broken pair (snapped, a lens cracked, the other in shards) only where the mod put a
--    point; the point follows the object if it is kicked.
-- 5. Retrieval: a character who lost a hat the game throws (except the police caps: extra-hats)
--    goes back for it with the game's own "get my hat" goal, GoalGetItem(h, 4, hat) (the goal the
--    game's thug brain gives itself), when the fight has calmed down: he is alive and conscious,
--    no player is within DirtyFights.calm m and he has no enemies (BrHasEnemies) for 1.5 s, and
--    the hat lies within DirtyFights.reach m of him. Some do (DirtyFights.fetch); one gives up
--    when the hat is gone (picked up, broken) or farther.
-- Lua 4.0: varargs arrive in the table `arg`; call(f, arg) calls f with them.
if not DirtyFights then
  DirtyFights = {
    cfgobj = CfgObj, cfghat = CfgHat,
    hucreate = HuCreate, gangaddspawner = GangAddSpawner,
    id = MOD_ID,
    -- riot hats that are a whole accessory (no hair, helmet or visor in the same mesh), by model
    -- name: their wearers (CfgChar) in config_preload2
    headgear = {
      dyn_sun = 1,        -- sunglasses (civl_ct_eb)
      dyn_tr_cw = 1,      -- gold-rimmed glasses (civl_tr_cw, civl_ct_ma1)
      dyn_scps = 1,       -- black cap and sunglasses (civl_scps)
      dyn_we_ma2 = 1,     -- red cap and sunglasses (civl_we_ma2)
      dyn_beansie = 1,    -- the Destroyers lieutenant's winter hat (dest_lt_be)
      dyn_bm_vb8 = 1,     -- winter hat (civl_bm_vb8)
      dyn_bm_vb9 = 1,     -- red and white cap (civl_bm_vb9)
      dyn_samo1 = 1,      -- Satan's Mothers do-rag (samo_so_va1)
    },
    -- sunglasses: CfgChar id -> spare hat object; spare -> its mesh group (needs the meshes)
    glasses = {[119] = "dyn_civl_hl_ho",   -- Vargas (hurr_va)
               [64] = "dyn_pl_dr"},        -- Masai, the Riffs (riff_bo_va1)
    spares = {dyn_civl_hl_ho = "glasses-vargas", dyn_pl_dr = "glasses-masai"},
    -- glasses that can break -> the placed group drawing their broken version on the host
    breakable = {dyn_civl_hl_ho = "broken-vargas", dyn_pl_dr = "broken-masai",
                 dyn_sun = "broken-sun", dyn_tr_cw = "broken-trcw"},
    host = "dyn_s_newspaper_c",   -- a prop nothing in the game (or another bundled mod) uses: the broken pair on the ground
    police = {dyn_cops_va = 1, dyn_cops_vb = 1, dyn_cops_vc = 1},   -- mods/10-extra-hats' caps
    low = 20,          -- % of health under which a hit knocks the hat off
    heavy = 10,        -- % of health taken by one hit that knocks the hat off
    ko = 0.5,          -- % of health at which a character counts as knocked out
    settle = 1500,     -- ms after the fall: the pair lies still and may break
    fetch = 0.6,       -- chance that a character goes back for his hat
    calm = 8,          -- m: no player this close
    reach = 7,         -- m: the hat at most this far
    watch = {},        -- character handle -> {hat handle, last health %}
    lost = {},         -- character handle -> {hat, name, time, hit %, ...}: a hat he lost
    points = {},       -- character handle -> {group, hat}: glasses lost (ModMeshPoint)
    pieces = {},       -- spawned host handle -> its broken group: a broken pair on the ground
    pending = {},
    tick = -100000,
    spawned = {},
    drops = 0, broke = 0, fetched = 0,
  }
  DirtyFights.meshes = MOD_MESHES and MOD_MESHES >= 6 and ModMeshSet
  DirtyFights.broken = MOD_MESHES and MOD_MESHES >= 10 and ModMeshPoint
  if DirtyFights.meshes then
    for spare, group in DirtyFights.spares do ModMeshSet(MOD_ID .. "/" .. group, 1) end
    if DirtyFights.broken then
      for name, group in DirtyFights.breakable do ModMeshSet(MOD_ID .. "/" .. group, 1) end
    else
      ModLog("meshes without broken glasses (an older tools/mod_meshes): build them again (docs/MODDING.md); glasses never break")
    end
  elseif MOD_MESHES and MOD_MESHES > 0 then
    ModLog("meshes from an older version of tools/mod_meshes: build them again (docs/MODDING.md); no glasses fly")
  end

  -- Front end (config_preload2): the headgear flies; the spares are flying hats in the head
  -- joint's frame.
  CfgObj = function(...)
    local D = DirtyFights
    if arg[2] == "hat_object" and arg.n >= 22 and OBJECT and PHYS and AXIS and ANIM then
      local spare = D.meshes and D.spares[arg[1]]
      if D.headgear[arg[1]] or spare then
        arg[9] = PHYS.OBB
        arg[10] = AXIS.Z
        arg[14] = ANIM.LeftHandHatPickUp
        arg[19] = OBJECT.TYPE_HAT
      end
      if spare then
        arg[21] = {0, 0, 0}
        arg[22] = {0, 0, 0, 1}
      end
    end
    return call(D.cfgobj, arg)
  end
  CfgHat = function(a, b, model, offset, rotation)
    if DirtyFights.meshes and DirtyFights.spares[model] then
      offset, rotation = {0, 0, 0}, {0, 0, 0, 1}
    end
    return DirtyFights.cfghat(a, b, model, offset, rotation)
  end

  -- A character the level creates: glasses for their wearers, then watched if he wears a hat
  -- (checked a moment later: the game attaches hats once he is set up).
  DirtyFights.Add = function(h)
    if not h or h == NilHandle then return end
    tinsert(DirtyFights.pending, h)
    if not DirtyFights.scheduled then
      DirtyFights.scheduled = 1
      ScheduleFunc("DirtyFights.Place", 100)
    end
  end
  DirtyFights.Place = function()
    local D = DirtyFights
    D.scheduled = nil
    local list = D.pending
    D.pending = {}
    local i = 1
    while list[i] do
      local h = list[i]
      local spare = D.meshes and D.glasses[HuGetCharType(h)]
      if spare and HuHasHat(h) == NilHandle then
        HuPlaceHatOnHead(h, spare)
      end
      D.Start(h)
      i = i + 1
    end
  end
  -- Watch h while he wears a hat (ours knocked off by D.Watch, the game's own by the game; a riot
  -- hat never leaves the head, so only hats that fly are ever lost). Police caps: extra-hats.
  DirtyFights.Start = function(h)
    local D = DirtyFights
    local hat = HuHasHat(h)
    if hat == NilHandle or D.watch[h] then return end
    local name = GetObjectName(hat)
    if D.police[name] then return end
    D.watch[h] = {hat, -1, (D.headgear[name] or (D.meshes and D.spares[name])) and 1}
    if GetGameTime() - D.tick > 2000 then
      D.tick = GetGameTime()
      ScheduleFunc("DirtyFights.Watch", 100)
    end
  end

  -- A protected call: nil (and nothing logged) when f fails, e.g. a removed character.
  DirtyFights.Quiet = function() end
  DirtyFights.Try = function(f, ...)
    if not f then return nil end
    return call(f, arg, "x", DirtyFights.Quiet)
  end
  -- {x, y, z} of a character or object (GetPosition gives a vector with x, y, z), or nil.
  DirtyFights.Pos = function(h)
    local p = DirtyFights.Try(GetPosition, h)
    if p == nil then return nil end
    if type(p) == "number" then return nil end
    local x, y, z = p.x, p.y, p.z
    if type(x) ~= "number" then return nil end
    return {x, y, z}
  end
  DirtyFights.Dist = function(a, b)
    local dx, dy = a[1] - b[1], a[2] - b[2]
    return sqrt(dx * dx + dy * dy)
  end
  -- 1 when h is up and not fighting: alive, conscious, no enemies, no player within D.calm m.
  DirtyFights.Calm = function(h, p)
    local D = DirtyFights
    if HuIsDead and HuIsDead(h) then return nil end
    if (HuGetHealthPercent(h) or 0) <= D.ko then return nil end
    if BrHasEnemies and D.Try(BrHasEnemies, h) then return nil end
    local pl = {player, player2}
    for i = 1, 2 do
      local q = pl[i] and pl[i] ~= NilHandle and D.Pos(pl[i])
      if q and D.Dist(p, q) < D.calm then return nil end
    end
    return 1
  end
  -- Chance that a pair breaks: 50 % for a knock-off at 10 % of health, more for harder hits,
  -- 60 % when he is knocked out or killed; between 30 % and 85 %.
  DirtyFights.BreakChance = function(hit, down)
    local c = 0.5 + ((hit or 0) - 10) * 0.02
    if down and c < 0.6 then c = 0.6 end
    if c < 0.3 then c = 0.3 elseif c > 0.85 then c = 0.85 end
    return c
  end

  DirtyFights.Watch = function()
    local D = DirtyFights
    D.tick = GetGameTime()
    local gone = {}
    for h, w in D.watch do
      if HuHasHat(h) ~= w[1] then
        tinsert(gone, h)   -- knocked off (by the game or by us), picked up, or the character is gone
      else
        local hp = HuGetHealthPercent(h) or 0
        local dead = HuIsDead and HuIsDead(h)
        local last = w[2]
        if w[3] and (dead or hp <= D.ko or (last >= 0 and hp < last and (hp < D.low or last - hp >= D.heavy))) then
          HuDrop(h, 1, 0, 0)
          D.drops = D.drops + 1
          ModLog(format("%s knocked off character %d (health %.1f %%%s)", tostring(GetObjectName(w[1])), HuGetCharType(h), hp,
                        (dead and ", dead") or ""))
        end
        if last >= 0 and hp < last then w[4] = last - hp end   -- the last hit, % of health
        w[2] = hp
      end
    end
    local i = 1
    while gone[i] do
      local h = gone[i]
      local w = D.watch[h]
      D.watch[h] = nil
      if HuHasHat(h) == NilHandle then D.Lost(h, w) end
      i = i + 1
    end
    D.Points()
    ScheduleFunc("DirtyFights.Watch", 100)
  end

  -- h lost his hat w[1]: break it, remember it, go back for it (D.Tend every 500 ms).
  DirtyFights.Lost = function(h, w)
    local D = DirtyFights
    local name = GetObjectName(w[1])
    local hp = HuGetHealthPercent(h) or 0
    local down = (HuIsDead and HuIsDead(h)) or hp <= D.ko
    D.lost[h] = {hat = w[1], name = name, t = GetGameTime(), hit = w[4] or 0, down = down,
                 fetch = random() < D.fetch, calm = 0, tries = 0}
    if D.meshes and D.spares[name] then
      D.points[h] = {D.id .. "/" .. D.spares[name], w[1]}
    end
    if not D.tending then
      D.tending = 1
      ScheduleFunc("DirtyFights.Tend", 500)
    end
  end

  -- Lost glasses: the wearer's position for the runtime (ModMeshPoint), until he wears them again.
  DirtyFights.Points = function()
    local D = DirtyFights
    if not D.meshes or not ModMeshPoint then return end
    local done = {}
    for h, e in D.points do
      local p = D.Pos(h)
      if not p or HuHasHat(h) == e[2] then
        ModMeshPoint(e[1], tostring(h))
        tinsert(done, h)
      else
        ModMeshPoint(e[1], tostring(h), p[1], p[2], p[3])
      end
    end
    local i = 1
    while done[i] do D.points[done[i]] = nil i = i + 1 end
    for o, group in D.pieces do   -- a broken pair kicked away: its point follows it
      local p = D.Pos(o)
      if p then ModMeshPoint(group, tostring(o), p[1], p[2], p[3]) end
    end
  end

  DirtyFights.Break = function(h, e, hp)
    local D = DirtyFights
    local group = D.breakable[e.name]
    D.Try(ObjDestroy, e.hat)
    local a = random() * 6.2832
    local o = ObjSpawn(D.host, {hp[1], hp[2], hp[3]}, {0, 0, sin(a * 28.6479), cos(a * 28.6479)}, -1, 0, 0, 4294967295, nil)
    if o and o ~= NilHandle then
      ModMeshPoint(D.id .. "/" .. group, tostring(o), hp[1], hp[2], hp[3])
      D.pieces[o] = D.id .. "/" .. group
    end
    D.broke = D.broke + 1
    ModLog(format("%s broke (character %d, hit %.1f %%) at %.2f %.2f %.2f", e.name, HuGetCharType(h), e.hit, hp[1], hp[2], hp[3]))
  end

  DirtyFights.Tend = function()
    local D = DirtyFights
    local now, done, any = GetGameTime(), {}, nil
    for h, e in D.lost do
      any = 1
      local hp = D.Pos(e.hat)
      local p = D.Pos(h)
      local mine = HuHasHat(h)
      if mine == e.hat then
        D.fetched = D.fetched + 1
        ModLog(format("character %d put his %s back on", HuGetCharType(h), e.name))
        tinsert(done, h)
        D.Start(h)
      elseif e.sent and now - e.sent <= 8000 and p and not (player and HuHasHat(player) == e.hat) then
        -- on his way: the hat in his hand has no world position until it is on his head
      elseif not hp or not p or mine ~= NilHandle or (player and HuHasHat(player) == e.hat) then
        tinsert(done, h)   -- the hat is gone or worn by someone else, he is gone, or wears another
      else
        if not e.rest then
          if now - e.t >= D.settle then
            e.rest = hp
            if D.broken and D.breakable[e.name] and random() < D.BreakChance(e.hit, e.down) then
              D.Break(h, e, hp)
              tinsert(done, h)
            end
          end
        elseif D.Dist(e.rest, hp) > 0.5 and e.tries == 0 then
          tinsert(done, h)   -- someone moved it
        elseif (HuIsDead and HuIsDead(h)) or (HuIsAPlayer and HuIsAPlayer(h)) then
          tinsert(done, h)   -- dead, or played: nobody goes back for it
        elseif e.fetch and e.tries < 2 and now - e.t >= 2500 then
          if D.Calm(h, p) and D.Dist(p, hp) <= D.reach then e.calm = e.calm + 1 else e.calm = 0 end
          if e.calm >= 3 and (not e.sent or now - e.sent > 8000) then
            e.sent, e.tries = now, e.tries + 1
            D.Try(GoalGetItem, h, 4, e.hat)
            ModLog(format("character %d goes back for his %s (%.1f m)", HuGetCharType(h), e.name, D.Dist(p, hp)))
          end
        elseif not e.fetch or e.tries >= 2 and now - e.sent > 8000 then
          tinsert(done, h)   -- not one who goes back, or gave up
        end
      end
    end
    local i = 1
    while done[i] do D.lost[done[i]] = nil i = i + 1 end
    if any then ScheduleFunc("DirtyFights.Tend", 500) else D.tending = nil end
  end

  -- "a.b.c" -> the function the game would call for that name
  DirtyFights.Resolve = function(name)
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
      local h = call(DirtyFights.hucreate, arg)
      DirtyFights.Add(h)
      return h
    end
  end

  -- argument 14 of GangAddSpawner names the function called with each character it spawns
  if GangAddSpawner then
    GangAddSpawner = function(...)
      if arg.n >= 14 then
        local n = getn(DirtyFights.spawned) + 1
        DirtyFights.spawned[n] = arg[14]
        DirtyFights["Spawned" .. n] = function(...)
          DirtyFights.Add(arg[1])
          local f = DirtyFights.spawned[%n]
          if type(f) == "string" then f = DirtyFights.Resolve(f) end
          if type(f) == "function" then return call(f, arg) end
        end
        arg[14] = "DirtyFights.Spawned" .. n
      end
      return call(DirtyFights.gangaddspawner, arg)
    end
  end
end
