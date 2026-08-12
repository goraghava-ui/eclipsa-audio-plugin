-- FRIDAY Bridge — B4 / V2-02 gate: live pan -> Studio, and a .fstudio handoff.
--
--   track "renderer" : Eclipsa Audio Renderer  (holds the StudioLink)
--   track "source"   : stem + Eclipsa Audio Element Plugin (publishes objects)
--
-- Transport is rolling for the whole test so the panner's processBlock runs
-- continuously and the live pings keep flowing; with REAPER stopped the FX
-- graph may idle and there would be nothing to measure.
--
-- Timestamps come from `date +%s%N` via io.popen: ReaScript's time_precise()
-- is monotonic-since-launch and cannot be compared with Python's time.time().
-- The popen happens BEFORE the pan is issued, so its cost lands inside the
-- measured interval and the reported latency is an UPPER bound.

local OUT_DIR = "/data/build/b4"
local B2_DIR = "/data/build/b2"
local SRC = B2_DIR .. "/stem714.wav"
local LOG = OUT_DIR .. "/b4-gate-result.txt"
local PANS = OUT_DIR .. "/pans.json"
local IAMF = OUT_DIR .. "/bridge_b4.iamf"

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end
local function now_ns()
  local p = io.popen("date +%s%N")
  local s = p:read("*a"); p:close()
  return (s:gsub("%s+$", ""))
end

-- Calibrate REAPER's monotonic clock against the wall clock ONCE, bracketed,
-- so the per-pan timestamp costs nothing. Doing io.popen per pan put the fork
-- of a multi-hundred-MB process inside every measurement and inflated it by
-- more than the gate itself.
local clockOffset, clockUncertainty
local function calibrateClock(samples)
  local best
  for _ = 1, samples do
    local a = reaper.time_precise()
    local wall = tonumber(now_ns()) / 1e9
    local b = reaper.time_precise()
    local halfWidth = (b - a) / 2
    if best == nil or halfWidth < best.u then
      best = {o = wall - (a + b) / 2, u = halfWidth}
    end
  end
  clockOffset, clockUncertainty = best.o, best.u
end
local function wallNow()
  return reaper.time_precise() + clockOffset
end

for _, p in ipairs({IAMF, OUT_DIR .. "/b4gate.rpp", OUT_DIR .. "/b4-monitor.wav",
                    OUT_DIR .. "/bridge_b4.fstudio", PANS,
                    OUT_DIR .. "/harness-done"}) do
  os.remove(p)
end
-- the per-audio-element WAV name is derived from the element name
os.execute("rm -f '" .. OUT_DIR .. "'/bridge_b4.iamf_*.wav")

log("=== B4 gate: live link + handoff ===")
local okMode, mode = reaper.GetAudioDeviceInfo("MODE")
local okBs, bsize = reaper.GetAudioDeviceInfo("BSIZE")
local okSr, srate = reaper.GetAudioDeviceInfo("SRATE")
log("audio device running=" .. tostring(reaper.Audio_IsRunning()) ..
    " mode=" .. tostring(okMode and mode or "?") ..
    " bsize=" .. tostring(okBs and bsize or "?") ..
    " srate=" .. tostring(okSr and srate or "?"))
calibrateClock(9)
log(string.format("clock calibrated: uncertainty +/- %.3f ms",
                  clockUncertainty * 1000))

reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
local rchunk = slurp(OUT_DIR .. "/renderer-b4.b64")
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
local pchunk = slurp(OUT_DIR .. "/panner-b4.b64")
if sfx >= 0 and pchunk then
  log("panner state applied=" ..
      tostring(reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)))
end

reaper.SetMediaTrackInfo_Value(strack, "B_MAINSEND", 0)
local send = reaper.CreateTrackSend(strack, rtrack)
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_SRCCHAN", 0 | (4 << 10))
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_DSTCHAN", 0)
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)

-- X/Y over [-50,+50] normalised to 0..1; az = -atan2(x/50, y/50).
-- Distinct, well-separated targets so each pan message pairs unambiguously.
local STEPS = {
  {name = "az-45", x = 0.1464, y = 0.8536},
  {name = "az+00", x = 0.5000, y = 1.0000},
  {name = "az+60", x = 0.1340, y = 0.7500},
  {name = "az-20", x = 0.6710, y = 0.9698},
  {name = "az+90", x = 0.0000, y = 0.5000},
  {name = "az+15", x = 0.3706, y = 0.9830},
}

local pans = {}
local step = 0
local phase = "settle"
local tick = 0

reaper.GetSet_LoopTimeRange(true, false, 0.0, 20.0, false)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUT_DIR, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", "b4-monitor", true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true)

reaper.OnPlayButton()
log("transport rolling, playstate=" .. tostring(reaper.GetPlayState()))

local function writePans()
  local parts = {}
  for _, p in ipairs(pans) do
    parts[#parts + 1] = string.format(
      '{"name":"%s","t":%.6f,"x":%.6f,"y":%.6f}', p.name, p.t, p.x, p.y)
  end
  local f = io.open(PANS, "w")
  f:write(string.format('{"clock_uncertainty_ms":%.4f,"pans":[', 
                        clockUncertainty * 1000))
  f:write(table.concat(parts, ",\n ") .. "]}\n")
  f:close()
end

local function stepFn()
  tick = tick + 1
  if phase == "settle" then
    -- Give the link its connect + hello + scene before timing anything.
    if tick > 90 then phase = "pan"; tick = 0 end
  elseif phase == "pan" then
    if tick % 12 == 1 then
      step = step + 1
      if step > #STEPS then
        phase = "export"; tick = 0
        writePans()
        log("pans issued=" .. #pans)
      else
        local s = STEPS[step]
        local t = wallNow()
        reaper.TrackFX_SetParam(strack, sfx, 2, s.x)
        reaper.TrackFX_SetParam(strack, sfx, 3, s.y)
        reaper.TrackFX_SetParam(strack, sfx, 4, 0.5)
        pans[#pans + 1] = {name = s.name, t = t, x = s.x, y = s.y}
      end
    end
  elseif phase == "export" then
    if tick > 15 then
      reaper.OnStopButton()
      reaper.Main_OnCommand(41824, 0)   -- bounce -> arms the KALA export
      log("bounce issued")
      phase = "finalise"; tick = 0
    end
  elseif phase == "finalise" then
    if tick == 30 then
      reaper.OnPlayButton()             -- back to realtime -> finalise
      log("play issued")
    elseif tick == 90 then
      reaper.OnStopButton()
    elseif tick == 200 then
      -- Track removal must empty the live scene: the tap announces its own
      -- departure from its destructor, so the scope cannot keep a ghost orb.
      reaper.DeleteTrack(strack)
      log("source track deleted")
    elseif tick > 260 then
      log("iamf exists=" .. tostring(reaper.file_exists(IAMF)))
      log("fstudio exists=" ..
          tostring(reaper.file_exists(OUT_DIR .. "/bridge_b4.fstudio")))
      local d = io.open(OUT_DIR .. "/harness-done", "w"); d:write("done\n"); d:close()
      reaper.Main_SaveProjectEx(0, OUT_DIR .. "/b4gate.rpp", 0)
      reaper.Main_OnCommand(40004, 0)
      return
    end
  end
  reaper.defer(stepFn)
end

reaper.defer(stepFn)
