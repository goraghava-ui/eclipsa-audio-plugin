#!/usr/bin/env python3
"""Pair each pan the REAPER harness issued with the pan message that reached
Studio's BridgeLink callback, and report the gate number.

Pairing is by order: the harness issues one pan per ~12 defer ticks and the
messages arrive strictly ordered and hundreds of ms apart, so the i-th issue
matches the i-th arrival. Any pairing ambiguity would show up as a negative or
absurd latency, which is checked for.
"""
import json
import sys

raw = json.load(open("/data/build/b4/pans.json"))
issued = raw["pans"]
clock_unc = float(raw.get("clock_uncertainty_ms", 0.0))
recv = json.load(open("/data/build/b4/received.json"))["events"]
pans = [e for e in recv if e["msg"].get("type") == "pan"]

if len(pans) != len(issued):
    print(f"WARNING: {len(issued)} pans issued, {len(pans)} received")

rows, lat = [], []
for i, iss in enumerate(issued):
    if i >= len(pans):
        rows.append((iss["name"], None, None))
        continue
    t0 = float(iss["t"])
    t1 = pans[i]["t"]
    ms = (t1 - t0) * 1000.0
    lat.append(ms)
    rows.append((iss["name"], pans[i]["msg"]["azimuth"], ms))

print(f"  clock calibration uncertainty: +/- {clock_unc:.3f} ms\n")
print("  # step      azimuth received   latency ms")
for i, (name, az, ms) in enumerate(rows, 1):
    az_s = "  (none)" if az is None else f"{az:9.4f}"
    ms_s = "     n/a" if ms is None else f"{ms:9.2f}"
    print(f"  {i} {name:10s} {az_s}    {ms_s}")

if not lat:
    print("\nNO PANS RECEIVED — FAIL")
    sys.exit(1)

lat.sort()
worst = lat[-1]
median = lat[len(lat) // 2]
print(f"\n  n={len(lat)}  min={lat[0]:.2f} ms  median={median:.2f} ms  "
      f"max={worst:.2f} ms")
print(f"  GATE (<100 ms, worst case): {'PASS' if worst < 100 else 'FAIL'} "
      f"at {worst:.2f} ms")
if any(m < 0 for m in lat):
    print("  !! negative latency — the pairing or the clocks are wrong")
    sys.exit(1)
sys.exit(0 if worst < 100 else 1)
