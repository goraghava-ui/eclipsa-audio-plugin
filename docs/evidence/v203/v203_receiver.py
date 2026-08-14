#!/usr/bin/env python3
"""V2-03 gate receiver — a real Studio-side BridgeAudioFeed.

Uses friday-studio's own studio.bridge_audio.BridgeAudioFeed, so the thing
under test is the actual contract rather than a re-implementation of it. Pulls
blocks the way a monitor callback would, writes the captured stream to a WAV,
and reports the feed's own counters.

Runs until the marker file appears (the REAPER harness writes it as it quits)
or the timeout expires.
"""
import json
import os
import sys
import time

import numpy as np
import soundfile as sf

sys.path.insert(0, "/data/projects/friday/friday-studio")
from studio.bridge_audio import BridgeAudioFeed  # noqa: E402

OUT_DIR = "/data/build/v203"
WAV = os.path.join(OUT_DIR, "feed-capture.wav")
REPORT = os.path.join(OUT_DIR, "feed-report.json")
DONE = os.path.join(OUT_DIR, "harness-done")
TIMEOUT_S = float(os.environ.get("V203_TIMEOUT", "240"))

state = {"hello": None, "start": None, "end": None}


def on_state(hello):
    if hello is not None:
        state["hello"] = hello
        state["start"] = time.time()
        print(f"stream up: {hello}", flush=True)
    else:
        if state["start"] is not None and state["end"] is None:
            state["end"] = time.time()
        print("stream down", flush=True)


feed = BridgeAudioFeed(on_state)
print(f"listening on 127.0.0.1:{feed.port}", flush=True)

blocks = []
deadline = time.time() + TIMEOUT_S
try:
    while time.time() < deadline and not os.path.exists(DONE):
        frame = feed.read_block()
        if frame is None:
            # Nothing buffered. A monitor callback would emit silence here;
            # the feed has already counted the underrun.
            time.sleep(0.0005)
            continue
        blocks.append(frame.copy())
finally:
    if state["start"] is not None and state["end"] is None:
        state["end"] = time.time()
    counters = {
        "dropped_frames": feed.dropped_frames,
        "sequence_gaps": feed.sequence_gaps,
        "underruns": feed.underruns,
    }
    feed.close()

hello = state["hello"] or {}
block = int(hello.get("block", 0)) or 512
channels = int(hello.get("channels", 0)) or 12
sr = int(hello.get("sample_rate", 0)) or 48000

audio = (np.concatenate(blocks, axis=0) if blocks
         else np.zeros((0, channels), dtype=np.float32))
if audio.size:
    sf.write(WAV, audio, sr, subtype="FLOAT")

wall = ((state["end"] or time.time()) - state["start"]) if state["start"] else 0.0
frames_received = len(blocks)
# What a gapless stream would have delivered over the same wall clock. A block
# the Bridge dropped never gets a sequence number -- it is dropped BEFORE the
# frame is built -- so it cannot show up as a sequence gap. This is the number
# that would notice it.
frames_expected = int(round(wall * sr / block)) if wall > 0 else 0

report = {
    "hello": hello,
    "stream_seconds": round(wall, 3),
    "frames_received": frames_received,
    "frames_expected_for_wall_clock": frames_expected,
    "frames_missing": max(0, frames_expected - frames_received),
    "audio_seconds": round(audio.shape[0] / sr, 3) if audio.size else 0.0,
    "counters": counters,
    "wav": WAV if audio.size else None,
}
with open(REPORT, "w") as f:
    json.dump(report, f, indent=1)
print(json.dumps(report, indent=1), flush=True)
