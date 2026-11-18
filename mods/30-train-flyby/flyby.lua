-- Unused content: train flyby. level9_chap2_tags has just been loaded: wrap ch2.ContinueLevel, which
-- the game calls when chapter 2's objective (bomb four more fresh trains) starts, and play the
-- retail scene l9_flyby1 once, 1.5 s later. SuperRunScene preloads the scene and plays it; when it
-- ends, the player's camera and the HUD come back (TrainFlyby.Done).
if ch2 and ch2.ContinueLevel and not TrainFlyby then
  TrainFlyby = {continue = ch2.ContinueLevel}

  ch2.ContinueLevel = function(p0)
    TrainFlyby.continue(p0)
    if not TrainFlyby.played then
      TrainFlyby.played = 1
      ScheduleFunc("TrainFlyby.Play", 1500)
    end
  end

  TrainFlyby.Play = function()
    ModLog("playing l9_flyby1")
    SuperRunScene({SceneId = "l9_flyby1", ReturnFunc = TrainFlyby.Done, NoClearWanted = true, FadeIn = true})
  end

  TrainFlyby.Done = function(p0)
    if player then CameraMakeActive(MainCam, 0, nil, player) end
    if player2 and HuIsAPlayer(player2) then CameraMakeActive(MainCam, 0, nil, player2) end
    CameraReset(MainCam)
    ShowHud(0)
    ModLog("l9_flyby1 done")
  end
end
