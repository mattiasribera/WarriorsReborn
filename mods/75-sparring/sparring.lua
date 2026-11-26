-- Sparring ring, part 1 (boot: once in every Lua state, before the level's own script).
--
-- How a fight starts: the hideout spot (hideout.lua) keeps the player's choice for the next Lua state
-- (ModKeep "fight" = "<level> <f|s> <gang> <enemies> <crew> <you>") and loads a rumble arena the way
-- the hideout's own Rumble menu does (menu.BeginRumbleMode: SetCheckPoint(1), MenuLoadLevel). An
-- arena's script reads its setup from the game's Rumble menu with GetRumbleModeData(RM_LuaData)
-- (1 game mode, 2 game type, 3 gang size, 4/5 the two gangs' character paks minus 1, 6-14 and 15-23
-- the two sides' character ids) and GetRumbleModeGangName(1/2) in its Main(), then doFile()s
-- "level<N>_<type>_init" (spawn flags fP1/fP2) and the mode's script (brawl.lua or survival.lua).
-- In the arena the choice is waiting, so these three are wrapped here, before the level script runs:
-- the first GetRumbleModeData of that arena gets the choice instead of the menu's data and uses it up;
-- every other rumble gets the game's own data. fight.lua (after the arena's script) does the rest.
-- The intro's DJ (who names both gangs) does not read that table: the native
-- ShowRumbleModeIntro plays dj_gang_<id> from the Rumble menu's own gang ids, so the choice's
-- gangs are also written there with ModRumbleGangs (mod runtime 1.8).
--
-- Unused arenas: level103 (Warrior Turf) has its own brawl spawn file; level112 (Coney Drag) only the
-- spawn files of two other modes (Have Mercy, Wheels of Steel), so its brawl uses level112_mercy_init
-- (ten spawn flags a side, the same street).

Sparring = {
  STORY_LEVEL = 84,        -- the last story mission: the gym-records rule (records.lua)
  RM_BRAWL = 1, RM_SURV = 9,
  MAX = 9,                 -- the game's largest side ("ARMY-ING": 9 a side, ten spawn flags)
  -- the gangs the Warriors beat in the story (the trophy room's list, 20-hideout-trophies): the rumble
  -- gang id (CfgRumbleGang, also its pak number rumble_full_gang_<id>), the mission that beats them
  -- and the gang's nine rumble characters (its warchief first)
  GANGS = {
    {id = 3, level = 2, en = "Orphans", name = "ORPHANS", chars = {220, 223, 224, 225, 226, 227, 228, 225, 226}},
    {id = 14, level = 5, en = "Hurricanes", name = "HURRICANES", chars = {119, 122, 123, 125, 126, 127, 124, 125, 126}},
    {id = 10, level = 81, en = "Hi-Hats", name = "HI-HATS", chars = {135, 137, 136, 138, 139, 140, 141, 138, 139}},
    {id = 2, level = 93, en = "Destroyers", name = "DESTROYERS", chars = {178, 186, 185, 196, 180, 192, 196, 194, 197}},
    {id = 4, level = 31, en = "Turnbull ACs", name = "TURNBULL ACS", chars = {100, 99, 101, 102, 103, 104, 105, 101, 102}},
    {id = 16, level = 14, en = "Jones Street Boys", name = "JONES STREET BOYS", chars = {154, 155, 156, 157, 158, 159, 156, 157, 158}},
    {id = 20, level = 9, en = "Moonrunners", name = "MOONRUNNERS", chars = {214, 215, 216, 217, 218, 219, 216, 217, 218}},
    {id = 5, level = 54, en = "Baseball Furies", name = "BASEBALL FURIES", chars = {89, 90, 91, 92, 93, 94, 91, 92, 93}},
    {id = 6, level = 55, en = "Lizzies", name = "LIZZIES", chars = {237, 238, 239, 240, 241, 242, 243, 239, 240}},
    {id = 7, level = 55, en = "Punks", name = "PUNKS", chars = {229, 231, 233, 234, 235, 236, 233, 234, 235}},
    {id = 8, level = 84, en = "Rogues", name = "ROGUES", chars = {77, 80, 81, 82, 83, 84, 81, 82, 83}},
    {id = 12, level = 82, en = "Satan's Mothers", name = "SATAN'S MOTHERS", chars = {161, 160, 163, 164, 165, 162, 163, 164, 165}},
    {id = 11, level = 20, en = "Savage Huns", name = "SAVAGE HUNS", chars = {106, 107, 108, 109, 110, 111, 112, 113, 110}},
    {id = 13, level = 11, en = "Boppers", name = "BOPPERS", chars = {128, 129, 134, 130, 131, 132, 133, 313, 314}},
  },
  -- the Warriors' rumble gang (id 1, pak rumble_full_gang_1): the player's crew
  WARRIORS = {id = 1, name = "WARRIORS",
              chars = {2, 6, 12, 16, 34, 22, 19, 27, 31},
              names = {"Cleon", "Swan", "Ajax", "Cochise", "Snow", "Fox", "Cowboy", "Vermin", "Rembrandt"}},
  -- venues: the arena level, its names (the game's own), the spawn file a brawl there needs when the
  -- arena has none of its own
  VENUES = {
    f = {
      {level = 102, en = "Fight Pen", es = "Corral de lucha"},
      {level = 105, en = "Red Devil", es = "Red Devil"},
      {level = 134, en = "Tremont", es = "Tremont"},
      {level = 103, en = "Warrior Turf", es = "Territorio Warrior"},
      {level = 112, en = "Coney Drag", es = "Calle de Coney", init = "level112_mercy_init"},
    },
    s = {
      {level = 127, en = "The Park", es = "El parque"},
      {level = 134, en = "Tremont", es = "Tremont"},
    },
  },
}

Sparring.On = function()
  return UM_IsLevelComplete(Sparring.STORY_LEVEL)
end

Sparring.Es = function()
  return GetLanguage and GetLanguage() == 1
end

-- the gangs beaten on this profile, in story order
Sparring.Beaten = function()
  local out = {}
  local i = 1
  while Sparring.GANGS[i] do
    if UM_IsLevelComplete(Sparring.GANGS[i].level) then
      tinsert(out, Sparring.GANGS[i])
    end
    i = i + 1
  end
  return out
end

Sparring.GangById = function(id)
  local i = 1
  while Sparring.GANGS[i] do
    if Sparring.GANGS[i].id == id then
      return Sparring.GANGS[i]
    end
    i = i + 1
  end
end

-- the crew's character ids, the chosen Warrior first, then the others in the gang's order
Sparring.CrewChars = function(lead)
  local W = Sparring.WARRIORS
  local out = {W.chars[lead]}
  local i = 1
  while W.chars[i] do
    if i ~= lead then
      tinsert(out, W.chars[i])
    end
    i = i + 1
  end
  return out
end

-- "112 f 14 3 2 2" <-> {level, kind, gang, enemies, crew, lead}
Sparring.Encode = function(c)
  return format("%d %s %d %d %d %d", c.level, c.kind, c.gang, c.enemies, c.crew, c.lead)
end

Sparring.Decode = function(s)
  if type(s) ~= "string" then
    return nil
  end
  local a, b, level, kind, gang, enemies, crew, lead = strfind(s, "^(%d+) ([fs]) (%d+) (%d+) (%d+) (%d+)$")
  if not a then
    return nil
  end
  local c = {level = tonumber(level), kind = kind, gang = tonumber(gang), enemies = tonumber(enemies),
             crew = tonumber(crew), lead = tonumber(lead)}
  if not Sparring.GangById(c.gang) or c.enemies < 1 or c.enemies > Sparring.MAX or c.crew < 1 or
     c.crew > Sparring.MAX or c.lead < 1 or c.lead > getn(Sparring.WARRIORS.chars) then
    return nil
  end
  c.venue = Sparring.VenueOf(c.kind, c.level)
  if not c.venue then
    return nil
  end
  return c
end

Sparring.VenueOf = function(kind, level)
  local list = Sparring.VENUES[kind]
  local i = 1
  while list and list[i] do
    if list[i].level == level then
      return list[i]
    end
    i = i + 1
  end
end

-- the rumble setup of a choice, as GetRumbleModeData fills RM_LuaData
Sparring.Fill = function(t, c)
  local size = c.crew
  if c.kind == "f" and c.enemies > size then
    size = c.enemies
  end
  t[1] = 3                                            -- one player
  t[2] = (c.kind == "s" and Sparring.RM_SURV) or Sparring.RM_BRAWL
  t[3] = size
  t[4] = Sparring.WARRIORS.id - 1                     -- the arena adds 1: rumble_full_gang_1
  t[5] = c.gang - 1
  local crew = Sparring.CrewChars(c.lead)
  local enemy = Sparring.GangById(c.gang).chars
  for i = 1, 9 do
    t[5 + i] = crew[i]
    t[14 + i] = enemy[i]
  end
end

-- records: the fastest win per gang, enemy count and crew size; the longest survival per gang
Sparring.Key = function(c)
  if c.kind == "s" then
    return "surv." .. c.gang
  end
  return "win." .. c.gang .. "." .. c.enemies .. "." .. c.crew
end

Sparring.Best = function(c)
  local v = Sparring.Load and Sparring.Load(Sparring.Key(c))
  local ko = c.kind == "s" and Sparring.Load and Sparring.Load("survko." .. c.gang)
  return v and tonumber(v), ko and tonumber(ko)
end

-- one finished fight: returns {ms, kos, won, best, bestko, new}; a won fight (or any survival run)
-- that beats the record is saved
Sparring.Result = function(c, won, ms, kos)
  local best, bestko = Sparring.Best(c)
  local r = {ms = ms, kos = kos, won = won, best = best, bestko = bestko}
  if c.kind == "s" then
    r.new = not best or ms > best
  else
    r.new = won and (not best or ms < best)
  end
  if r.new then
    if not Sparring.Save(Sparring.Key(c), ms) then
      r.unsaved = 1
    end
    if c.kind == "s" then
      Sparring.Save("survko." .. c.gang, kos or 0)
    end
  end
  return r
end

-- "41.20 s" (Spanish "41,20 s"); survival "2:31"
Sparring.Secs = function(ms)
  local s = format("%.2f s", ms / 1000)
  if Sparring.Es() then
    s = gsub(s, "%.", ",")
  end
  return s
end

Sparring.Clock = function(ms)
  local s = floor(ms / 1000)
  return format("%d:%02d", floor(s / 60), mod(s, 60))
end

-- what the menu and the result say a record is
Sparring.RecordText = function(c, best, bestko, T)
  if not best then
    return T.none
  end
  if c.kind == "s" then
    return Sparring.Clock(best) .. " (" .. format(T.kos, bestko or 0) .. ")"
  end
  return Sparring.Secs(best)
end

-- Latin-1, like the game's own Spanish strings
Sparring.TEXT = {
  en = {title = "Sparring ring", press = "press ", verb = " to spar", mode = "Mode", fight = "Fight",
        surv = "Survival", gang = "Gang", enemies = "Enemies", you = "You", crew = "Crew", venue = "Venue",
        record = "Record", none = "none yet", kos = "%d KOs", go = " fight  ", cancel = " back",
        pad = "D-pad: choose and change", won = "Won", lost = "Beaten", time = "Time",
        new = "New record!", was = "before", survived = "Survived"},
  es = {title = "Ring de entrenamiento", press = "pulsa ", verb = " para entrenar", mode = "Modo",
        fight = "Pelea", surv = "Supervivencia", gang = "Pandilla", enemies = "Rivales", you = "T\250",
        crew = "Tu banda", venue = "Lugar", record = "R\233cord", none = "todav\237a ninguno",
        kos = "%d KO", go = " pelear  ", cancel = " volver", pad = "Cruceta: elegir y cambiar",
        won = "Ganaste", lost = "Perdiste", time = "Tiempo", new = "\161Nuevo r\233cord!", was = "antes",
        survived = "Aguantaste"},
}

Sparring.T = function()
  if Sparring.Es() then
    return Sparring.TEXT.es
  end
  return Sparring.TEXT.en
end

-- Kept while this file runs: ModSave / ModLoad / ModKeep / ModKept are bound to this mod only here
-- (docs/MODDING.md); the arena and the menu call them later.
Sparring.Save = ModSave
Sparring.Load = ModLoad
Sparring.Keep = ModKeep
Sparring.Kept = ModKept

-- The arena's setup. Only when a choice is waiting; the wrappers are harmless in other states (a
-- level with no rumble never calls them).
if Sparring.Decode(Sparring.Kept and Sparring.Kept("fight")) then
  Sparring.GetData = GetRumbleModeData
  Sparring.GetName = GetRumbleModeGangName
  Sparring.DoFile = doFile

  GetRumbleModeData = function(t)
    local S = Sparring
    local c = S.Decode(S.Kept("fight"))
    if c and Level == c.level then
      S.Keep("fight", nil)      -- used once: the next rumble is the game's own again
      S.c = c
      S.Fill(t, c)
      -- the native intro (ShowRumbleModeIntro) has the DJ name the Rumble menu's own gangs, not
      -- this table's: set them too (docs/MODDING.md, ModRumbleGangs)
      ModRumbleGangs(S.WARRIORS.id, c.gang)
      ModLog(format("arena level%d: %s, %s %d vs %d of the Warriors", c.level,
                    (c.kind == "s" and "survival") or "fight", S.GangById(c.gang).en, c.enemies, c.crew))
      return
    end
    return S.GetData(t)
  end

  GetRumbleModeGangName = function(n)
    local S = Sparring
    if S.c then
      if n == 1 then
        return S.WARRIORS.name
      end
      return S.GangById(S.c.gang).name
    end
    return S.GetName(n)
  end

  doFile = function(f)
    local S = Sparring
    local c = S.c
    if c and c.venue.init and f == "level" .. c.level .. "_brawl_init" then
      f = c.venue.init
    end
    return S.DoFile(f)
  end
end
