-- FRIDAY Bridge — B1-Linux audio gate (architecture-correct).
--
--   track "renderer" : Eclipsa Audio Renderer   (ZMQ SUB, binds tcp://localhost:5555)
--   track "source"   : test WAV + Eclipsa Audio Element Plugin (ZMQ PUB), routed
--                      to the renderer track
--
-- Both plugins get an authored state chunk so an Audio Element exists and the
-- panner is assigned to it — that assignment is normally made in the GUI, and
-- without it the panner's firstOutputChannel stays -1 and nothing is produced.
--
-- Quits via Main_SaveProjectEx so the "save changes?" modal never appears.

local OUT_DIR = "/data/build/b1-gate"
local SRC = OUT_DIR .. "/test-714.wav"
local OUTNAME = "gate"
local LOG = OUT_DIR .. "/" .. OUTNAME .. "-result.txt"

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end
local function slurp(p)
  local f = io.open(p, "r"); if not f then return nil end
  local s = f:read("*a"); f:close(); return (s:gsub("%s+$", ""))
end
local function wait(sec)
  local t0 = reaper.time_precise()
  while reaper.time_precise() - t0 < sec do end
end

log("=== B1-Linux audio gate ===")

-- 1. Renderer track first: it binds the ZMQ port.
reaper.InsertTrackAtIndex(0, true)
local rtrack = reaper.GetTrack(0, 0)
reaper.GetSetMediaTrackInfo_String(rtrack, "P_NAME", "renderer", true)
reaper.SetMediaTrackInfo_Value(rtrack, "I_NCHAN", 12)
local rfx = reaper.TrackFX_AddByName(rtrack, "Eclipsa Audio Renderer", false, 1)
log("renderer fx=" .. tostring(rfx))
local rchunk = slurp(OUT_DIR .. "/renderer-mod.b64")
if rfx >= 0 and rchunk then
  local ok = reaper.TrackFX_SetNamedConfigParm(rtrack, rfx, "vst_chunk", rchunk)
  log("renderer state applied=" .. tostring(ok) .. " len=" .. #rchunk)
end
wait(2.0)

-- 2. Source track with the panner, routed into the renderer track.
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
local pchunk = slurp(OUT_DIR .. "/panner-mod.b64")
if sfx >= 0 and pchunk then
  local ok = reaper.TrackFX_SetNamedConfigParm(strack, sfx, "vst_chunk", pchunk)
  log("panner state applied=" .. tostring(ok) .. " len=" .. #pchunk)
end

-- Route source -> renderer, 12 channels, and keep source out of the master so
-- the render can only contain what the renderer produced.
reaper.SetMediaTrackInfo_Value(strack, "B_MAINSEND", 0)
local send = reaper.CreateTrackSend(strack, rtrack)
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_SRCCHAN", 0 | (4 << 10)) -- ch1-12
reaper.SetTrackSendInfo_Value(strack, 0, send, "I_DSTCHAN", 0)
log("send created idx=" .. tostring(send))

wait(3.0)

-- 3. Render the master.
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)
reaper.GetSetProjectInfo(0, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(0, "RENDER_BOUNDSFLAG", 0, true)
reaper.GetSet_LoopTimeRange(true, false, 0.0, 2.0, false)
reaper.GetSetProjectInfo(0, "RENDER_STARTPOS", 0.0, true)
reaper.GetSetProjectInfo(0, "RENDER_ENDPOS", 2.0, true)
reaper.GetSetProjectInfo(0, "RENDER_CHANNELS", 12, true)
reaper.GetSetProjectInfo(0, "RENDER_SRATE", 48000, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FILE", OUT_DIR, true)
reaper.GetSetProjectInfo_String(0, "RENDER_PATTERN", OUTNAME, true)
reaper.GetSetProjectInfo_String(0, "RENDER_FORMAT", "ZXZhdxgAAQ==", true)
reaper.Main_OnCommand(41824, 0)
log("rendered; proj len=" .. reaper.GetProjectLength(0))

-- 4. Quit with no modal: save to a scratch project first.
reaper.Main_SaveProjectEx(0, OUT_DIR .. "/gate.rpp", 0)
reaper.Main_OnCommand(40004, 0)
