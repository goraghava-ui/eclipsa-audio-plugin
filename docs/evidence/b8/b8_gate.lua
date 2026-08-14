-- FRIDAY Bridge — B8 probe: a BELOW-HORIZON object, end to end.
--
-- Not a parity gate. Eclipsa's tent/arch/curve surfaces can put an object
-- under the listening plane; Studio's constraint surfaces are clamped to
-- [0, 90] and its scope has no below-horizon representation. This asks what
-- actually happens to such an object when the Bridge hands it over — through
-- BOTH routes, the live NDJSON link and the .fstudio handoff written beside
-- the exported .iamf.
--
-- Pose: elevation mode "tent", front rim.
--
--   X = 0, Y = 50  ->  tent height = 1 - 2*|1| = -1  ->  Z = -50
--   elevation = atan2(-50, 50) = -45 deg
--
-- Z is NOT written here. X and Y arrive with the plugin state and
-- ElevationListener derives the height from the tent surface, which is
-- untouched by B7/B8 — that is the point, this probes the handoff, not the
-- surface. The "captured" line in the log is the position that was really
-- sent.
--
-- Transport rolls so the panner's processBlock runs and the live link gets its
-- hello + scene out before the export.

local OUT_DIR = "/data/build/b8"
local SRC = OUT_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/b8-gate-result.txt"
local IAMF = OUT_DIR .. "/bridge_tent.iamf"


local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end

for _, p in ipairs({IAMF, OUT_DIR .. "/b8gate.rpp", OUT_DIR .. "/b8-monitor.wav",
                    OUT_DIR .. "/bridge_tent.fstudio"}) do
  os.remove(p)
end
os.execute("rm -f '" .. OUT_DIR .. "'/bridge_tent.iamf_*.wav '" ..
           OUT_DIR .. "'/bridge_b_*.wav")

log("=== B8 probe: below-horizon object, az 0 el -45, elevation=tent ===")
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?"))

reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
local rchunk = slurp(OUT_DIR .. "/renderer-tent.b64")
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
local pchunk = slurp(OUT_DIR .. "/panner-tent.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

-- X and Y arrive with the state (X = 0, Y = 50, Z = 0). Nothing is written
-- here on purpose: the height has to come from the tent surface, so the -45
-- this probe hands over is one the plugin really produced.
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
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "b8-monitor", true)
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
      log("fstudio exists=" ..
          tostring(reaper.file_exists(OUT_DIR .. "/bridge_tent.fstudio")))
      local f = io.open(OUT_DIR .. "/harness-done", "w")
      if f then f:write("done\n"); f:close() end
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/b8gate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(step)
end
reaper.defer(step)
