-- Cleon loses his bandana (boot: runs before the game's config scripts and at the start of
-- every level, before the level script creates its characters).
--
-- While the bandana is on his head Cleon is drawn exactly as the game draws him (his own head
-- with the modelled bandana: opening scenes, cutscenes and gameplay look like the original).
-- The bandana is also a hat: the unused Destroyers hat object "dyn_dest_ltb", set up so its
-- frame is the head joint's (attach offset and knock-off poses made identity) and put on Cleon's
-- head with the game's own HuPlaceHatOnHead, but not drawn while he wears it (the mod's folder
-- hides that draw). Once the game's hat code knocks it off (a hard hit), the mod's mesh group
-- "knocked" is turned on: Cleon is drawn with his flashback head and the falling hat object as
-- the bandana, which stays on the ground until the level reloads.
--
-- The state is the game's own: every 100 ms the bandana is "off" when a Cleon it was placed on
-- no longer wears that hat (HuHasHat). Without its meshes (MOD_MESHES < 3: assets.ini could not be
-- built from the game files, see the [modassets] lines of the log) the mod changes nothing.
-- Every placement (or why there was none) and every knock-off is a [mods] log line.
-- Lua 4.0: varargs arrive in the table `arg`; call(f, arg) calls f with them.
if MOD_MESHES and MOD_MESHES >= 3 and ModMeshSet and not CleonBandana then
  CleonBandana = {
    hat = "dyn_dest_ltb",
    group = MOD_ID .. "/knocked",
    cleon = {[1] = 1, [2] = 1, [4] = 1},   -- CfgChar ids of Cleon's normal model (warr_cl, _gen, _chase)
    pending = {},
    worn = {},        -- {cleon handle, hat handle} of each placed bandana
    off = 0,          -- the group's state
    watching = nil,
    cfgobj = CfgObj, cfghat = CfgHat, hucreate = HuCreate,
  }
  ModMeshSet(CleonBandana.group, 0)   -- a new level: the bandana is on

  -- Front end (config_preload2): the hat object's frame = the head joint's frame.
  CfgObj = function(...)
    if arg[1] == CleonBandana.hat and arg[2] == "hat_object" and arg.n >= 22 then
      arg[21] = {0, 0, 0}
      arg[22] = {0, 0, 0, 1}
    end
    return call(CleonBandana.cfgobj, arg)
  end
  CfgHat = function(a, b, model, offset, rotation)
    if model == CleonBandana.hat then
      offset, rotation = {0, 0, 0}, {0, 0, 0, 1}
    end
    return CleonBandana.cfghat(a, b, model, offset, rotation)
  end

  -- Every level: Cleon gets the bandana hat a moment after he is created (the game attaches a
  -- hat only to a character that is already set up). In a rumble arena he is not, for a while:
  -- the arena creates the sides at the start of its intro and HuPlaceHatOnHead gives him nothing
  -- until later, so a Cleon who gets no hat is tried again every 250 ms, for up to 60 s.
  CleonBandana.TRIES = 240
  HuCreate = function(...)
    local h = call(CleonBandana.hucreate, arg)
    if CleonBandana.cleon[arg[2]] and h then
      tinsert(CleonBandana.pending, {h, 0})
      -- one retry chain at a time; a chain whose next call is long overdue (a lost schedule) is
      -- replaced
      local B, now = CleonBandana, GetGameTime()
      if not B.next or abs(now - B.next) > 1000 then
        B.next = now + 100
        ScheduleFunc("CleonBandana.Place", 100)
      end
    end
    return h
  end
  -- one log line per placement (ModLog outside the mod's own file says "log:", so the id is in it)
  CleonBandana.Log = function(what, who)
    ModLog(format("cleon-bandana: bandana %s (character %s, level %s)", what, tostring(who), tostring(Level)))
  end
  CleonBandana.Place = function()
    local B = CleonBandana
    local list = B.pending
    B.pending = {}
    local i = 1
    while list[i] do
      local h, tries = list[i][1], list[i][2] + 1
      if HuHasHat(h) ~= NilHandle then
        B.Log("not placed: Cleon already wears a hat", h)
      else
        HuPlaceHatOnHead(h, B.hat)
        local hat = HuHasHat(h)
        if hat ~= NilHandle then
          tinsert(B.worn, {h, hat})
          B.Log(format("placed on Cleon (try %d)", tries), h)
        elseif tries < B.TRIES then
          tinsert(B.pending, {h, tries})
        else
          B.Log(format("NOT placed on Cleon: HuPlaceHatOnHead gave him no hat in %d tries", tries), h)
        end
      end
      i = i + 1
    end
    if B.pending[1] then
      B.next = GetGameTime() + 250
      ScheduleFunc("CleonBandana.Place", 250)
    else
      B.next = nil
    end
    if not B.watching and B.worn[1] then
      B.watching = 1
      ScheduleFunc("CleonBandana.Watch", 100)
    end
  end
  -- The bandana is off while any Cleon it was placed on wears something else (or nothing).
  CleonBandana.Watch = function()
    local off = 0
    local i = 1
    while CleonBandana.worn[i] do
      local w = CleonBandana.worn[i]
      if HuHasHat(w[1]) ~= w[2] then
        off = 1
      end
      i = i + 1
    end
    if off ~= CleonBandana.off then
      CleonBandana.off = off
      ModMeshSet(CleonBandana.group, off)
      if off == 1 then
        ModLog("cleon-bandana: bandana knocked off (level " .. tostring(Level) .. ")")
      end
    end
    ScheduleFunc("CleonBandana.Watch", 100)
  end
end
if MOD_MESHES == 2 and not CleonBandana then
  ModLog("meshes from an older version of the mod: build them again (--rebuild-mod-assets cleon-bandana)")
end
