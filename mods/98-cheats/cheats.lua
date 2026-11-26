-- Cheats menu (after.global, every Lua state): the game's own cheats from a value menu instead of
-- button codes. The game's code entry (native) only calls the Lua callback global.lua registers
-- with SetCheatCallback("DbgEnterCheat") with the cheat's number; DbgEnterCheat(n) checks the
-- player, runs DoCheat(n, player, 1), plays the cheat's voice line and shows "Cheat Activated!",
-- then runs DoCheat(n, player2, 2). This mod calls DbgEnterCheat(n) itself, so a cheat from the
-- menu is exactly the cheat from its code. Only switching a toggle off, which the codes cannot do,
-- uses the game functions the cheat set (HuSetGodMode / HuSetTireless), and unlimited rage is the
-- game's own on / off code (CheatRage).
--
-- Game rule kept: when rage ends the game's ClrRageMode clears god mode, so unlimited health ends
-- with it (as with the codes); the menu then shows it off. Toggles that are on are kept for the
-- rest of the run (ModKeep, never written) and set again, silently (DoCheat), in the next level.
-- Opening: ModMenuKey (Back + LB held, or F8), only in a level with a living player.
-- Texts: English and neutral Spanish ("tú"); the game font takes Latin-1 escapes.

if not Cheats then
  Cheats = {}
end
Cheats.Keep = ModKeep
Cheats.Kept = ModKept
Cheats.Log = ModLog

