-- Minimal exit probe: load both plugins, roll nothing, quit. If the exit
-- crash is about static teardown it does not need an export to show up.
local OUT = "/data/build/sweep"
local f = io.open(OUT .. "/quit-probe.txt", "w")
local function log(s) f:write(tostring(s) .. "\n"); f:flush() end

log("probe start")
reaper.InsertTrackAtIndex(0, true)
local t0 = reaper.GetTrack(0, 0)
reaper.SetMediaTrackInfo_Value(t0, "I_NCHAN", 12)
log("renderer fx=" .. reaper.TrackFX_AddByName(t0, "Eclipsa Audio Renderer", false, 1))
reaper.InsertTrackAtIndex(1, true)
local t1 = reaper.GetTrack(0, 1)
reaper.SetMediaTrackInfo_Value(t1, "I_NCHAN", 12)
log("panner fx=" .. reaper.TrackFX_AddByName(t1, "Eclipsa Audio Element Plugin", false, 1))

local tick = 0
local function step()
  tick = tick + 1
  if tick > 60 then
    log("quitting")
    f:close()
    reaper.Main_SaveProjectEx(0, OUT .. "/quitprobe.rpp", 0)
    reaper.Main_OnCommand(40004, 0)
    return
  end
  reaper.defer(step)
end
reaper.defer(step)
