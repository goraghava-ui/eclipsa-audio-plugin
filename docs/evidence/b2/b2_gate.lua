-- FRIDAY Bridge — B2 gate: headless Bridge export through the KALA path.
--
--   track "renderer" : Eclipsa Audio Renderer, state armed to export .iamf
--   track "source"   : 997 Hz stem + Eclipsa Audio Element Plugin at az +30
--
-- FileOutputProcessor arms on setNonRealtime(true) (REAPER's offline bounce)
-- and FINALISES on setNonRealtime(false) — which REAPER only issues when it
-- next goes back to realtime. So this drives play/stop after the bounce.
--
-- Everything is sequenced with reaper.defer rather than a busy-wait: a spin
-- loop blocks REAPER's message loop, so the transport never actually moves and
-- the export is never finalised. That was the first version's bug.

local OUT_DIR = "/data/build/b2"
local SRC = OUT_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/b2-gate-result.txt"
local IAMF = OUT_DIR .. "/bridge.iamf"

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end

log("=== B2 gate: Bridge export via KALA ===")

-- REAPER silently SKIPS a render whose output file already exists, and it
-- pops a modal "overwrite?" when saving over an existing .rpp. Both stall a
-- headless run, so every artefact this script produces is removed up front.
for _, p in ipairs({IAMF, OUT_DIR .. "/b2gate.rpp", OUT_DIR .. "/b2-monitor.wav",
                    OUT_DIR .. "/bridge.iamf_AE1.wav"}) do
  os.remove(p)
end

-- A working audio device is what makes REAPER leave offline mode after the
-- bounce, which is what calls setNonRealtime(false) -> closeFileExport.
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
local okSr, srate = reaper.GetAudioDeviceInfo("SRATE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?") ..
    " srate=" .. tostring(okSr and srate or "?"))

-- Renderer first so it binds its ports before any panner connects.
reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
log("renderer fx=" .. tostring(rfx))
local rchunk = slurp(OUT_DIR .. "/renderer-b2.b64")
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
log("source items=" .. reaper.CountTrackMediaItems(strack))

local sfx = reaper.TrackFX_AddByName(strack, "Eclipsa Audio Element Plugin", false, 1)
log("panner fx=" .. tostring(sfx))
local pchunk = slurp(OUT_DIR .. "/panner-b2.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

-- az +30 el 0 — X/Y/Z are normalised over [-50, +50] (ParameterMetaData.h:34)
-- and AudioPanner reads az = -atan2(x, y), so az +30 is x -25, y +43.3, z 0.
if sfx >= 0 then
  reaper.TrackFX_SetParam(strack, sfx, 2, 0.25)
  reaper.TrackFX_SetParam(strack, sfx, 3, 0.933)
  reaper.TrackFX_SetParam(strack, sfx, 4, 0.5)
  local _, xv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 2, "")
  local _, yv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 3, "")
  local _, zv = reaper.TrackFX_GetFormattedParamValue(strack, sfx, 4, "")
  log("object position X=" .. xv .. " Y=" .. yv .. " Z=" .. zv)
end

reaper.SetMediaTrackInfo_Value(strack, "B_MAINSEND", 0)
local send = reaper.CreateTrackSend(strack, rtrack)
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_SRCCHAN", 0 | (4 << 10))
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_DSTCHAN", 0)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)

reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSet_LoopTimeRange(true, false, 0.0, 2.0, false)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUT_DIR, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "b2-monitor", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true)

-- The bounce is issued from the script BODY, not from a defer callback:
-- issuing it from inside defer did not arm FileOutputProcessor at all, while
-- the same call here does (confirmed against the plugin's own log). Only the
-- finalisation afterwards needs the message loop, so only that is deferred.
reaper.Main_OnCommand(41824, 0)
log("bounce issued")

local tick = 0
local stage = "play"

local function step()
  tick = tick + 1
  if stage == "play" then
    if tick > 30 then
      reaper.OnPlayButton()              -- back to realtime -> finalise
      log("play issued, playstate=" .. tostring(reaper.GetPlayState()))
      stage = "stop"
      tick = 0
    end
  elseif stage == "stop" then
    if tick > 60 then
      reaper.OnStopButton()
      log("stop issued, playstate before=" .. tostring(reaper.GetPlayState()) ..
          " pos=" .. string.format("%.3f", reaper.GetPlayPosition()))
      stage = "finish"
      tick = 0
    end
  elseif stage == "finish" then
    if tick > 120 then                   -- let drain + encode complete
      log("bridge.iamf exists=" .. tostring(reaper.file_exists(IAMF)))
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/b2gate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(step)
end

reaper.defer(step)