-- id: the game's cheat number (DoCheat); kind: "toggle", "once" or "save" (changes the save
-- for good: confirmed with a second A).
Cheats.list = {
  {id = 1, kind = "toggle", key = "health", en = "Unlimited health", es = "Salud ilimitada",
   note_en = "You take no damage. Ends when your rage ends, as in the game.",
   note_es = "No recibes da\241o. Termina cuando termina tu furia, como en el juego."},
  {id = 5, kind = "toggle", key = "rage", en = "Unlimited rage", es = "Furia ilimitada",
   note_en = "Your rage bar stays full and fills again after each rage.",
   note_es = "Tu barra de furia se queda llena y se vuelve a llenar tras cada furia."},
  {id = 2, kind = "toggle", key = "sprint", en = "Unlimited sprint", es = "Sprint ilimitado",
   note_en = "You never tire while sprinting or fighting.",
   note_es = "Nunca te cansas al correr ni al pelear."},
  {id = 3, kind = "once", en = "Lose the cops", es = "Perder a la polic\237a",
   note_en = "The police stop chasing the Warriors.",
   note_es = "La polic\237a deja de perseguir a los Warriors."},
  {id = 6, kind = "once", en = "Get a knife", es = "Conseguir un cuchillo",
   note_en = "Puts a knife in your hand (not in boss fights).",
   note_es = "Te pone un cuchillo en la mano (no en peleas contra jefes)."},
  {id = 7, kind = "once", en = "Get a bat", es = "Conseguir un bate",
   note_en = "Puts a bat in your hand (not in boss fights).",
   note_es = "Te pone un bate en la mano (no en peleas contra jefes)."},
  {id = 8, kind = "once", en = "Get a lead pipe", es = "Conseguir un tubo de plomo",
   note_en = "Puts a lead pipe in your hand (not in boss fights).",
   note_es = "Te pone un tubo de plomo en la mano (no en peleas contra jefes)."},
  {id = 9, kind = "once", en = "Get a machete", es = "Conseguir un machete",
   note_en = "Puts a machete in your hand (not in boss fights).",
   note_es = "Te pone un machete en la mano (no en peleas contra jefes)."},
  {id = 11, kind = "once", en = "Get an unbreakable bat", es = "Conseguir un bate irrompible",
   note_en = "Puts a bat that never breaks in your hand (not in boss fights).",
   note_es = "Te pone en la mano un bate que nunca se rompe (no en peleas contra jefes)."},
  {id = 26, kind = "once", en = "99 credits", es = "99 cr\233ditos",
   note_en = "Armies of the Night only: sets your credits to 99.",
   note_es = "Solo en Armies of the Night: pone tus cr\233ditos en 99."},
  {id = 0, kind = "save", en = "$200, flash and spray paint", es = "200 $, flash y aerosol",
   note_en = "Changes your save: your money becomes $200 (also if you had more), with 9 flash and 9 spray paint.",
   note_es = "Cambia tu partida guardada: tu dinero pasa a ser 200 $ (aunque tuvieras m\225s), con 9 flash y 9 aerosoles."},
  {id = 12, kind = "save", en = "Complete current mission", es = "Completar la misi\243n actual",
   note_en = "Changes your save: the mission counts as completed and the next one starts.",
   note_es = "Cambia tu partida guardada: la misi\243n cuenta como completada y empieza la siguiente."},
  {id = 13, kind = "save", en = "100% completion", es = "Completar al 100 %",
   note_en = "Changes your save: unlocks every mission, bonus and upgrade, then completes this mission.",
   note_es = "Cambia tu partida guardada: desbloquea todas las misiones, extras y mejoras, y completa esta misi\243n."},
  {id = 14, kind = "save", en = "Brass knuckles", es = "Pu\241o americano",
   note_en = "Changes your save: unlocks brass knuckles for good.",
   note_es = "Cambia tu partida guardada: desbloquea el pu\241o americano para siempre."},
  {id = 15, kind = "save", en = "Steel-toed boots", es = "Botas con punta de acero",
   note_en = "Changes your save: unlocks steel-toed boots for good.",
   note_es = "Cambia tu partida guardada: desbloquea las botas con punta de acero para siempre."},
  {id = 16, kind = "save", en = "Cuff keys", es = "Llaves de esposas",
   note_en = "Changes your save: unlocks cuff keys, to free arrested Warriors.",
   note_es = "Cambia tu partida guardada: desbloquea las llaves de esposas, para liberar a Warriors arrestados."},
  {id = 17, kind = "save", en = "Handcuffs", es = "Esposas",
   note_en = "Changes your save: unlocks handcuffs for good.",
   note_es = "Cambia tu partida guardada: desbloquea las esposas para siempre."},
  {id = 18, kind = "save", en = "Uncuff yourself", es = "Quitarte las esposas",
   note_en = "Changes your save: unlocks freeing yourself from handcuffs.",
   note_es = "Cambia tu partida guardada: desbloquea liberarte t\250 mismo de las esposas."},
  {id = 19, kind = "save", en = "More flash capacity", es = "M\225s capacidad de flash",
   note_en = "Changes your save: unlocks the flash capacity upgrade for good.",
   note_es = "Cambia tu partida guardada: desbloquea la mejora de capacidad de flash para siempre."},
  {id = 20, kind = "save", en = "Flashhead", es = "Flashhead",
   note_en = "Changes your save: unlocks the Flashhead upgrade for good.",
   note_es = "Cambia tu partida guardada: desbloquea la mejora Flashhead para siempre."},
  {id = 21, kind = "save", en = "More combat stamina", es = "M\225s resistencia en combate",
   note_en = "Changes your save: boosts your combat stamina for good.",
   note_es = "Cambia tu partida guardada: aumenta tu resistencia en combate para siempre."},
  {id = 22, kind = "save", en = "More sprint stamina", es = "M\225s resistencia al correr",
   note_en = "Changes your save: boosts your sprint stamina for good.",
   note_es = "Cambia tu partida guardada: aumenta tu resistencia al correr para siempre."},
  {id = 23, kind = "save", en = "Dealer hints", es = "Pistas de vendedores",
   note_en = "Changes your save: unlocks the dealer hints for good.",
   note_es = "Cambia tu partida guardada: desbloquea las pistas de vendedores para siempre."},
  {id = 24, kind = "save", en = "Hobo allies", es = "Vagabundos aliados",
   note_en = "Changes your save: unlocks the hobo allies for good.",
   note_es = "Cambia tu partida guardada: desbloquea a los vagabundos aliados para siempre."},
  {id = 25, kind = "save", en = "Knife dealers", es = "Vendedores de cuchillos",
   note_en = "Changes your save: unlocks the knife dealers for good.",
   note_es = "Cambia tu partida guardada: desbloquea a los vendedores de cuchillos para siempre."},
}

function Cheats.Es()
  return GetLanguage and GetLanguage() == 1
end

function Cheats.T(en, es)
  if Cheats.Es() then
    return es
  end
  return en
end

