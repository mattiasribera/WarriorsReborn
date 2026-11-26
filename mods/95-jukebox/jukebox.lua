-- Jukebox ("Radio" in Spanish): the hideout's boom box opens an album-style list of the game's radio songs
-- (docs/MODDING.md "Menus and music"). This file (boot) holds the song list and the functions; the
-- prompt at the boom box is set up by hideout.lua (after.level95). Our own code: the songs and the
-- cover are the game's own, played and shown from the player's disc by the runtime.

Jukebox = {
  -- The game's radio songs (its sound names are vags/music/<id>; ids only, as identifiers).
  songs = {
    {"in_the_city", "In the City"},
    {"nowhere_to_run", "Nowhere to Run"},
    {"remember", "Remember"},
    {"love_is_a_fire", "Love Is a Fire"},
    {"last_of_an_ancient_breed", "Last of an Ancient Breed"},
    {"knock_on_wood", "Knock on Wood"},
    {"last_of_a_party", "Last of a Party"},
    {"you_re_movin_too_slow", "You're Movin' Too Slow"},
    {"sho", "Sho"},
    {"get_down_radio", "Get Down"},
    {"shake_it_radio", "Shake It"},
    {"love_de_lux", "Love de Lux"},
    {"drhook", "Dr. Hook"},
    {"alberto", "Alberto"},
    {"tna_funk", "TNA Funk"},
    {"funk2", "Funk 2"},
  },
  -- The cover: the game's "Stay Tuned" loading screen (its boom box), read from the player's disc
  -- (file name hash f0529bb6, texture chunk c9b3c1ce), cropped to the picture without its text.
  cover = "disc:f0529bb6:c9b3c1ce:0,100,262,262",
  save = ModSave, load = ModLoad,    -- ModSave / ModLoad work outside this file only as copies
}

-- Texts (Spanish: neutral, "tú"; the game font takes Latin-1 escapes: \250 = ú, \237 = í).
function Jukebox.Text()
  if GetLanguage and GetLanguage() == 1 then
    return {title = "Radio", game = "Radio del juego", shuffle = "Aleatorio (todas)",
            footer = "A / Enter: elegir     B / Esc: volver",
            press = (Platform == 2 and "presionar ") or "pulsa ", verb = " para elegir m\250sica"}
  end
  return {title = "Jukebox", game = "The game's radio", shuffle = "Shuffle (all songs)",
          footer = "A / Enter: choose     B / Esc: back", press = "press ", verb = " to choose music"}
end

function Jukebox.Sound(id) return "vags/music/" .. id end

-- The game's own radio sounds kept silent while a song of the jukebox plays: every song and the
-- DJ links between them.
function Jukebox.MuteList()
  local t, i = "", 1
  while Jukebox.songs[i] do
    t = t .. Jukebox.Sound(Jukebox.songs[i][1]) .. "\n"
    i = i + 1
  end
  local groups = {{"djlady_", 13}, {"dj_nextsong_", 15}, {"dj_dedicate_", 18}, {"dj_rumble_", 9}}
  local g = 1
  while groups[g] do
    for k = 1, groups[g][2] do t = t .. format("vags/music/%s%02d\n", groups[g][1], k) end
    g = g + 1
  end
  return t .. "vags/music/switch_station_02"
end

-- The menu: the game's radio, shuffle, then one row per song (each previews its song).
function Jukebox.Open(flag, who)
  local J = Jukebox
  if who and HuIsAPlayer and not HuIsAPlayer(who) then return end
  local x = J.Text()
  local items, sel, i = x.game .. "\n" .. x.shuffle, 1, 1
  if J.mode == "shuffle" then sel = 2 end
  while J.songs[i] do
    items = items .. "\n" .. J.songs[i][2] .. "\t" .. J.Sound(J.songs[i][1])
    if J.mode == "song" and J.current == J.songs[i][1] then sel = i + 2 end
    i = i + 1
  end
  if ModMenuOpen(x.title, items, x.footer, sel, J.cover) then ScheduleFunc("Jukebox.Poll", 100) end
  return true
end

function Jukebox.Poll()
  local state, i = ModMenuState()
  if state == "open" then
    ScheduleFunc("Jukebox.Poll", 100)
  elseif state == "chosen" then
    Jukebox.Choose(i)
  end
end

-- Item i chosen: 1 the game's radio, 2 shuffle, else that song (the runtime keeps playing the
-- previewed song as the jukebox's, looping).
function Jukebox.Choose(i)
  local J = Jukebox
  if i == 1 then
    J.mode, J.current = nil, nil
    ModMusicPlay(nil)
    J.save("song", nil)
  elseif i == 2 then
    J.mode = "shuffle"
    J.Next()
    J.save("song", "shuffle")
  elseif J.songs[i - 2] then
    J.mode, J.current = "song", J.songs[i - 2][1]
    J.save("song", J.current)
  end
  ModLog("chosen: " .. (J.mode == "shuffle" and "shuffle" or J.current or "the game's radio"))
end

-- Shuffle: a random song other than the last, played once; Watch starts the next when it ends.
function Jukebox.Next()
  local J = Jukebox
  local n, k = getn(J.songs), 1
  repeat   -- the game's random() returns large integers (not 1..n): bring it into range
    local r = abs(random(n))
    k = r - floor(r / n) * n + 1
  until n < 2 or J.songs[k][1] ~= J.current
  J.current = J.songs[k][1]
  ModMusicPlay(J.Sound(J.current), 0)
  if not J.watching then
    J.watching = 1
    ScheduleFunc("Jukebox.Watch", 2000)
  end
end

function Jukebox.Watch()
  local J = Jukebox
  -- Out on the Coney streets (level95_coney's `soundbox` loaded) the game stops every sound
  -- of the hideout, the jukebox's too: shuffle ends there and comes back at the next hideout load.
  if J.mode ~= "shuffle" or soundbox then
    J.watching = nil
    return
  end
  if ModMenuState() ~= "open" and not ModMusicPlaying() then J.Next() end
  ScheduleFunc("Jukebox.Watch", 2000)
end

-- Entering the hideout: the song chosen before (or shuffle) comes back once the game's radio plays.
-- Not on the Coney streets (level95 too, with level95_coney's `soundbox` loaded): the jukebox is
-- the hideout's.
function Jukebox.Resume()
  local J = Jukebox
  if soundbox then return end
  local s = J.load("song")
  if s == "shuffle" then
    J.mode = "shuffle"
    J.Next()
  elseif s then
    local i = 1
    while J.songs[i] and J.songs[i][1] ~= s do i = i + 1 end
    if J.songs[i] and ModMusicPlay(J.Sound(s)) then J.mode, J.current = "song", s end
  end
end
