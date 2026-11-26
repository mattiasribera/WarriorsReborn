-- A crate of our own (docs/MODDING.md "Your own assets"). The hideout (level95) has just loaded:
-- spawn the host object (a sheet of paper nothing else places here) where assets.ini's
-- [collision crate-col] stands, give the mesh group its point so the paper is drawn as our crate,
-- and knock when the player walks into it. ModSound is bound to this mod while its file runs: keep
-- the copy (OwnCrate.Sound) for later calls.
OwnCrate = {pos = {-184.2, 95.0, -194.38}, reach = 0.85, Sound = ModSound, quiet = 0}

function OwnCrate.Knock(volume)
  local p = OwnCrate.pos
  OwnCrate.Sound("knock", p[1], p[2], p[3] + 0.3, volume)
end

function OwnCrate.Place()
  local C = OwnCrate
  if not (MOD_ASSETS and MOD_ASSETS >= 3) then
    ModLog("the crate is not built (see the [modassets] lines of the log); not placed")
    return
  end
  local p = C.pos
  C.obj = ObjSpawn("dyn_s_paper_list", p, {0, 0, 0, -1}, -1, 1, 0, 4294967295, nil)
  if not C.obj then
    ModLog("the host object could not be spawned")
    return
  end
  ObjEnablePhysics(C.obj, 0)
  ModMeshPoint("own-crate/crate", "crate", p[1], p[2], p[3])
  ModMeshSet("own-crate/crate", 1)
  ModLog("crate placed at " .. p[1] .. " " .. p[2] .. " " .. p[3])
  C.Knock(1)
  ScheduleFunc("OwnCrate.Poll", 250)
end

-- Every 250 ms: a knock when the player is pressed against the crate (at most every 1.5 s).
function OwnCrate.Poll()
  local C = OwnCrate
  if C.quiet > 0 then C.quiet = C.quiet - 1 end
  if player and GetPosition then
    local q = GetPosition(player)
    if q then
      local dx, dy = q.x - C.pos[1], q.y - C.pos[2]
      if C.quiet == 0 and dx * dx + dy * dy < C.reach * C.reach and abs(q.z - C.pos[3]) < 1 then
        C.Knock(0.8)
        ModLog("bump")
        C.quiet = 6
      end
    end
  end
  ScheduleFunc("OwnCrate.Poll", 250)
end

OwnCrate.Place()
