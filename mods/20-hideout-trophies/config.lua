-- Hideout trophies, part 1 (boot: runs before the game's config scripts).
-- The dragon statue (dyn_special_dragon) is set up by the game as a pick-up item: it floats above
-- the ground, turns and bobs, and the player could take it. Nothing in the game places it (its
-- objective was cut), so here it is set up as a plain object instead, like the game's other
-- display objects (dyn_snow, the trophies): it stands where it is put. Ash's vest (dyn_war_vest)
-- is held to the same plain registration (the game already registers it that way, like the retail
-- T-shirt trophy: no pick-up, no spin axis, no special type), so it keeps the rotation it is placed
-- with and never turns or bobs. So is the sheet of paper dyn_s_paper_list, a prop nothing in the game
-- places, which the mod hangs on the wall as Cleon's necklace (its meshes folder draws it as the
-- framed necklace). Every call still goes to the game.
if not HideoutTrophiesCfg then
  HideoutTrophiesCfg = {cfgobj = CfgObj, dragon = 0}
  CfgObj = function(...)
    if arg[1] == "dyn_special_dragon" and arg[2] == "pickup_item" and arg.n >= 22 and PHYS and ANIM and OBJECT then
      arg[2] = "simple_object"
      arg[3] = 100
      arg[6] = 10
      arg[9] = PHYS.OBB
      arg[10] = ANIM.na
      arg[14] = ANIM.na
      arg[19] = OBJECT.TYPE_GENERIC
      HideoutTrophiesCfg.dragon = 1
      ModLog("dyn_special_dragon set up as a display object")
    elseif (arg[1] == "dyn_war_vest" or arg[1] == "dyn_s_paper_list") and arg.n >= 22 and PHYS and ANIM and OBJECT then
      if arg[2] ~= "simple_object" or arg[9] ~= PHYS.OBB or arg[10] ~= ANIM.na or arg[14] ~= ANIM.na or arg[19] ~= OBJECT.TYPE_GENERIC then
        arg[2] = "simple_object"
        arg[9] = PHYS.OBB
        arg[10] = ANIM.na
        arg[14] = ANIM.na
        arg[19] = OBJECT.TYPE_GENERIC
        ModLog(arg[1] .. " set up as a display object")
      else
        ModLog(arg[1] .. " is a display object")
      end
    end
    return call(HideoutTrophiesCfg.cfgobj, arg)
  end
end
