-- Dirty clothes: the hideout's "Change clothes" spot (after.level95). A walk-up prompt (AddFlag +
-- SetMsgHandlerEx, as the game's own menu and Flashback spots), set up once the hideout's follow
-- camera exists; the action button calls DirtyClothes.Change (clothes.lua): the crew's wear goes
-- back to clean. The game keeps pointers to the prompt's strings, not copies: they stay referenced
-- in DirtyClothes.prompt.

DirtyClothes.spot = {-179.3, 86.6, -194.3}   -- main floor, beside the couch in the crew's corner
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
  local title, line, done = "Change clothes", "Clean clothes for the whole crew", "Clean clothes for the whole crew"
  if es then
    title, line, done = "Cambiarte de ropa", "Ropa limpia para toda la banda", "Ropa limpia para toda la banda"
  end
  D.prompt = {title = CL.wht .. "<CENTER><SIZE 2.0><BIGFONT>" .. title .. "</BIGFONT>", line = CL.wht .. line}
  D.done_text = done
  D.flag = AddFlag("fModClothes", D.spot, 0, 0, 0)
  SetMsgHandlerEx(D.flag, 0, "DirtyClothes.Change", D.prompt.title, D.prompt.line)
  ModLog("change-clothes spot added")
  if not D.running then
    D.running = 1
    ScheduleFunc("DirtyClothes.Watch", D.cfg.tick)
  end
end

DirtyClothes.Prompt()
