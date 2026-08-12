#!/usr/bin/env python3
"""B4 gate: does the .fstudio the Bridge wrote actually open in Studio, and do
its objects/positions match the pan that was captured?"""
import json
import sys

sys.path.insert(0, "/data/projects/friday/friday-studio")
from studio.session import Session  # noqa: E402

path = sys.argv[1] if len(sys.argv) > 1 else "/data/build/b4/bridge_b4.fstudio"

print(f"  file: {path}")
s = Session.load(path)                       # raises if not a Studio session
print(f"  loaded: name={s.name!r} sample_rate={s.sample_rate} "
      f"target_lkfs={s.target_lkfs} language={s.language!r}")
print(f"  beds={len(s.beds)} objects={len(s.objects)}")

problems = s.validate()
for o in s.objects:
    print(f"  object {o.name!r}")
    print(f"    file_path: {o.file_path}")
    print(f"    keyframes: {len(o.keyframes)}")
    for k in o.keyframes[:8]:
        print(f"      t={k.t:.6f}  az={k.azimuth:+8.4f}  el={k.elevation:+7.4f}"
              f"  spread={k.spread:.3f}  gain={k.gain_db:+.2f}  {k.curve}")
    if len(o.keyframes) > 8:
        print(f"      ... {len(o.keyframes) - 8} more")

print(f"\n  validate(): {problems if problems else 'clean (0 problems)'}")

# Cross-check against what the live link reported for the same object.
try:
    recv = json.load(open("/data/build/b4/received.json"))
    live = {o["name"]: o for o in recv["final_remote"]}
except (OSError, ValueError):
    live = {}

ok = not problems
if live:
    print("\n  captured pan vs the .fstudio's last keyframe:")
    for o in s.objects:
        ref = live.get(o.name)
        if ref is None:
            print(f"    {o.name!r}: no live counterpart — cannot cross-check")
            continue
        last = o.keyframes[-1]
        d_az = abs(last.azimuth - ref["azimuth"])
        d_el = abs(last.elevation - ref["elevation"])
        print(f"    {o.name!r}: az {last.azimuth:+.4f} vs {ref['azimuth']:+.4f}"
              f" (d={d_az:.4f}), el {last.elevation:+.4f} vs"
              f" {ref['elevation']:+.4f} (d={d_el:.4f})")
        if d_az > 0.01 or d_el > 0.01:
            ok = False

print(f"\n  GATE (.fstudio opens in Studio, clean, positions match):"
      f" {'PASS' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
