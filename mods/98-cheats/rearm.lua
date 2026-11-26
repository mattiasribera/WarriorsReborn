-- After the hub script (level95): it drops calls scheduled earlier, so poll the menu key again.
if Cheats and Cheats.Arm then
  Cheats.ticked = nil
  Cheats.Arm()
end
