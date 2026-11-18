-- Brutal fights (boot: runs at start-up and at the start of every level, in that level's Lua
-- state). The game's blood is native code: hits start its blood effect with a drop count from the
-- hit's animation, the drops leave pools on the ground that hold 3 s and fade 3 s, and a fighter
-- wears one of three blood textures by health (under 90 %, 60 %, 30 %), washed off by healing.
-- The runtime's ModBlood turns those up; every number is a field of Gore.cfg.
if not Gore then
  Gore = {}
  Gore.cfg = {
    drops = 2.5,       -- x the drop count of each blood effect (a punch has 1: 3 = mist + 4 drops)
    pool_hold = 900,   -- seconds a ground pool stays before its 3 s fade (retail 3 s)
    pool_max = 160,    -- held pools at most; the oldest fades first
    char_gain = 2,     -- blood on fighters: 100 - (100 - health %) x 2 picks the texture
    char_keep = 1,     -- 1: a fighter keeps the blood of his lowest health until the level ends
  }
end

if ModBlood then
  for k, v in Gore.cfg do ModBlood(k, v) end
  if MOD_SCRIPT ~= "enum_preload" then
    ModLog(format("blood up in %s: drops x%.1f, pools %d s (at most %d), fighters gain %.1f%s", tostring(MOD_SCRIPT),
                  Gore.cfg.drops, Gore.cfg.pool_hold, Gore.cfg.pool_max, Gore.cfg.char_gain,
                  (Gore.cfg.char_keep == 1 and ", blood stays") or ""))
  end
elseif not Gore.warned then
  Gore.warned = 1
  ModLog("this game has no ModBlood (an older WarriorsReborn): the mod changes nothing; update the game")
end
