-- Jukebox: the walk-up prompt at the hideout's boom box (after.level95). The game's own boom box
-- (dyn_blaster at (-165.5, 96.3)) carries level95's "Start Next Level" spot (fMenu_5 at
-- (-166.2, 96.3)); the jukebox spot is a few steps south of it, in front of the crates, so the
-- two prompts never overlap. The prompt is the game's own walk-up message (AddFlag +
-- SetMsgHandlerEx, as the game's menu spots), set up once the hideout's follow camera exists; the
-- game keeps pointers to the prompt's strings: they stay referenced in Jukebox.prompt.
Jukebox.spot = {-166.3, 93.6, -194.3}
Jukebox.tries = 0

function Jukebox.Prompt()
  local J = Jukebox
  if not (Cameras and Cameras.follow and CL and CL.wht) then
    J.tries = J.tries + 1
    if J.tries < 60 then
      ScheduleFunc("Jukebox.Prompt", 1000)
    else
      ModLog("no jukebox spot (the hideout camera never came up)")
    end
    return
  end
  local x = J.Text()
  -- The button is drawn by <MONEYFONT><ST></MONEYFONT> (the action button: Y on the pad, E on the
  -- keyboard), as level95's own action hints.
  J.prompt = {title = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT>" .. x.title .. "</BIGFONT>",
              line = CL.gry .. x.press .. CL.wht .. "<MONEYFONT><ST></MONEYFONT>" .. CL.gry .. x.verb}
  J.flag = AddFlag("fModJukebox", J.spot, 0, 0, 0)
  SetMsgHandlerEx(J.flag, 0, "Jukebox.Open", J.prompt.title, J.prompt.line)
  ModLog("jukebox spot added")
end

-- The game's radio songs and DJ links are muted while a jukebox song plays (set before the
-- hideout's radio starts, so the runtime learns where the boom box is from its first song).
ModMusicMute(Jukebox.MuteList())
Jukebox.Prompt()
ScheduleFunc("Jukebox.Resume", 3000)
