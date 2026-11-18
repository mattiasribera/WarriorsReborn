-- Dirty clothes: the Warriors' stages at the start of a level (after.level*: after the level's
-- script; once per Lua state). Not in the front end's main menu (level100).
if DirtyClothes and not DirtyClothes.started and MOD_SCRIPT ~= "level100" then
  DirtyClothes.started = 1
  DirtyClothes.Apply(MOD_SCRIPT)
end
