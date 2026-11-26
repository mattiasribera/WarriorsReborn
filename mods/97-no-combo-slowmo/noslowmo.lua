-- No combo slow motion (boot; docs/MODDING.md, "No combo slow motion").
-- In single player some fight combos (X+A, A, A and others, most of all with Rage on) start a
-- slow motion: an event of the fight animation sets the world's time step to the follow camera's
-- slow-motion share (0.2) for a moment. The game skips it when two players play, so co-op never
-- slows down. That test is native code, not Lua: the runtime's ModComboSlowMo(0) skips the start in
-- single player too, as co-op does. HuSetSlowMo (slow motion of the level scripts) is not changed.
if not NoComboSlowMo then
  NoComboSlowMo = 1
  if ModComboSlowMo and ModComboSlowMo(0) == 0 then
    ModLog("combo slow motion removed in single player (as in co-op)")
  elseif ModComboSlowMo then
    ModLog("this game's combo code is not the one the runtime knows: combo slow motion stays")
  else
    ModLog("this game has no ModComboSlowMo (an older WarriorsReborn): combo slow motion stays")
  end
end