-- A player who can take a cheat (DbgEnterCheat's own test).
function Cheats.Ready()
  return Level ~= 100 and player and player ~= NilHandle and HuIsAPlayer(player) and HuIsAlive(player)
end

-- Whether a toggle is on now: rage is the game's own CheatRage, the other two the mod's flags
-- (the game has no getter for god mode or tireless).
function Cheats.IsOn(c)
  if c.key == "rage" then
    return CheatRage
  end
  return Cheats.on[c.key]
end

function Cheats.Values()
  local vals, notes = "", ""
  local i = 1
  while Cheats.list[i] do
    local c = Cheats.list[i]
    local v
    if c.kind == "toggle" then
      if Cheats.IsOn(c) then v = Cheats.T("On", "S\237") else v = Cheats.T("Off", "No") end
    elseif Cheats.armed == i then
      v = Cheats.T("A again: yes", "A otra vez: s\237")
    elseif Cheats.refused == i then
      v = Cheats.T("Not now", "Ahora no")
    elseif c.kind == "save" then
      v = Cheats.T("Changes save", "Cambia partida")
    else
      v = Cheats.T("Use", "Usar")
    end
    if i > 1 then
      vals = vals .. "\n"
      notes = notes .. "\n"
    end
    vals = vals .. v
    notes = notes .. Cheats.T(c.note_en, c.note_es)
    i = i + 1
  end
  return vals, notes
end

-- The game's DoCheat, wrapped by global name to learn whether player 1's cheat took (DbgEnterCheat
-- returns nothing).
function Cheats.DoCheat(n, who, idx)
  local r = Cheats.gameDoCheat(n, who, idx)
  if idx == 1 then
    Cheats.took = r
    if r and n == 1 then Cheats.on.health = 1 Cheats.Keep("health", "1") end   -- also a typed code
    if r and n == 2 then Cheats.on.sprint = 1 Cheats.Keep("sprint", "1") end
  end
  return r
end

-- The game's SetRageMode / ClrRageMode (rage starts / ends), wrapped: rage gives god mode and its
-- end clears it, so unlimited health is off after it.
function Cheats.SetRageMode(h, x)
  Cheats.gameSetRageMode(h, x)
  if h == player then Cheats.inRage = 1 end
end

function Cheats.ClrRageMode(h, x)
  Cheats.gameClrRageMode(h, x)
  if h == player then Cheats.inRage = nil end
  if h == player and Cheats.on.health then
    Cheats.on.health = nil
    Cheats.Keep("health", nil)
    Cheats.Log("rage ended: unlimited health off (the game's rule)")
    Cheats.Refresh()
  end
end

function Cheats.Run(n)
  Cheats.took = nil
  DbgEnterCheat(n)
  return Cheats.took
end

function Cheats.Choose(i, dir)
  local c = Cheats.list[i]
  if not c then
    return
  end
  Cheats.refused = nil
  if c.kind == "toggle" then
    if Cheats.IsOn(c) then
      if c.key == "rage" then
        Cheats.Run(5)
      elseif c.key == "health" then
        if not Cheats.inRage then   -- in rage the game holds god mode and clears it when rage ends
          HuSetGodMode(player, 0)
          if player2 and player2 ~= NilHandle then HuSetGodMode(player2, 0) end
        end
      else
        HuSetTireless(player, 0)
        if player2 and player2 ~= NilHandle then HuSetTireless(player2, 0) end
      end
      Cheats.on[c.key] = nil
      Cheats.Log(c.en .. " off")
    elseif Cheats.Run(c.id) then
      Cheats.on[c.key] = 1
      Cheats.Log(c.en .. " on")
    else
      Cheats.refused = i
      Cheats.Log(c.en .. ": the game refused it here")
    end
    if Cheats.IsOn(c) then
      Cheats.Keep(c.key, "1")
    else
      Cheats.Keep(c.key, nil)
    end
    Cheats.armed = nil
    return
  end
  if dir ~= 0 then   -- left / right only switch toggles
    return
  end
  if c.kind == "save" and Cheats.armed ~= i then
    Cheats.armed = i
    return
  end
  Cheats.armed = nil
  if Cheats.Run(c.id) then
    Cheats.Log(c.en .. " (cheat " .. c.id .. ")")
    if c.id == 12 or c.id == 13 then   -- the mission ends: the menu closes
      ModMenuClose()
    end
  else
    Cheats.refused = i
    Cheats.Log(c.en .. ": the game refused it here")
  end
end

function Cheats.Refresh()
  if Cheats.open then
    local v, n = Cheats.Values()
    ModMenuValues(v)
  end
end

function Cheats.Open()
  local labels = ""
  local i = 1
  while Cheats.list[i] do
    if i > 1 then labels = labels .. "\n" end
    labels = labels .. Cheats.T(Cheats.list[i].en, Cheats.list[i].es)
    i = i + 1
  end
  if not ModMenuOpen(Cheats.T("Cheats", "Trucos"), labels,
                     Cheats.T("A: use / switch     B: close", "A: usar / cambiar     B: cerrar"), Cheats.sel or 1, nil) then
    return
  end
  Cheats.open = 1
  Cheats.armed = nil
  Cheats.refused = nil
  local v, n = Cheats.Values()
  ModMenuValues(v, n)
  Cheats.Log("menu open")
end

-- Toggles kept from an earlier level, set again once the player is up (silently: DoCheat).
function Cheats.Reapply()
  Cheats.applied = 1
  local who = {player, player2}
  local k = 1
  while k <= 2 do
    local h = who[k]
    if h and h ~= NilHandle and HuIsAPlayer(h) then
      if Cheats.carry.health then Cheats.gameDoCheat(1, h, k) end
      if Cheats.carry.sprint then Cheats.gameDoCheat(2, h, k) end
    end
    k = k + 1
  end
  if Cheats.carry.rage and not CheatRage then
    Cheats.gameDoCheat(5, player, 1)   -- player 1 only: the game's toggle is one global
  end
  if Cheats.carry.health or Cheats.carry.sprint or Cheats.carry.rage then
    Cheats.Log("kept cheats set again")
  end
end

-- The hub (level95) drops calls scheduled before its scripts run, which stopped the menu key
-- from being polled in free roam. Tick records when it last ran; Arm schedules it again when
-- it is overdue, from the GetPosition and HuCreate wrappers below (the hub calls both often).
function Cheats.Arm()
  local now = GetGameTime and GetGameTime()
  if not now then return end
  if not Cheats.ticked or now - Cheats.ticked > 2000 then
    Cheats.ticked = now
    ScheduleFunc("Cheats.Tick", 250)
  end
end

function Cheats.GetPosition(...)
  Cheats.Arm()
  return call(Cheats.gameGetPosition, arg)
end

function Cheats.HuCreate(...)
  Cheats.Arm()
  return call(Cheats.gameHuCreate, arg)
end

function Cheats.Tick()
  Cheats.ticked = GetGameTime and GetGameTime()
  if Cheats.open then
    local state, i, dir = ModMenuState()
    if state == "chosen" then
      Cheats.sel = i
      Cheats.Choose(i, dir)
      Cheats.Refresh()
    elseif state ~= "open" then
      Cheats.open = nil
      if i then Cheats.sel = i end
    end
    ScheduleFunc("Cheats.Tick", 100)
    return
  end
  if Cheats.Ready() then
    if not Cheats.applied then
      Cheats.Reapply()
    end
    if ModMenuKey() then
      Cheats.Open()
    end
  end
  ScheduleFunc("Cheats.Tick", 250)
end

-- Once per Lua state (global.lua runs once in each).
if DoCheat and DbgEnterCheat and not Cheats.state then
  Cheats.state = 1
  Cheats.gameDoCheat = DoCheat
  DoCheat = Cheats.DoCheat
  if SetRageMode and ClrRageMode then
    Cheats.gameSetRageMode = SetRageMode
    SetRageMode = Cheats.SetRageMode
    Cheats.gameClrRageMode = ClrRageMode
    ClrRageMode = Cheats.ClrRageMode
  end
  -- what earlier levels left on (set again once the player is up) and the mod's own flags
  Cheats.carry = {health = Cheats.Kept("health"), sprint = Cheats.Kept("sprint"), rage = Cheats.Kept("rage")}
  Cheats.on = {health = Cheats.carry.health, sprint = Cheats.carry.sprint}
  if GetPosition and not Cheats.gameGetPosition then   -- the hub's own loops call it often
    Cheats.gameGetPosition = GetPosition
    GetPosition = Cheats.GetPosition
  end
  if HuCreate and not Cheats.gameHuCreate then
    Cheats.gameHuCreate = HuCreate
    HuCreate = Cheats.HuCreate
  end
  Cheats.Log("cheats menu ready: hold Back + LB or press F8")
  Cheats.ticked = GetGameTime and GetGameTime()
  ScheduleFunc("Cheats.Tick", 1000)
end
