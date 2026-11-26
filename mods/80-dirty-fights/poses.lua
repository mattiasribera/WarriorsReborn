-- Dirty fights, knock-off poses (after config_preload2, which has just registered every hat).
-- The game registers, per hat model, where the hat sits relative to the head joint in each
-- knock-down animation (CfgHat(anim, anim2, model, offset, rotation)); a hat without a pose for
-- the animation is put in the head joint's own frame: inside the head, turned the wrong way.
-- The headgear this mod makes fly has no pose of its own (the game never throws it), so each
-- gets the poses of the donors (the police cap dyn_cops_va, then the pimp's hat dyn_pimp_va for
-- the pairs the cap lacks), as a change of the worn transform: the donor's pose P relative to
-- its attach transform T (CfgObj arguments 21, 22), D = P * T^-1 in the head joint's frame,
-- applied to the target's own attach transform, D * T'. The cap moves as the donor's does and
-- keeps its own place and facing on the head (a winter hat or glasses modelled turned half
-- round keep their facing). Pairs a model already has are left alone.
-- Transforms: v -> q v q^-1 + offset; quaternions {x, y, z, w}. Lua 4.0: a function cannot use
-- the locals of the code around it, so the helpers name DirtyFights.
local D = DirtyFights
if D and D.attach and not D.posed then
  D.posed = 1
  D.QMul = function(a, b)
    return {a[4] * b[1] + a[1] * b[4] + a[2] * b[3] - a[3] * b[2],
            a[4] * b[2] - a[1] * b[3] + a[2] * b[4] + a[3] * b[1],
            a[4] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[4],
            a[4] * b[4] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3]}
  end
  D.QInv = function(q) return {-q[1], -q[2], -q[3], q[4]} end
  D.QNorm = function(q)
    local n = sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3] + q[4] * q[4])
    if n <= 0 then return {0, 0, 0, 1} end
    return {q[1] / n, q[2] / n, q[3] / n, q[4] / n}
  end
  D.QRot = function(q, v)
    local r = DirtyFights.QMul(DirtyFights.QMul(q, {v[1], v[2], v[3], 0}), DirtyFights.QInv(q))
    return {r[1], r[2], r[3]}
  end
  -- The target's pose for a donor pose (p, r): D = (p, r) * (od, qd)^-1, then D * (ot, qt).
  D.Retarget = function(p, r, od, qd, ot, qt)
    local qD = DirtyFights.QNorm(DirtyFights.QMul(r, DirtyFights.QInv(qd)))
    local d = DirtyFights.QRot(qD, {ot[1] - od[1], ot[2] - od[2], ot[3] - od[3]})
    return {p[1] + d[1], p[2] + d[2], p[3] + d[3]}, DirtyFights.QNorm(DirtyFights.QMul(qD, qt))
  end
  D.Valid = function(t, n)
    if type(t) ~= "table" then return nil end
    for i = 1, n do
      if type(t[i]) ~= "number" then return nil end
    end
    return 1
  end

  local targets = {}
  for model, on in D.headgear do tinsert(targets, model) end
  sort(targets)
  local added, models, names = 0, 0, ""
  local i = 1
  while targets[i] do
    local model = targets[i]
    local mine = D.poses[model] or {}
    D.poses[model] = mine
    local at, n = D.attach[model], 0
    local j = 1
    while at and D.Valid(at[1], 3) and D.Valid(at[2], 4) and D.donors[j] do
      local donor = D.donors[j]
      local ad, dp = D.attach[donor], D.poses[donor]
      if ad and dp and D.Valid(ad[1], 3) and D.Valid(ad[2], 4) then
        local keys = {}
        for k, e in dp do tinsert(keys, k) end
        sort(keys)
        local m = 1
        while keys[m] do
          local e = dp[keys[m]]
          if not mine[keys[m]] and D.Valid(e[3], 3) and D.Valid(e[4], 4) then
            local o, q = D.Retarget(e[3], e[4], ad[1], ad[2], at[1], at[2])
            D.cfghat(e[1], e[2], model, o, q)
            mine[keys[m]] = {e[1], e[2], o, q}
            n = n + 1
          end
          m = m + 1
        end
      end
      j = j + 1
    end
    if n > 0 then
      added, models = added + n, models + 1
      names = names .. " " .. model .. "+" .. n
    end
    i = i + 1
  end
  ModLog(format("%d knock-off poses added to %d headgear models:%s", added, models, names))
  local s = D.poses.dyn_we_ma2 and D.poses.dyn_we_ma2["1:1"]
  if s then
    ModLog(format("dyn_we_ma2 pose 1:1 = {%.5f, %.5f, %.5f} {%.5f, %.5f, %.5f, %.5f}",
                  s[3][1], s[3][2], s[3][3], s[4][1], s[4][2], s[4][3], s[4][4]))
  end
end
