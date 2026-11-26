-- Dirty clothes (docs/MODDING.md, "Dirty clothes"): the Warriors' vests, shirts and trousers get
-- dirtier and torn as the story goes on and with every fight; changing clothes in the hideout
-- (hideout.lua) makes them clean again.
--
-- Wear per Warrior = 2 points for every story mission completed since the crew last changed
-- clothes + his own fight points (1 for every 25 % of health lost in a fight, 2 more when he is
-- knocked out). Stages: 0 clean, 1 from 4 points, 2 from 9, 3 from 15. Kept with ModSave per save
-- profile ("base" = story missions done at the last change, "fight.<code>" = fight points).
-- Each stage is a runtime texture layer over the Warrior's skins ("dirty-clothes/<code>-<stage>",
-- built once from your game files by the game, from assets.ini): this file turns on, for every
-- Warrior, the layer of his stage at the start of every level and whenever his stage changes.
-- `boot` (this file): runs at the start of every Lua state (front end and each level).

DirtyClothes = {
  save = ModSave, load = ModLoad,    -- ModSave / ModLoad work outside this file only as copies
  hucreate = HuCreate,
  cfg = {
    stage_at = {4, 9, 15},  -- wear points of stages 1, 2, 3
    story = 2,              -- points per story mission completed since the last change
    hit_step = 25,          -- % of health lost per fight point
    ko = 0.5,               -- % of health at which a Warrior counts as knocked out
    ko_points = 2,
    tick = 250,             -- ms between health checks
  },
  -- the story missions, in the order of global.lua's findLastMission (Armies of the Night left out)
  story = {99, 80, 87, 34, 2, 3, 5, 81, 86, 93, 31, 14, 9, 51, 52, 54, 55, 84},
  codes = {"cl", "sw", "aj", "co", "cb", "fo", "ve", "re", "sn"},
  -- CfgChar ids of each Warrior (config_*.lua: his model and its _gen, _cv, _ghost, _chase keys)
  ids = {cl = {1, 4}, sw = {5, 9}, aj = {11, 14}, co = {15, 17}, cb = {18, 20}, fo = {21, 25},
         ve = {26, 29}, re = {30, 32}, sn = {33, 36}},
  applied = {}, watch = {}, running = nil, layers = nil,
}

function DirtyClothes.Code(h)
  local t = HuGetCharType and HuGetCharType(h)
  if not t then return nil end
  for code, r in DirtyClothes.ids do
    if t >= r[1] and t <= r[2] then return code end
  end
  return nil
end

function DirtyClothes.StoryDone()
  local n, i = 0, 1
  local s = DirtyClothes.story
  while s[i] do
    if UM_IsLevelComplete(s[i]) then n = n + 1 end
    i = i + 1
  end
  return n
end

function DirtyClothes.Wear(code)
  local D = DirtyClothes
  local base = tonumber(D.load("base") or "0") or 0
  local story = D.StoryDone() - base
  if story < 0 then story = 0 end
  return D.cfg.story * story + (tonumber(D.load("fight." .. code) or "0") or 0)
end

function DirtyClothes.Stage(code)
  local w, s = DirtyClothes.Wear(code), 0
  local at = DirtyClothes.cfg.stage_at
  while at[s + 1] and w >= at[s + 1] do s = s + 1 end
  return s
end

-- Turns on, for every Warrior, the layer of his stage (and off the other two).
function DirtyClothes.Apply(why)
  local D = DirtyClothes
  if not ModTexLayer then
    if not D.warned then ModLog("this game has no texture layers (ModTexLayer): update the game") end
    D.warned = 1
    return
  end
  if D.layers == nil then
    D.layers = ModTexLayer("dirty-clothes/cl-1", 0) and 1 or 0
    if D.layers == 0 then
      ModLog("its texture layers were not built (see the [modassets] line above; --rebuild-mod-assets dirty-clothes); clothes stay clean")
    end
  end
  if D.layers == 0 then return end
  local i, line = 1, ""
  while D.codes[i] do
    local code = D.codes[i]
    local s = D.Stage(code)
    if D.applied[code] ~= s then
      for k = 1, 3 do ModTexLayer("dirty-clothes/" .. code .. "-" .. k, k == s and 1 or 0) end
      D.applied[code] = s
    end
    line = line .. format(" %s %d (%d)", code, s, D.Wear(code))
    i = i + 1
  end
  ModLog(format("stages (%s):%s", why or "", line))
end

function DirtyClothes.AddFight(code, points, why)
  local D = DirtyClothes
  local n = (tonumber(D.load("fight." .. code) or "0") or 0) + points
  D.save("fight." .. code, n)
  ModLog(format("%s: +%d fight point%s (%s), wear %d", code, points, points == 1 and "" or "s", why, D.Wear(code)))
  if D.Stage(code) ~= D.applied[code] then D.Apply("fight") end
end

function DirtyClothes.Add(h)
  local D = DirtyClothes
  if not h or h == NilHandle or D.watch[h] then return end
  local code = D.Code(h)
  if not code then return end
  D.watch[h] = {code = code, hp = HuGetHealthPercent(h) or 100, lost = 0}
  if not D.running then
    D.running = 1
    ScheduleFunc("DirtyClothes.Watch", D.cfg.tick)
  end
end

-- Every tick: health lost by each watched Warrior (healing does not give points back).
function DirtyClothes.Watch()
  local D = DirtyClothes
  if player and player ~= NilHandle then D.Add(player) end
  if player2 and player2 ~= NilHandle then D.Add(player2) end
  for h, w in D.watch do
    local hp = HuGetHealthPercent(h)
    if hp then
      if hp < w.hp then
        w.lost = w.lost + (w.hp - hp)
        local pts = 0
        while w.lost >= D.cfg.hit_step do
          w.lost = w.lost - D.cfg.hit_step
          pts = pts + 1
        end
        if pts > 0 then D.AddFight(w.code, pts, "hits") end
        if hp <= D.cfg.ko and w.hp > D.cfg.ko then D.AddFight(w.code, D.cfg.ko_points, "knocked out") end
      end
      w.hp = hp
    end
  end
  ScheduleFunc("DirtyClothes.Watch", D.cfg.tick)
end

-- "Change clothes" (hideout.lua): the whole crew is clean again.
function DirtyClothes.Change(flag, who)
  local D = DirtyClothes
  D.save("base", D.StoryDone())
  local i = 1
  while D.codes[i] do
    D.save("fight." .. D.codes[i], nil)
    i = i + 1
  end
  D.Apply("changed clothes")
  if HUDSetAnnounceMsg and D.done_text then HUDSetAnnounceMsg(5, D.done_text) end
  return true
end

if HuCreate then
  HuCreate = function(...)
    local h = call(DirtyClothes.hucreate, arg)
    DirtyClothes.Add(h)
    return h
  end
end

-- The stages are set once the level's own script has run (start.lua, after.level*): the game's
-- progress (UM_IsLevelComplete) is not ready at the start of the front end.
