-- More hats fly, part 2 (after config_preload2, which has just registered every hat).
-- A model whose hat object is identical (same box, type, offset and rotation) to another
-- model's gets that model's hat poses it does not have yet: the same cap on the same head,
-- knocked off by the same hits. Models with a hat of their own shape are left alone, so only
-- hats that are a separate object the game already throws are used.
local H = ExtraHats
if H and not H.done then
  H.done = 1
  local added, models, names = 0, 0, ""
  -- deterministic order: models by name
  local list = {}
  for model, g in H.geometry do tinsert(list, model) end
  sort(list)
  local i = 1
  while list[i] do
    local model = list[i]
    -- donors: other models with the same hat, the one with the most poses first
    local donors, j = {}, 1
    while list[j] do
      local other = list[j]
      if other ~= model and H.geometry[other] == H.geometry[model] and H.poses[other] then
        tinsert(donors, other)
      end
      j = j + 1
    end
    sort(donors, function(x, y)
      local cx, cy = ExtraHats.poses[x].count, ExtraHats.poses[y].count
      if cx ~= cy then return cx > cy end
      return x < y
    end)
    local mine = H.poses[model] or {count = 0}
    H.poses[model] = mine
    local before, d = mine.count, 1
    while donors[d] do
      for k, e in H.poses[donors[d]] do
        if k ~= "count" and not mine[k] then
          H.cfghat(e[1], e[2], model, e[3], e[4])
          mine[k] = e
          mine.count = mine.count + 1
        end
      end
      d = d + 1
    end
    if mine.count > before then
      added = added + mine.count - before
      models = models + 1
      names = names .. " " .. model .. "+" .. (mine.count - before)
    end
    i = i + 1
  end
  ModLog(format("%d police caps made to fly; %d hat poses added to %d models:%s", H.caps, added, models, names))
end
