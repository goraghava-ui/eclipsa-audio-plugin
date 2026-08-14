-- FRIDAY Bridge — B7 gate B: the pose the old dome surface got wrong.
--
-- Gate A only proves the change did not reach the flat path. This is the one
-- that exercises it: elevation mode "dome", az 0, el 60. Z is NOT written by
-- this script — X and Y are set and ElevationListener derives the height from
-- the dome surface, which is exactly the code that changed.
--
--   x = -50 * sin(0) * cos(60) =   0.00000
--   y =  50 * cos(0) * cos(60) =  25.00000  -> normalised radius 0.5
-- normalised over [-50, +50]: (v + 50) / 100.
--
-- On the unit sphere the surface answers height = sqrt(1 - 0.5^2) = 0.8660254,
-- so Z = 43.30127 and the elevation is acos(0.5) = 60.000 deg — Studio's pose.
-- On the old surface it answered 2*sqrt(0.75) - 1 = 0.7320508, Z = 36.60254
-- and 55.67 deg: 4.33 deg low, into a 7.1.4 layout whose nearest speakers are
-- at 45 deg, so the VBAP gains would differ and the null would open up.
--
-- The Z_NORM write below is the starting value the listener overwrites; the
-- "captured" line in the log is the number that matters.

local OUT_DIR = "/data/build/b7"
local SRC = OUT_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/b-gate-result.txt"
local IAMF = OUT_DIR .. "/bridge_b.iamf"


local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end

for _, p in ipairs({IAMF, OUT_DIR .. "/bgate.rpp", OUT_DIR .. "/b-monitor.wav",
                    OUT_DIR .. "/bridge_b.fstudio"}) do
  os.remove(p)
end
os.execute("rm -f '" .. OUT_DIR .. "'/bridge_b.iamf_*.wav '" ..
           OUT_DIR .. "'/bridge_b_*.wav")

log("=== B7 gate B: elevated pose, az 0 el 60, elevation=dome ===")
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?"))

reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
local rchunk = slurp(OUT_DIR .. "/renderer-b.b64")
if rfx >= 0 and rchunk then
  log("renderer state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(rtrack, rfx, "vst_chunk", rchunk)))
end

reaper.InsertTrackAtIndex(1, true)
local strack = reaper.GetTrack(0, 1)
reaper.GetSetMediaTrackInfo_String(strack, "P_NAME", "source", true)
reaper.SetMediaTrackInfo_Value(strack, "I_NCHAN", 12)
reaper.SetOnlyTrackSelected(strack)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(SRC, 0)
local sfx = reaper.TrackFX_AddByName(strack, "Eclipsa Audio Element Plugin", false, 1)
local pchunk = slurp(OUT_DIR .. "/panner-b.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

-- X and Y arrive with the state (X = 0, Y = 25, Z = 0). Nothing is written
-- here on purpose: the height in the exported file has to come from the dome
-- surface, and if this script wrote Z the gate would be testing itself.
if sfx >= 0 then
  local _, xv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 2, "")
  local _, yv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 3, "")
  local _, zv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 4, "")
  log("loaded X=" .. xv .. " Y=" .. yv .. " Z=" .. zv .. " (before the surface runs)")
end

reaper.SetMediaTrackInfo_Value(strack, "B_MAINSEND", 0)
local send = reaper.CreateTrackSend(strack, rtrack)
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_SRCCHAN", 0 | (4 << 10))
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_DSTCHAN", 0)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)

reaper.GetSet_LoopTimeRange(true, false, 0.0, 2.0, false)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUT_DIR, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "b-monitor", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true)

local tick = 0
local stage = "settle"
local function step()
  tick = tick + 1
  if stage == "settle" then
    -- Let the message thread dispatch any parameter listeners before the
    -- bounce, then record the position that was actually captured.
    if tick > 30 then
      local _, xv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 2, "")
      local _, yv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 3, "")
      local _, zv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 4, "")
      log("captured X=" .. xv .. " Y=" .. yv .. " Z=" .. zv)
      reaper.Main_OnCommand(41824, 0)
      log("bounce issued")
      stage = "play"; tick = 0
    end
  elseif stage == "play" then
    if tick > 30 then
      reaper.OnPlayButton(); stage = "stop"; tick = 0
    end
  elseif stage == "stop" then
    if tick > 60 then
      reaper.OnStopButton(); stage = "finish"; tick = 0
    end
  elseif stage == "finish" then
    if tick > 150 then
      log("iamf exists=" .. tostring(reaper.file_exists(IAMF)))
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/bgate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(step)
end
reaper.defer(step)
