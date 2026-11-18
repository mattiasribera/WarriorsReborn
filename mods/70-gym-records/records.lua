-- Gym records. Runs after two hideout scripts (MOD_SCRIPT says which):
--   level95            defines worksetup, the four walk-up stations of the gym (fStartWorkout[1..4]:
--                      1 heavy bag, 2 chin-ups, 3 push-ups, 4 sit-ups). Their panel text comes from
--                      worksetup.BuildWorkoutMessage(exercise, true), which returns the text (a
--                      SaveTempString index) and the rank the player would train next (-1 when the
--                      story caps it: "come back later").
--   level95_workout    defines the table `workout` for one set; the game loads it again for every set
--                      and every end of a set goes through workout.ForceQuit -> workout.CleanUp,
--                      which sets workout = nil.
--
-- What the game does in a set (level95_workout): workout.StartHUD shows the counter (and, when
-- timeTable[rank] > 0, starts the stopwatch); a cycle workout counts workout.reps in workout.DoRep,
-- the heavy bag counts the combos it called out in workout.landed in workout.Check01; reaching
-- repTable[rank] sets workout.DelayedWin (success), the stopwatch running out sets workout.TimedOut,
-- being hit or walking off ends it without either.
--
-- This mod, only once the story is finished (UM_IsLevelComplete(84), the last story mission: the game
-- itself changes the menu music on it):
--   * times each set from workout.StartHUD (GetGameTime) to the rep or combo that reached the target;
--   * keeps, per exercise and rank, the fastest successful time with ModSave ("best.<exercise>.<rank>",
--     milliseconds), per save profile; failed and timed-out sets do not count;
--   * 1.8 s after a successful set shows the time and the record in the hideout's announce slot (the
--     game's own "new feature unlocked" spot), "New record!" when it is one;
--   * adds a purple "Rank N record" line to each station's panel;
--   * logs every set: `[mods] log: gym-records: chin-ups rank 3: 10/10 in 9.53 s (limit 22 s), done, ...`.
-- Nothing of the game's save changes; the records live in mods-data/gym-records.json.

