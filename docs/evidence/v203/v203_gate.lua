-- FRIDAY Bridge — V2-03 gate: the renderer's bed, live to Studio.
--
-- Same 997 Hz stem and same pose as every gate since B2 (az +30.000, el 0,
-- elevation mode "none"), so the feed can be checked against numbers that are
-- already on the record.
--
-- The transport LOOPS a 2 s region for a minute, because the point of this
-- gate is not one block -- it is whether a continuous stream survives a minute
-- on localhost without a gap. The receiver (v203_receiver.py, driving Studio's
-- own BridgeAudioFeed) counts what arrives.
--
--   x = -50 * sin(30) * cos(0) = -25.00000
--   y =  50 * cos(30) * cos(0) =  43.30127
-- normalised over [-50, +50]: (v + 50) / 100.

local OUT_DIR = "/data/build/v203"
local SRC = OUT_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/v203-gate-result.txt"
local IAMF = OUT_DIR .. "/bridge_v203.iamf"

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

for _, p in ipairs({IAMF, OUT_DIR .. "/v203gate.rpp", OUT_DIR .. "/v203-monitor.wav",
                    OUT_DIR .. "/bridge_v203.fstudio"}) do
  os.remove(p)
end
os.execute("rm -f '" .. OUT_DIR .. "'/bridge_v203.iamf_*.wav '" ..
           OUT_DIR .. "'/bridge_v203_*.wav")

log("=== V2-03 gate: live bed feed, az +30.000 el 0 ===")
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?"))

reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
local rchunk = slurp(OUT_DIR .. "/renderer-v203.b64")
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
local pchunk = slurp(OUT_DIR .. "/panner-v203.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

if sfx >= 0 then
  reaper.TrackFX_SetParam(strack, sfx, 4, Z_NORM)
  reaper.TrackFX_SetParam(strack, sfx, 2, X_NORM)
  reaper.TrackFX_SetParam(strack, sfx, 3, Y_NORM)
  local _, xv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 2, "")
  local _, yv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 3, "")
  log("requested X=" .. xv .. " Y=" .. yv)
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
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "v203-monitor", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true)

local STREAM_SECONDS = 62.0

local tick = 0
local playStarted = 0
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
      -- Loop the 2 s region so the tone is present for the whole minute; a
      -- silent stream would exercise the transport but not the comparison.
      reaper.GetSetRepeat(1)
      reaper.OnPlayButton()
      playStarted = reaper.time_precise()
      log("transport rolling, looping 2 s for " .. STREAM_SECONDS .. " s")
      stage = "stream"; tick = 0
    end
  elseif stage == "stream" then
    if reaper.time_precise() - playStarted >= STREAM_SECONDS then
      reaper.OnStopButton()
      log("streamed " ..
          string.format("%.2f", reaper.time_precise() - playStarted) .. " s")
      stage = "finish"; tick = 0
    end
  elseif stage == "finish" then
    if tick > 150 then
      log("iamf exists=" .. tostring(reaper.file_exists(IAMF)))
      local f = io.open(OUT_DIR .. "/harness-done", "w")
      if f then f:write("done\n"); f:close() end
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/v203gate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(step)
end
reaper.defer(step)
