-- FRIDAY Bridge B1-Linux gate: does audio pass through the renderer plugin?
-- Run headless:  reaper -nosplash /data/build/b1-gate/b1_passthrough.lua
-- Writes /data/build/b1-gate/b1-result.txt and renders /data/build/b1-gate/b1-out.wav

local OUT_DIR = "/data/build/b1-gate"
local LOG = OUT_DIR .. "/b1-result.txt"
local SRC = OUT_DIR .. "/test-714.wav"

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w")
  f:write(table.concat(lines, "\n") .. "\n")
  f:close()
end

log("=== FRIDAY Bridge B1-Linux pass-through ===")

-- 1. Track with 12 channels (SMPTE 7.1.4)
reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
reaper.SetMediaTrackInfo_Value(track, "I_NCHAN", 12)
reaper.GetSetMediaTrackInfo_String(track, "P_NAME", "B1 test", true)
reaper.SetOnlyTrackSelected(track)
log("track created, I_NCHAN=" .. reaper.GetMediaTrackInfo_Value(track, "I_NCHAN"))

-- 2. Test signal as an item at 0:00
reaper.SetEditCurPos(0, false, false)
local ok = reaper.InsertMedia(SRC, 0)
log("InsertMedia -> " .. tostring(ok) .. ", items on track=" .. reaper.CountTrackMediaItems(track))

-- 3. The plugin under test
local fx = reaper.TrackFX_AddByName(track, "Eclipsa Audio Renderer", false, 1)
log("TrackFX_AddByName(Eclipsa Audio Renderer) -> index " .. tostring(fx))
if fx < 0 then
  log("RESULT: FAIL — plugin could not be instantiated")
  reaper.Main_OnCommand(40004, 0)
  return
end
local _, fxname = reaper.TrackFX_GetFXName(fx >= 0 and track or track, fx, "")
log("fx name: " .. tostring(fxname))
log("fx enabled: " .. tostring(reaper.TrackFX_GetEnabled(track, fx)))
log("fx count on track: " .. reaper.TrackFX_GetCount(track))

-- 4. Render settings — master mix, entire project, 12ch, 48k, 24-bit WAV
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)   -- master mix
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 1, true) -- entire project
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUT_DIR, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "b1-out", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true) -- WAV 24-bit
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)
log("render config set")

-- 5. Render using the most recent settings (no dialog)
reaper.Main_OnCommand(41824, 0)
log("render command issued")

reaper.Main_OnCommand(40004, 0) -- File: Quit REAPER