if not GymRecords then
  GymRecords = {
    STORY_LEVEL = 84,
    DELAY = 1800,          -- ms after the end of a set before the message shows
    DELAY_UNLOCK = 5500,   -- the same when the game shows its own unlock messages first (2 x 2.5 s)
    SHOW = 5000,           -- ms the message stays (<DISPLAYTIME>)
    NAMES = {"heavy bag", "chin-ups", "push-ups", "sit-ups"},
    TEXT = {
      en = {time = "Time", record = "Record", new = "New record!", was = "before",
            rank = "Rank %d record: %s", none = "none yet"},
      -- Latin-1, like the game's own Spanish strings
      es = {time = "Tiempo", record = "R\233cord", new = "\161Nuevo r\233cord!", was = "antes",
            rank = "R\233cord del rango %d: %s", none = "todav\237a ninguno"},
    },
  }

  GymRecords.On = function()
    return UM_IsLevelComplete(GymRecords.STORY_LEVEL)
  end

  GymRecords.T = function()
    if GetLanguage() == 1 then
      return GymRecords.TEXT.es
    end
    return GymRecords.TEXT.en
  end

  -- "9.53 s" (Spanish: "9,53 s")
  GymRecords.Secs = function(ms)
    local s = format("%.2f s", ms / 1000)
    if GetLanguage() == 1 then
      s = gsub(s, "%.", ",")
    end
    return s
  end

  GymRecords.Key = function(ex, rank)
    return "best." .. ex .. "." .. rank
  end

  GymRecords.Best = function(ex, rank)
    local v = GymRecords.Load and GymRecords.Load(GymRecords.Key(ex, rank))
    return v and tonumber(v)
  end

  -- worksetup.BuildWorkoutMessage(exercise, panel) with the record line on the station panels.
  GymRecords.Build = function(p0, p1)
    local text, rank = GymRecords.BuildOrig(p0, p1)
    if p1 and rank and rank > 0 and GymRecords.On() then
      local T = GymRecords.T()
      local best = GymRecords.Best(p0, rank)
      local line = format(T.rank, rank, (best and GymRecords.Secs(best)) or T.none)
      text = SaveTempString(GetTempString(text) .. "<CR3 0.02>" .. CL.bon .. line)
    end
    return text, rank
  end

  GymRecords.StartHUD = function()
    GymRecords.t0 = GetGameTime()
    GymRecords.t1 = nil
    GymRecords.StartHUDOrig()
  end

  -- After the game's rep / combo count: the moment the set reached its target.
  GymRecords.Reached = function()
    if workout and workout.DelayedWin and GymRecords.t0 and not GymRecords.t1 then
      GymRecords.t1 = GetGameTime()
    end
  end
  GymRecords.DoRep = function(p0)
    GymRecords.DoRepOrig(p0)
    GymRecords.Reached()
  end
  GymRecords.Check01 = function()
    GymRecords.Check01Orig()
    GymRecords.Reached()
  end

  -- The end of every set, read before the game's CleanUp clears `workout`; the record is stored
  -- first, so the station panel the game rebuilds in CleanUp (worksetup.FinishWorkout) shows it.
  GymRecords.CleanUp = function()
    local w = workout
    local ex, rank = w.exerciseType, w.rankPassed
    local msg
    if ex and ex >= 1 and ex <= 4 and rank and rank >= 1 and w.repTable then
      local target, limit = w.repTable[rank], w.timeTable[rank]
      local count = (ex == 1 and w.landed) or w.reps or 0
      local t0, t1 = GymRecords.t0, GymRecords.t1
      local line = format("%s rank %d: %d/%d", GymRecords.NAMES[ex], rank, count, target or 0)
      if t0 then
        line = line .. format(" in %.2f s", ((t1 or GetGameTime()) - t0) / 1000)
      end
      if limit and limit > 0 then
        line = line .. format(" (limit %d s)", limit)
      end
      if w.DelayedWin and not w.TimedOut and t0 and t1 then
        local ms = t1 - t0
        local best = GymRecords.Best(ex, rank)
        local new = not best or ms < best
        if new then
          if not GymRecords.Save(GymRecords.Key(ex, rank), ms) then
            line = line .. " (record not saved)"
          end
        end
        line = line .. ", done, record " .. format("%.2f s", ((new and ms) or best) / 1000)
        if new then
          line = line .. " (new" .. ((best and format(", was %.2f s", best / 1000)) or "") .. ")"
        end
        msg = {ex = ex, rank = rank, ms = ms, best = best, new = new}
      elseif w.TimedOut then
        line = line .. ", timed out"
      elseif t0 then
        line = line .. ", stopped"
      else
        line = line .. ", not started"
      end
      ModLog("gym-records: " .. line)
    end
    GymRecords.t0 = nil
    GymRecords.t1 = nil
    GymRecords.CleanUpOrig()
    if msg then
      GymRecords.msg = msg
      FlushScheduledFuncs("GymRecords.Announce")
      ScheduleFunc("GymRecords.Announce", (UnlockString and GymRecords.DELAY_UNLOCK) or GymRecords.DELAY)
    end
  end

  -- Shown unless the player has already started the next set.
  GymRecords.Announce = function()
    local m = GymRecords.msg
    GymRecords.msg = nil
    if not m or workout then
      return
    end
    local T = GymRecords.T()
    -- "Chin Ups   Rank: 6/10" (centred), "Time: 17.95 s", then the record line
    local s = "<CENTER><SIZE 2.0><BIGFONT><DISPLAYTIME " .. GymRecords.SHOW .. ">" .. LEVEL95.WORKOUT[m.ex] .. "   " .. CL.wht ..
              LEVEL95.RANK .. m.rank .. "/10<CR3 0.02>" .. T.time .. ": " .. GymRecords.Secs(m.ms)
    if m.new then
      s = s .. "<CR3 0.02>" .. CL.warr .. T.new
      if m.best then
        s = s .. " " .. CL.bon .. "(" .. T.was .. " " .. GymRecords.Secs(m.best) .. ")"
      end
    else
      s = s .. "<CR3 0.02>" .. CL.bon .. T.record .. ": " .. GymRecords.Secs(m.best)
    end
    GymRecords.text = s .. "</BIGFONT>"   -- kept referenced while the HUD shows it
    HUDSetAnnounceMsg(5, GymRecords.text)
  end
end

-- Kept while this file runs: ModSave / ModLoad are bound to this mod here (docs/MODDING.md).
GymRecords.Save = ModSave
GymRecords.Load = ModLoad

if not GymRecords.On() then
  if MOD_SCRIPT == "level95" then
    ModLog("story not finished (mission level" .. GymRecords.STORY_LEVEL .. " not complete): no gym records yet")
  end
elseif MOD_SCRIPT == "level95" then
  if worksetup and worksetup.BuildWorkoutMessage and worksetup.BuildWorkoutMessage ~= GymRecords.Build then
    GymRecords.BuildOrig = worksetup.BuildWorkoutMessage
    worksetup.BuildWorkoutMessage = GymRecords.Build
    ModLog("records on: station panels wrapped")
  else
    ModLog("level95 has no worksetup.BuildWorkoutMessage: nothing to do")
  end
elseif MOD_SCRIPT == "level95_workout" then
  if workout and workout.StartHUD and workout.CleanUp and workout.DoRep and workout.Check01 then
    GymRecords.StartHUDOrig = workout.StartHUD
    GymRecords.DoRepOrig = workout.DoRep
    GymRecords.Check01Orig = workout.Check01
    GymRecords.CleanUpOrig = workout.CleanUp
    workout.StartHUD = GymRecords.StartHUD
    workout.DoRep = GymRecords.DoRep
    workout.Check01 = GymRecords.Check01
    workout.CleanUp = GymRecords.CleanUp
    GymRecords.t0 = nil
    GymRecords.t1 = nil
  else
    ModLog("level95_workout has no workout set functions: nothing to do")
  end
end
