-- FRIDAY Bridge — automation ramp gate (plan section 11).
--
-- A FAST azimuth sweep, -90 to +90 in one second, written as REAPER parameter
-- envelopes so the host interpolates them and every processBlock sees a fresh
-- position. That is what makes the capture carry real automation rather than
-- one frozen point.
--
--   x = -50 * sin(az),  y = 50 * cos(az),  el 0
--   az -90 -> x = +50, y =  0
--   az   0 -> x =   0, y = 50
--   az +90 -> x = -50, y =  0
-- normalised over [-50, +50]: (v + 50) / 100.
--
-- The export writes BOTH the .iamf and a .fstudio handoff carrying the
-- keyframes it captured. The reference is studio_cli rendering THAT handoff,
-- so both sides are given the identical automation by construction and the
-- null measures only how each one renders it.

local OUT_DIR = "/data/build/sweep"
local SRC = OUT_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/sweep-gate-result.txt"
local IAMF = OUT_DIR .. "/bridge_sweep.iamf"

local X_NORM = 0.25        -- -25.00000
local Y_NORM = 0.9330127   --  43.30127
local Z_NORM = 0.5         --    0.00000

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end

for _, p in ipairs({IAMF, OUT_DIR .. "/sweepgate.rpp", OUT_DIR .. "/sweep-monitor.wav",
                    OUT_DIR .. "/bridge_sweep.fstudio"}) do
  os.remove(p)
end
os.execute("rm -f '" .. OUT_DIR .. "'/bridge_sweep.iamf_*.wav '" ..
           OUT_DIR .. "'/bridge_sweep_*.wav")

log("=== automation ramp gate: az -90 -> +90 in 1 s ===")
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?"))

reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
local rchunk = slurp(OUT_DIR .. "/renderer-sweep.b64")
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
local pchunk = slurp(OUT_DIR .. "/panner-sweep.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

local SWEEP_SECONDS = 1.0
local POINTS = 200

if sfx >= 0 then
  -- Park Z at 0; only azimuth moves.
  reaper.TrackFX_SetParam(strack, sfx, 4, 0.5)

  local xenv = reaper.GetFXEnvelope(strack, sfx, 2, true)
  local yenv = reaper.GetFXEnvelope(strack, sfx, 3, true)
  if xenv and yenv then
    reaper.DeleteEnvelopePointRange(xenv, -1, 1e9)
    reaper.DeleteEnvelopePointRange(yenv, -1, 1e9)
    for i = 0, POINTS do
      local frac = i / POINTS
      local t = frac * SWEEP_SECONDS
      local az = (-90.0 + 180.0 * frac) * math.pi / 180.0
      local x = -50.0 * math.sin(az)
      local y = 50.0 * math.cos(az)
      -- 0 = linear shape, so REAPER interpolates between our points exactly
      -- the way Studio interpolates between keyframes.
      reaper.InsertEnvelopePoint(xenv, t, (x + 50.0) / 100.0, 0, 0, false, true)
      reaper.InsertEnvelopePoint(yenv, t, (y + 50.0) / 100.0, 0, 0, false, true)
    end
    -- Hold the end position for the rest of the take.
    reaper.InsertEnvelopePoint(xenv, 2.0, (-50.0 + 50.0) / 100.0, 0, 0, false, true)
    reaper.InsertEnvelopePoint(yenv, 2.0, (0.0 + 50.0) / 100.0, 0, 0, false, true)
    reaper.Envelope_SortPoints(xenv)
    reaper.Envelope_SortPoints(yenv)
    log("envelopes written: " .. (POINTS + 2) .. " points each over " ..
        SWEEP_SECONDS .. " s")
  else
    log("ENVELOPE CREATION FAILED")
  end
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
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "sweep-monitor", true)
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
      log("start X=" .. xv .. " Y=" .. yv .. " Z=" .. zv)
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
      log("fstudio exists=" ..
          tostring(reaper.file_exists(OUT_DIR .. "/bridge_sweep.fstudio")))
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/sweepgate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(step)
end
reaper.defer(step)
