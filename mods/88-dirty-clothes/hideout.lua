-- Dirty clothes: the hideout's "Change clothes" spot (after.level95). A walk-up prompt (AddFlag +
-- SetMsgHandlerEx, as the game's own menu and Flashback spots), set up once the hideout's follow
-- camera exists; the action button calls DirtyClothes.Change (clothes.lua): the crew's wear goes
-- back to clean. The game keeps pointers to the prompt's strings, not copies: they stay referenced
-- in DirtyClothes.prompt.

-- The spot: on the upper floor, where the player comes in (the hideout puts him just inside the
-- double doors of the west brick wall, at about (-185.4, 95.0)): two steps south of there, between
-- the doors and the tall round bar table with its stools, on the way to the Rumble Mode and Replay
-- Levels spots of the south wall. Not on the arrival point itself (the prompt does not pop up as
-- the hideout opens) and away from the doors' own "Leave" spot (-187.0, 95.1). 1.2 had it at
-- (-179.3, 86.6), in the dark south-west corner by the black leather couch, where nobody found it.
DirtyClothes.spot = {-185.6, 92.6, -194.3}
DirtyClothes.tries = 0

function DirtyClothes.Prompt()
  local D = DirtyClothes
  if not (Cameras and Cameras.follow and CL and CL.wht) then
    D.tries = D.tries + 1
    if D.tries < 60 then
      ScheduleFunc("DirtyClothes.Prompt", 1000)
    else
      ModLog("no change-clothes spot (the hideout camera never came up)")
    end
    return
  end
  local es = GetLanguage and GetLanguage() == 1
  -- The line is the game's own action hint (level95's LEVEL95.ACTC): "press <button> to ...", the
  -- button drawn by <MONEYFONT><ST></MONEYFONT> (the action button: Y on the pad, E on the keyboard).
  -- What pressing it did is told as the game's own announcement (HUDSetAnnounceMsg, as level95's
  -- "New Feature Unlocked"), under the title: the message box keeps the prompt's line while the
  -- player stands at the spot, so a tutorial text there is never seen.
  local title, press, verb, done = "Change clothes", "press ", " to change clothes",
    "Clean clothes for the whole crew"
  if es then
    -- level95 says "presionar" where Platform == 2 (this port) and "pulsa" elsewhere
    press = (Platform == 2 and "presionar ") or "pulsa "
    title, verb, done = "Cambiarte de ropa", " para cambiarte de ropa",
      "Ropa limpia para toda la banda"
  end
  D.prompt = {title = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT>" .. title .. "</BIGFONT>",
              line = CL.gry .. press .. CL.wht .. "<MONEYFONT><ST></MONEYFONT>" .. CL.gry .. verb}
  D.done_text = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT><DISPLAYTIME 2250>" .. done .. "</BIGFONT>"
  D.flag = AddFlag("fModClothes", D.spot, 0, 0, 0)
  SetMsgHandlerEx(D.flag, 0, "DirtyClothes.Change", D.prompt.title, D.prompt.line)
  ModLog("change-clothes spot added")
  if not D.running then
    D.running = 1
    ScheduleFunc("DirtyClothes.Watch", D.cfg.tick)
  end
end

DirtyClothes.Prompt()
