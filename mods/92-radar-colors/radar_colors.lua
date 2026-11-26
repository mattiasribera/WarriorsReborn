-- Gang colours on the radar (boot; docs/MODDING.md, "Gang colours on the radar").
--
-- The game draws every enemy on the radar as the same orange dot and every cop as the same blue
-- one. This mod gives the runtime (ModRadarColor, mods version 1.9) one solid colour per character
-- id: while the game configures its characters (CfgChar, at start-up), each id whose model name
-- starts with a prefix of RadarColors.gangs gets that gang's colour. The dot of every character of
-- that id then has it, wherever the game puts him on the radar. Cops other than transit and riot
-- cops, the Warriors, civilians and objective markers keep the game's own colours.
--
-- Colours are "RRGGBB". The radar is drawn over the dark map, so black and dark blue are lifted to
-- shades that stay visible there: black is a dark slate grey (5A5A6E) and dark blue a mid blue (2F5BFF).
if not RadarColors then
  RadarColors = {}
  local black, darkblue = "5A5A6E", "2F5BFF"
  RadarColors.colours = {
    green = "39FF3C", black = black, darkblue = darkblue, yellow = "FFE21F", olive = "8C9A2B",
    grey = "A6A6A6", red = "E8261E", pink = "FF5FC8", purple = "A23CF0", orange = "FF8A1E",
  }
  -- Model name prefix (the 10th CfgChar argument) -> colour name. Longer prefixes first: the
  -- first match wins. 8 Balls and Van Cortlandt Rangers have no models of their own in the game.
  RadarColors.gangs = {
    {"cops_ty", "green"},   -- transit cops (the subway's rail police)
    {"cops_vc", "black"},   -- riot cops (riot visor)
    {"dest", "darkblue"},   -- Destroyers
    {"moon", "yellow"},     -- Moonrunners
    {"orph", "olive"},      -- Orphans
    {"fury", "grey"},       -- Baseball Furies
    {"hiha", "red"},        -- Hi-Hats
    {"hurr", "darkblue"},   -- Hurricanes
    {"sava", "olive"},      -- Savage Huns
    {"turn", "red"},        -- Turnbull ACs
    {"jsbs", "black"},      -- Jones Street Boys
    {"lizz", "pink"},       -- Lizzies
    {"elec", "yellow"},     -- Electric Eliminators
    {"rogu", "grey"},       -- Rogues
    {"samo", "red"},        -- Satan's Mothers
    {"punk", "yellow"},     -- Punks
    {"bopp", "purple"},     -- Boppers
    {"panz", "olive"},      -- Panzers
    {"sara", "black"},      -- Saracens
    {"riff", "orange"},     -- Riffs
  }
  RadarColors.set = 0

  -- The colour ("RRGGBB") for a model name, or nil.
  function RadarColors.ColourOf(model)
    if type(model) ~= "string" then return nil end
    local i = 1
    while RadarColors.gangs[i] do
      local g = RadarColors.gangs[i]
      if strsub(model, 1, strlen(g[1])) == g[1] then return RadarColors.colours[g[2]] end
      i = i + 1
    end
    return nil
  end
end

if MOD_SCRIPT == "config_preload2" then
  ModLog("gang colours: " .. RadarColors.set .. " character ids coloured")
-- Every Lua state: wrap CfgChar once (the front end's state configures the gang characters; the
-- runtime keeps the colours until the game exits).
elseif ModRadarColor and CfgChar and not RadarColors_wrapped then
  RadarColors_wrapped = 1
  RadarColors.CfgChar = CfgChar
  RadarColors.ModRadarColor = ModRadarColor
  CfgChar = function(...)
    local r = call(RadarColors.CfgChar, arg)
    local c = RadarColors.ColourOf(arg[10])
    if c and type(arg[1]) == "number" and RadarColors.ModRadarColor(arg[1], c) then
      RadarColors.set = RadarColors.set + 1
    end
    return r
  end
elseif not ModRadarColor and not RadarColors_warned then
  RadarColors_warned = 1
  ModLog("ModRadarColor missing (mods version 1.9 needed): radar colours off")
end
