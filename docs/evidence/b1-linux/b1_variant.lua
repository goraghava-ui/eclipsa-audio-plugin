-- Parametrised B1 pass-through render.
-- Reads the FX name from /data/build/b1-gate/fxname.txt ("" = no FX control run)
-- and the output basename from /data/build/b1-gate/outname.txt

local OUT_DIR = "/data/build/b1-gate"
local SRC = OUT_DIR .. "/test-714.wav"

local function slurp(p)
  local f = io.open(p, "r"); if not f then return "" end
  local s = f:read("*l") or ""; f:close(); return s
end

local FXNAME = slurp(OUT_DIR .. "/fxname.txt")
local OUTNAME = slurp(OUT_DIR .. "/outname.txt")
local LOG = OUT_DIR .. "/" .. OUTNAME .. "-result.txt"

local lines = {}
local function log(s)
  lines[#lines + 1] = tostring(s)
  local f = io.open(LOG, "w"); f:write(table.concat(lines, "\n") .. "\n"); f:close()
end

log("=== variant: fx='" .. FXNAME .. "' out='" .. OUTNAME .. "' ===")

reaper.InsertTrackAtIndex(0, true)
local track = reaper.GetTrack(0, 0)
reaper.SetMediaTrackInfo_Value(track, "I_NCHAN", 12)
reaper.SetOnlyTrackSelected(track)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(SRC, 0)
log("items=" .. reaper.CountTrackMediaItems(track))

if FXNAME ~= "" and FXNAME ~= "NONE" then
  local fx = reaper.TrackFX_AddByName(track, FXNAME, false, 1)
  log("fx index=" .. tostring(fx))
  if fx >= 0 then
    local _, n = reaper.TrackFX_GetFXName(track, fx, "")
    log("fx name=" .. tostring(n))
    log("in channels=" .. reaper.TrackFX_GetIOSize(track, fx))
  end
else
  log("no FX (control)")
end

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
reaper.SetMediaTrackInfo_Value(reaper.GetMasterTrack(0), "I_NCHAN", 12)

reaper.Main_OnCommand(41824, 0)
log("rendered; proj len=" .. reaper.GetProjectLength(0))
reaper.Main_OnCommand(40004, 0)
