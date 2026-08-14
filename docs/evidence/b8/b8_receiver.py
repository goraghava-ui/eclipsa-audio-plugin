#!/usr/bin/env python3
"""B8 probe receiver — a real Studio-side BridgeLink, timestamping every message.

Uses friday-studio's own studio.bridge_link.BridgeLink so the thing under test
is the actual contract, not a re-implementation of it. Records the wall clock
at which each message reaches the on_message callback; the REAPER harness
records the wall clock at which it issued each pan. The difference is the
gate's "pan in REAPER -> Studio scope" latency.

Runs until the marker file appears (the harness writes it as it quits) or the
timeout expires, then dumps everything as JSON.
"""
import json
import os
import sys
import time

sys.path.insert(0, "/data/projects/friday/friday-studio")
from studio.bridge_link import BridgeLink, apply_to_remote  # noqa: E402

OUT = "/data/build/b8/received.json"
DONE = "/data/build/b8/harness-done"
TIMEOUT_S = float(os.environ.get("B8_TIMEOUT", "180"))

events = []
remote = []


def on_message(msg):
    # time.time() first: everything after this is measurement overhead.
    t = time.time()
    global remote
    remote = apply_to_remote(remote, msg)
    events.append({"t": t, "msg": msg, "remote": [dict(o) for o in remote]})


def on_state(connected):
    events.append({"t": time.time(),
                   "msg": {"type": "_state", "connected": bool(connected)}})


link = BridgeLink(on_message, on_state)
print(f"listening on 127.0.0.1:{link.port}", flush=True)

deadline = time.time() + TIMEOUT_S
try:
    while time.time() < deadline and not os.path.exists(DONE):
        time.sleep(0.05)
finally:
    link.close()

with open(OUT, "w") as f:
    json.dump({"events": events, "final_remote": remote}, f, indent=1)
print(f"{len(events)} events -> {OUT}", flush=True)
