-- Hideout trophies, part 2. level95 (the hideout) has just been loaded and has placed its own
-- trophies on the south wall of the main room (Objects.Spider, King, LC, Fuckyou, Bigmoe). The game
-- shows each of those only once its mission is done: each one is an unlock of type 5 in global.lua's
-- Unlockables table ({level, 0, 0, 5, name, n}: Spider = level 34, King = 2, LC = 5, Fuckyou = 81,
-- Bigmoe = 31), opened by UM_Unlock(Level, 0, 0) when that mission is completed, and
-- flashback.SetupTrophies() destroys the ones that are still locked.
--
-- This mod adds three more trophies that work the same way: each is placed only once its mission is
-- complete (UM_IsLevelComplete, the check level95 itself uses for level 84), so on a profile that has
-- not played it nothing appears. Nothing is saved or unlocked by the mod.
--   * The green dragon statue (dyn_special_dragon) from Fox's flashback in Chinatown (level 20,
--     "Flashback D", mission 22): the game registers it there as a stolen item, but the objective
--     that took it from the Savage Huns was cut. It stands on a rusty barrel (like the one under the
--     Spider trophy) beside the "F.U. Huns" T-shirt (Fuckyou), between it and the subway map, facing
--     the room. config.lua makes it a plain object (no pick-up float); its origin is its base, set
--     from captures: the barrel top is 0.8 m up, well under its 1.12 m bounding box.
--   * Ash's vest, in his memory, for the mission where he dies (level 86, mission 9, the Destroyers
--     attack the Coney strip, scene L86C3): a Warriors vest (dyn_war_vest, the game's flat vest with
--     the gang's back patch) hung flat on the wall between the crown's shelf (King) and the panties'
--     frame (LC), like the retail "F.U. Huns" T-shirt, back patch facing the room. The model lies
--     flat with the patch up; the rotation stands it up against the wall (patch towards +y, the room,
--     upright: the other quarter turn shows it mirrored, from the inside). Its origin is its middle;
--     it hangs touching the wall, a little above the T-shirt's height so a player standing at its
--     label does not hide it from the follow camera.
--     (1.3 used dyn_s_vest, the vest crumpled on the fence in the death scene: a narrow rag that looks
--     the same from every side, so it seemed to float and turn with the camera.)
--   * Cleon's necklace, once the story is finished (level 84, the final mission: level95's own
--     flashback.SetupTrophies() checks UM_IsLevelComplete(84) too). The necklace is painted on
--     Cleon's chest in his skin texture, so tools/mod_meshes (python3 -m mod_meshes cleon-necklace)
--     builds it from the player's own game files into this mod's meshes folder: a wooden
--     shadow-box frame with the necklace on deep red velvet, drawn instead of dyn_s_paper_list, a
--     sheet of paper nothing in the game places. It hangs on the wall above the crown's shelf
--     (King), its back on the wall, its origin at its bottom middle. Without the built meshes
--     (MOD_MESHES 0) it is not placed: the sheet of paper would show instead. Its walk-up spot is at
--     the shelf's left end, halfway between the Flashback spots in front of the Spider trophy and
--     the crown (fFback_1, fFback_2). Those spots only prompt while their flashback mission is
--     unlocked and not yet complete (flashback.Setup), and every flashback is a story mission, so
--     once the story is finished they are silent; the necklace's prompt then also shows in front of
--     the crown.
--
-- The retail trophies have no text of their own. So each new trophy gets the hideout's own
-- walk-up prompt (AddFlag + SetMsgHandlerEx, as for the Flashback and menu spots), shown while the
-- player stands next to it: its name as the big title at the top and one line on where it comes from
-- in the message box at the bottom left; in Spanish when GetLanguage() == 1. Pressing the action
-- button there does nothing, like the retail locked-flashback spot (flashback.NoAction). The spots
-- are right in front of the trophies, closer than the Flashback spots beside them.
HideoutTrophies = {
  tries = 0,
  -- each trophy: the level whose completion shows it, the objects {model, position, rotation},
  -- the spot of its walk-up prompt (on the floor in front of it), {EN title, line}, {ES title, line}
  list = {
    {level = 20,
     objs = {{"dyn_barrel_b", {-179.5, 82.05, -194.38}, {0, 0, 0, -1}},
             {"dyn_special_dragon", {-179.5, 82.05, -193.58}, {0, 0, 0, -1}}},
     spot = {-179.5, 82.9, -194.3},
     en = {"Dragon Statue", "Taken from the Savage Huns"},
     es = {"Estatua del drag\243n", "Robada a los Savage Huns"}},
    {level = 86,
     objs = {{"dyn_war_vest", {-171.05, 81.76, -192.55}, {0, 0.7071068, 0.7071068, 0}}},
     spot = {-171.05, 82.6, -194.3},
     en = {"Ash's Vest", "In his memory"},
     es = {"Chaleco de Ash", "En su memoria"}},
    {level = 84, meshes = 1,
     objs = {{"dyn_s_paper_list", {-169.75, 81.74, -192.62}, {0, 0, 0, -1}}},
     spot = {-169.35, 82.65, -194.3},
     en = {"Cleon's Necklace", "The one he always wore"},
     es = {"Collar de Cleon", "El que siempre llev\243"}},
    -- the gang trophy room (the caged room under the main floor, see Room below): one trophy per gang
    -- the Warriors beat, by the mission where they are beaten. A hat or a bat (frozen = 1: no physics,
    -- it stays where it is put) lies at the back of the counter along the west wall, out of reach of
    -- the game's pick-up (Y takes a hat or a bat lying 1.0 m away, not 1.6 m; the counter keeps the
    -- player about 1.5 m away), with the gang's photo card (the game's own mission-intro card) on the wall above it;
    -- the other gangs' cards hang on the walls.
    {level = 5,
     objs = {{"dyn_hurricanes", {-174.80, 96.3, -195.98}, {0, 0, 0.7071068, 0.7071068}},
             {"dyn_hurr_lt", {-174.55, 96.0, -197.12}, {0, 0, 0, -1}, 1}},
     spot = {-172.75, 96.3, -197.5},
     en = {"Hurricanes' Hat", "Beaten in Mission 7"},
     es = {"Sombrero de los Hurricanes", "Vencidos en la misi\243n 7"}},
    {level = 81,
     objs = {{"dyn_hihats", {-174.80, 93.7, -195.98}, {0, 0, 0.7071068, 0.7071068}},
             {"dyn_hiha_lt", {-174.55, 93.7, -197.12}, {0, 0, 0, -1}, 1}},
     spot = {-172.75, 93.7, -197.5},
     en = {"Hi-Hats' Top Hat", "Beaten in Mission 8"},
     es = {"Galera de los Hi-Hats", "Vencidos en la misi\243n 8"}},
    {level = 31,
     objs = {{"dyn_turn_so", {-174.55, 95.0, -197.12}, {0, 0, 0.7071068, 0.7071068}, 1}},
     spot = {-172.75, 95.0, -197.5},
     en = {"Turnbull ACs' Shades", "Beaten in Mission 11"},
     es = {"Gafas de los Turnbull ACs", "Vencidos en la misi\243n 11"}},
    {level = 54,
     objs = {{"dyn_furies", {-174.80, 91.1, -195.98}, {0, 0, 0.7071068, 0.7071068}},
             {"dyn_lt_bat_blk_a", {-174.55, 91.1, -197.03}, {0, 0, 0, -1}, 1}},
     spot = {-172.75, 91.1, -197.5},
     en = {"Baseball Furies' Bat", "Beaten in Mission 16"},
     es = {"Bate de los Baseball Furies", "Vencidos en la misi\243n 16"}},
    {level = 84,
     objs = {{"dyn_rogu_lt", {-174.55, 92.4, -197.12}, {0, 0, 0, -1}, 1}},
     spot = {-172.75, 92.4, -197.5},
     en = {"Rogues' Cap", "Beaten in Mission 18"},
     es = {"Gorra de los Rogues", "Vencidos en la misi\243n 18"}},
    {level = 2,
     objs = {{"dyn_orphans", {-174.80, 88.6, -196.08}, {0, 0, 0.7071068, 0.7071068}}},
     spot = {-173.4, 88.6, -197.5},
     en = {"The Orphans", "Beaten in Mission 5"},
     es = {"Los Orphans", "Vencidos en la misi\243n 5"}},
    {level = 93,
     objs = {{"dyn_destroyers", {-174.80, 83.9, -196.08}, {0, 0, 0.7071068, 0.7071068}}},
     spot = {-173.4, 83.9, -197.5},
     en = {"The Destroyers", "Beaten in Mission 10"},
     es = {"Los Destroyers", "Vencidos en la misi\243n 10"}},
    {level = 14,
     objs = {{"dyn_jsbs", {-169.5, 81.73, -196.08}, {0, 0, 1, 0}}},
     spot = {-169.5, 83.1, -197.5},
     en = {"Jones Street Boys", "Beaten in Mission 12"},
     es = {"Los Jones Street Boys", "Vencidos en la misi\243n 12"}},
    {level = 9,
     objs = {{"dyn_moonrunners", {-165.20, 84.4, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 84.4, -197.5},
     en = {"The Moonrunners", "Beaten in Mission 13"},
     es = {"Los Moonrunners", "Vencidos en la misi\243n 13"}},
    {level = 55,
     objs = {{"dyn_lizzies", {-165.20, 86.9, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 86.9, -197.5},
     en = {"The Lizzies", "Beaten in Mission 17"},
     es = {"Las Lizzies", "Vencidas en la misi\243n 17"}},
    {level = 55,
     objs = {{"dyn_punks", {-165.20, 89.4, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 89.4, -197.5},
     en = {"The Punks", "Beaten in Mission 17"},
     es = {"Los Punks", "Vencidos en la misi\243n 17"}},
    {level = 82,
     objs = {{"dyn_samo", {-165.20, 92.6, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 92.6, -197.5},
     en = {"Satan's Mothers", "Beaten in Flashback A"},
     es = {"Las Satan's Mothers", "Vencidas en el Flashback A"}},
    {level = 20,
     objs = {{"dyn_huns", {-165.20, 94.8, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 94.8, -197.5},
     en = {"The Savage Huns", "Beaten in Flashback D"},
     es = {"Los Savage Huns", "Vencidos en el Flashback D"}},
    {level = 11,
     objs = {{"dyn_bopp", {-165.20, 97.0, -196.08}, {0, 0, 0.7071068, -0.7071068}}},
     spot = {-166.75, 97.0, -197.5},
     en = {"The Boppers", "Beaten in Flashback E"},
     es = {"Los Boppers", "Vencidos en el Flashback E"}},
  },
  -- the trophy room's furniture, always there: a low counter along the west wall, two rows of five
  -- desks (dyn_desk_ab turned a quarter: 1.28 m along the wall, 0.62 m deep, its top 0.46 m up, its
  -- origin in the middle of its footprint), the back row against the wall; cage.col gives the counter a
  -- solid box. Two table lamps (their shades draw lit) stand on the back row.
  room = {
    {"dyn_desk_ab", {-174.53, 96.3, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-174.53, 95.0, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-174.53, 93.7, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-174.53, 92.4, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-174.53, 91.1, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-173.91, 96.3, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-173.91, 95.0, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-173.91, 93.7, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-173.91, 92.4, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_desk_ab", {-173.91, 91.1, -197.58}, {0, 0, 0.7071068, 0.7071068}},
    {"dyn_bar_tbllamp", {-174.5, 96.72, -197.12}, {0, 0, 0, -1}},
    {"dyn_bar_tbllamp", {-174.5, 92.82, -197.12}, {0, 0, 0, -1}},
  },
  -- warm lights like the hideout's own lamps (level95 AddGlows: {1, 0.929, 0.843}, radius 5-8): one
  -- above each table lamp's shade, two fills under the ceiling (the east fence, the south end). They
  -- are made here, while the level loads: a light made later is listed but stays off. They light the
  -- room with the runtime's lighting options (--level-lights); without them the game lights no world
  -- surface with a Lua light, and the lamps' shades and the photo cards still show.
  lights = {
    {-174.3, 96.72, -196.6, 6},
    {-174.3, 92.82, -196.6, 6},
    {-167.6, 91.0, -195.0, 7},
    {-170.6, 85.0, -195.0, 7},
  },
}

function HideoutTrophies.Place()
  local T = HideoutTrophies
  local placed = 0
  local n = 0
  local i = 1
  while T.list[i] do
    local t = T.list[i]
    if t.meshes and not (MOD_MESHES and MOD_MESHES >= t.meshes) then
      ModLog(t.objs[getn(t.objs)][1] .. ": optional meshes not installed, not shown")
    elseif UM_IsLevelComplete(t.level) then
      local j = 1
      while t.objs[j] do
        local o = t.objs[j]
        n = n + 1
        Objects["ModTrophy" .. n] = ObjSpawn(o[1], o[2], o[3], -1, 1, 0, 4294967295, nil)
        if o[4] then
          ObjEnablePhysics(Objects["ModTrophy" .. n], 0)
        end
        j = j + 1
      end
      t.shown = 1
      placed = placed + 1
    else
      ModLog(t.objs[getn(t.objs)][1] .. ": level " .. t.level .. " not complete, not shown")
    end
    i = i + 1
  end
  ModLog(placed .. " trophies placed")
  return placed
end

function HideoutTrophies.Label()
  local T = HideoutTrophies
  if not (Cameras and Cameras.follow and CL and CL.wht and CL.gry) then
    T.tries = T.tries + 1
    if T.tries < 60 then
      ScheduleFunc("HideoutTrophies.Label", 1000)
    else
      ModLog("no labels (the hideout camera never came up)")
    end
    return
  end
  local i = 1
  while T.list[i] do
    local t = T.list[i]
    if t.shown then
      local text = t.en
      if GetLanguage() == 1 then
        text = t.es
      end
      -- the game keeps pointers to these strings, not copies: they must stay referenced (in t)
      t.title = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT>" .. text[1] .. "</BIGFONT>"
      t.line = CL.wht .. text[2]
      t.flag = AddFlag("fModTrophy" .. i, t.spot, 0, 0, 0)
      SetMsgHandlerEx(t.flag, 0, "HideoutTrophies.NoAction", t.title, t.line)
      ModLog(t.objs[getn(t.objs)][1] .. ": label added")
    end
    i = i + 1
  end
end

function HideoutTrophies.NoAction(p0, p1)
  return true
end

-- the trophy room's furniture and lights, whatever is unlocked
function HideoutTrophies.Room()
  local T = HideoutTrophies
  local i = 1
  while T.room[i] do
    local o = T.room[i]
    Objects["ModTrophyRoom" .. i] = ObjSpawn(o[1], o[2], o[3], -1, 1, 0, 4294967295, nil)
    i = i + 1
  end
  T.lightIds = {}
  i = 1
  while T.lights[i] do
    local l = T.lights[i]
    T.lightIds[i] = SetLight(0, 0, {l[1], l[2], l[3]}, {0, 0, 0}, {1, 0.929, 0.843, 1}, l[4], 0, 0, 0.25, 1.5, 1, 0, 0, 1)
    i = i + 1
  end
  ModLog("trophy room: " .. getn(T.room) .. " pieces of furniture, " .. getn(T.lights) .. " lights")
end

HideoutTrophies.Room()
if HideoutTrophies.Place() > 0 then
  HideoutTrophies.Label()
end
