-- Cowboy keeps his hat (boot: before the game's config scripts and at the start of every level;
-- docs/MODDING.md, "Cowboy keeps his hat").
-- Cowboy is CfgChar 18, 19 and 20 (warr_cb, warr_cb_gen, warr_cb_cv), all wearing the hat object
-- dyn_warr_cb, a hat the game throws (OBJECT.TYPE_HAT). The game's own HuSetKeepHat(h, 1) keeps a
-- character's hat on his head (level54's scripts use it on Cowboy): every Cowboy the level creates
-- (HuCreate) gets it, and a level script's HuSetKeepHat(cowboy, false) is ignored for him.
-- The hat object is not changed, so Dirty Fights and More Hats Fly (which change hat objects and
-- poses in config_preload2) do not touch it, and the mods' order does not matter: each wrapper
-- calls the HuCreate it found.
-- Lua 4.0: varargs arrive in the table `arg`; call(f, arg) calls f with them.
if not CowboyHat then
  CowboyHat = {ids = {[18] = 1, [19] = 1, [20] = 1}, kept = 0}
  CowboyHat.Keep = function(h)
    local C = CowboyHat
    if h and h ~= NilHandle and C.ids[HuGetCharType(h)] then
      call(C.keephat, {h, 1})
      C.kept = C.kept + 1
      if C.kept <= 3 then ModLog(format("Cowboy (character %d) keeps his hat", HuGetCharType(h))) end
    end
  end
  if HuCreate and HuSetKeepHat then
    CowboyHat.hucreate = HuCreate
    CowboyHat.keephat = HuSetKeepHat
    HuCreate = function(...)
      local h = call(CowboyHat.hucreate, arg)
      CowboyHat.Keep(h)
      return h
    end
    HuSetKeepHat = function(...)
      local C = CowboyHat
      if arg[1] and arg[1] ~= NilHandle and C.ids[HuGetCharType(arg[1])] then
        return call(C.keephat, {arg[1], 1})
      end
      return call(C.keephat, arg)
    end
    ModLog("Cowboy keeps his hat")
  else
    ModLog("this game has no HuCreate or HuSetKeepHat: Cowboy's hat falls as in the original game")
  end
end
