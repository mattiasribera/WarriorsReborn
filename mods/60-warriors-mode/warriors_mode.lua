-- Warriors Mode (truly hard). One file for every hook (see mod.ini and docs/MODDING.md):
--   boot              (each Lua state)   define WarriorsMode; wrap CfgGang before config_preload
--                                         sets the gangs (front end, once per run)
--   after.config_easy / config_normal / config_hard (each level load)
--                                         re-apply the difficulty from the game's Hard config with
--                                         the mode's changes, wrap the police, healing and money.
-- Every number the mode changes is a field of WarriorsMode.cfg. Nothing of the game is copied: the
-- mode changes the arguments of the game's own Cfg* calls on their way to the engine.
-- Lua 4.0: varargs arrive in `arg`; call(f, arg) calls f with them; floor/min/max are globals.

if not WarriorsMode then
  WarriorsMode = {
    cfg = {
      enemy_damage = 1.25,       -- CfgPowerClass arg 9: the class's damage multiplier
      enemy_timing = 0.8,        -- CfgPowerClass args 16 and 17 (retail Hard lowers them too)
      first_player_class = 58,   -- power classes 58+ are the Warriors' own: never changed
      warrior_classes = {[7] = 1, [8] = 1},   -- the Warriors' default classes (CfgChar): never changed
      block_chance = 80,         -- BaseChanceToBlock (retail: Easy 40, Normal 60, Hard 70)
      break_in_delay = 5,        -- CfgBreakAndEnterDelay (retail: Easy 15, Normal 10, Hard 7)
      player_gang = 0,           -- gang 0 = the Warriors: never changed
      max_attackers = 4,         -- CfgGang arg 3 (attackers on the player at once) +1, at most this
      guard_level_min = 2,       -- CfgGang arg 7 (guard-break level) +1, at least this ...
      guard_level_max = 4,       -- ... and at most this
      police_max = 4,            -- CfgCrimeResponders(crime, n): n >= 1 -> n + 1, at most this
      heal_scale = 0.5,          -- any rise of the player's health counts this much
      money_scale = 0.6,         -- any rise of the player's money counts this much
      tick_ms = 250,             -- health/money check period (game time)
      forced_normal_from = 60,   -- the game itself plays levels 60-64 on Normal: left alone
      forced_normal_to = 64,
      front_end_level = 100,     -- level100 = the menus; Quick Rumble levels are above it
    },
    done = nil,                  -- set once the difficulty was applied in this Lua state
  }

  -- ---- pure helpers (tests/test_mod_warriors_mode.py runs them without the game) ----

  WarriorsMode.round = function(x, digits)
    local m = 1
    for i = 1, digits do m = m * 10 end
    return floor(x * m + 0.5) / m
  end

  -- CfgPowerClass(class, ...): enemy classes hit harder and use shorter timings.
  WarriorsMode.power_args = function(a)
    local c = WarriorsMode.cfg
    local cls = a[1]
    if type(cls) ~= "number" or cls >= c.first_player_class or c.warrior_classes[cls] then return nil end
    if type(a[9]) == "number" then a[9] = WarriorsMode.round(a[9] * c.enemy_damage, 3) end
    if type(a[16]) == "number" then a[16] = WarriorsMode.round(a[16] * c.enemy_timing, 2) end
    if type(a[17]) == "number" then a[17] = WarriorsMode.round(a[17] * c.enemy_timing, 2) end
    return 1
  end

  -- CfgGang(gang, others, player, ..., level, ...): one more attacker on the player, earlier guard break.
  WarriorsMode.gang_args = function(a)
    local c = WarriorsMode.cfg
    if type(a[1]) ~= "number" or a[1] == c.player_gang then return nil end
    if type(a[3]) == "number" and a[3] >= 1 then a[3] = min(a[3] + 1, max(a[3], c.max_attackers)) end
    if type(a[7]) == "number" then a[7] = min(max(a[7] + 1, c.guard_level_min), max(a[7], c.guard_level_max)) end
    return 1
  end

  WarriorsMode.police = function(n)
    local c = WarriorsMode.cfg
    if type(n) ~= "number" or n < 1 then return n end
    return min(n + 1, max(n, c.police_max))
  end

  -- A rise from `last` to `now` counts `scale` of itself (rounded); a drop is kept as it is.
  WarriorsMode.scaled_gain = function(last, now, scale)
    if not last or not now or now <= last then return now end
    return last + floor((now - last) * scale + 0.5)
  end

  -- ---- game side ----

  WarriorsMode.log = function(s)
    if ModLog then ModLog(s) end
  end

  WarriorsMode.level_id = function()
    if GetLevelId and GetCurrentLevelIndex then return GetLevelId(GetCurrentLevelIndex()) end
    return -1
  end

  -- Re-run the difficulty with CfgPowerClass / CfgBreakAndEnterDelay changed on the way.
  WarriorsMode.apply = function(script)
    local c = WarriorsMode.cfg
    local lvl = WarriorsMode.level_id()
    if lvl >= c.forced_normal_from and lvl <= c.forced_normal_to then
      WarriorsMode.log(format("level %d: the game plays it on Normal; mode not applied", lvl))
      return
    end
    if lvl < 0 or lvl >= c.front_end_level then return end   -- menus (level100) and Quick Rumble
    local base = script
    local profile = GetProfileDifficulty and GetProfileDifficulty() or -1
    local orig_power, orig_bae = CfgPowerClass, CfgBreakAndEnterDelay
    local changed = {}
    WarriorsMode.orig_power = orig_power
    WarriorsMode.orig_bae = orig_bae
    WarriorsMode.changed = changed
    CfgPowerClass = function(...)
      local before9, before16 = arg[9], arg[16]
      if WarriorsMode.power_args(arg) then
        local ch = WarriorsMode.changed
        tinsert(ch, format("c%s %.4g>%.4g %.4g>%.4g", tostring(arg[1]), before9 or 0, arg[9] or 0,
                           before16 or 0, arg[16] or 0))
      end
      return call(WarriorsMode.orig_power, arg)
    end
    CfgBreakAndEnterDelay = function(n)
      WarriorsMode.bae_before = n
      return WarriorsMode.orig_bae(WarriorsMode.cfg.break_in_delay)
    end
    local block_before = BaseChanceToBlock
    if script == "config_hard" and CfgDifficulty then
      CfgDifficulty()                -- the same Hard config again, now through the wrappers
    elseif script == "config_hard" then
      doFile("config_hard")          -- the config removed its CfgDifficulty when it ended: reload it
    else
      base = "config_hard"
      doFile("config_hard")          -- Easy/Normal profile: load the game's Hard config instead
    end
    CfgPowerClass, CfgBreakAndEnterDelay = orig_power, orig_bae
    BaseChanceToBlock = c.block_chance   -- global.lua passes it to CfgBaseChanceToBlock next
    WarriorsMode.done = 1
    WarriorsMode.log(format("applied after %s (profile difficulty %s, level %d, now %s, GetDifficulty %s): " ..
                            "%d enemy power classes damage x%.2f timing x%.2f; block chance %s -> %d; " ..
                            "break-in delay %s -> %d", script, tostring(profile), lvl, base,
                            tostring(GetDifficulty and GetDifficulty()), getn(changed), c.enemy_damage,
                            c.enemy_timing, tostring(block_before), c.block_chance,
                            tostring(WarriorsMode.bae_before), c.break_in_delay))
    local i = 1                           -- class dmg before>after timing before>after, 10 a line
    while changed[i] do
      local t = "power"
      for k = i, min(i + 9, getn(changed)) do t = t .. " " .. changed[k] end
      WarriorsMode.log(t)
      i = i + 10
    end
  end

  -- Police: one more responder for every crime that already has some (levels set them later too).
  WarriorsMode.wrap_police = function()
    if WarriorsMode.orig_police or not CfgCrimeResponders then return end
    WarriorsMode.orig_police = CfgCrimeResponders
    CfgCrimeResponders = function(crime, n)
      return WarriorsMode.orig_police(crime, WarriorsMode.police(n))
    end
  end

  -- Healing and money: a check every tick_ms of game time while the player is alive. A rise of the
  -- player's health percent or money counts only heal_scale / money_scale of itself. Health and money
  -- the scripts set (HuSetHealth*, HuSetMaxHealth, InvSetMoney) are kept as they are.
  WarriorsMode.resync = function()
    local h = WarriorsMode.player()
    WarriorsMode.last_hp = h and HuGetHealthPercent(h) or nil
  end

  WarriorsMode.player = function()
    if player and player ~= NilHandle and HuIsAlive and HuIsAlive(player) then return player end
    return nil
  end

  WarriorsMode.wrap_health_setters = function()
    if WarriorsMode.orig_sethp then return end
    WarriorsMode.orig_sethp = {HuSetHealth = HuSetHealth, HuSetHealthPercent = HuSetHealthPercent,
                               HuSetMaxHealth = HuSetMaxHealth, InvSetMoney = InvSetMoney}
    for name, f in WarriorsMode.orig_sethp do
      if f then
        setglobal(name, WarriorsMode.setter(name))
      end
    end
  end

  WarriorsMode.setter = function(name)
    return function(...)
      local r = call(WarriorsMode.orig_sethp[%name], arg)
      if %name == "InvSetMoney" then
        WarriorsMode.last_money = {}       -- a script set the money: take it as it is
      elseif arg[1] == player then
        WarriorsMode.resync()
      end
      return r
    end
  end

  WarriorsMode.stats = {heals = 0, healed = 0, withheld_hp = 0, gains = 0, withheld_money = 0}

  WarriorsMode.tick = function()
    local c = WarriorsMode.cfg
    local s = WarriorsMode.stats
    local h = WarriorsMode.player()
    if h then
      local hp = HuGetHealthPercent(h)
      local last = WarriorsMode.last_hp
      if last and hp and hp > last + 0.5 then
        local want = WarriorsMode.scaled_gain(last, hp, c.heal_scale)
        WarriorsMode.orig_sethp.HuSetHealthPercent(h, want)
        s.heals = s.heals + 1
        s.healed = s.healed + (want - last)
        s.withheld_hp = s.withheld_hp + (hp - want)
        WarriorsMode.log(format("heal %s%% -> %s%%: kept +%s of +%s", tostring(last), tostring(want),
                                tostring(want - last), tostring(hp - last)))
        hp = HuGetHealthPercent(h)
      end
      WarriorsMode.last_hp = hp
    else
      WarriorsMode.last_hp = nil
      WarriorsMode.last_money = {}
    end
    if InvGetMoney and WarriorsMode.orig_sethp.InvSetMoney and h then
      for p = 1, 2 do
        local m = InvGetMoney(p)
        local last = WarriorsMode.last_money[p]
        if last and m and m > last then
          local want = WarriorsMode.scaled_gain(last, m, c.money_scale)
          WarriorsMode.orig_sethp.InvSetMoney(want, p)
          s.gains = s.gains + 1
          s.withheld_money = s.withheld_money + (m - want)
          WarriorsMode.log(format("money p%d $%d -> $%d: kept +%d of +%d", p, last, want, want - last, m - last))
          m = InvGetMoney(p)
        end
        WarriorsMode.last_money[p] = m
      end
    end
    ScheduleFunc("WarriorsMode.tick", c.tick_ms)
  end

  WarriorsMode.start_ticks = function()
    if WarriorsMode.ticking then return end
    WarriorsMode.ticking = 1
    WarriorsMode.last_money = {}
    ScheduleFunc("WarriorsMode.tick", 2000)
    -- a level script that flushes every scheduled function also stops ours: start it again
    if FlushScheduledFuncs and not WarriorsMode.orig_flush then
      WarriorsMode.orig_flush = FlushScheduledFuncs
      FlushScheduledFuncs = function(...)
        local r = call(WarriorsMode.orig_flush, arg)
        if arg.n == 0 or arg[1] == nil then ScheduleFunc("WarriorsMode.tick", WarriorsMode.cfg.tick_ms) end
        return r
      end
    end
  end
end

-- ---- what this hook does ----

if MOD_SCRIPT == "config_easy" or MOD_SCRIPT == "config_normal" or MOD_SCRIPT == "config_hard" then
  if not WarriorsMode.done then
    WarriorsMode.apply(MOD_SCRIPT)
    if WarriorsMode.done then
      WarriorsMode.wrap_police()
      WarriorsMode.wrap_health_setters()
      WarriorsMode.start_ticks()
    end
  end
elseif not WarriorsMode.orig_gang and CfgGang then
  -- boot of the front-end state (before config_preload sets the gangs, once per run)
  WarriorsMode.orig_gang = CfgGang
  WarriorsMode.gangs = 0
  CfgGang = function(...)
    local a3, a7 = arg[3], arg[7]
    if WarriorsMode.gang_args(arg) then
      WarriorsMode.gangs = WarriorsMode.gangs + 1
      WarriorsMode.log(format("gang %s: attackers on the player %s -> %s, guard-break level %s -> %s",
                              tostring(arg[1]), tostring(a3), tostring(arg[3]), tostring(a7), tostring(arg[7])))
    end
    return call(WarriorsMode.orig_gang, arg)
  end
end
