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
-- no longer wears that hat (HuHasHat). Without its meshes (MOD_MESHES < 3: the tool has not been
-- run, or an older version of it was: build them again) the mod changes nothing.
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
  -- hat only to a character that is already set up).
  HuCreate = function(...)
    local h = call(CleonBandana.hucreate, arg)
    if CleonBandana.cleon[arg[2]] and h then
      tinsert(CleonBandana.pending, h)
      ScheduleFunc("CleonBandana.Place", 100)
    end
    return h
  end
  CleonBandana.Place = function()
    local list = CleonBandana.pending
    CleonBandana.pending = {}
    local i = 1
    while list[i] do
      if HuHasHat(list[i]) == NilHandle then
        HuPlaceHatOnHead(list[i], CleonBandana.hat)
        local hat = HuHasHat(list[i])
        if hat ~= NilHandle then
          tinsert(CleonBandana.worn, {list[i], hat})
        end
      end
      i = i + 1
    end
    if not CleonBandana.watching and CleonBandana.worn[1] then
      CleonBandana.watching = 1
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
    end
    ScheduleFunc("CleonBandana.Watch", 100)
  end
end
if MOD_MESHES == 2 and not CleonBandana then
  ModLog("meshes from an older version of tools/mod_meshes: build them again (docs/MODDING.md)")
end
