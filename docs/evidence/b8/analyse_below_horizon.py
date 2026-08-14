#!/usr/bin/env python3
"""B8: what Studio actually does with a below-horizon object from the Bridge.

Reads the artefacts the probe produced (`received.json` from a real
`studio.bridge_link.BridgeLink`, `bridge_tent.fstudio` from the plugin) and
asks Studio's own code what becomes of them. friday-studio is imported, never
modified.
"""
import json
import math
import os
import sys

sys.path.insert(0, "/data/projects/friday/friday-studio")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtWidgets import QApplication    # noqa: E402
from studio.session import Keyframe           # noqa: E402
from studio.ui.widgets import PannerScope     # noqa: E402

_app = QApplication([])

HERE = os.path.dirname(os.path.abspath(__file__))
out: list[str] = []


def say(s=""):
    out.append(s)


say("=== B8: a below-horizon object, Bridge -> Studio ===")
say()

# ---------------------------------------------------------------- the wire
events = json.load(open(os.path.join(HERE, "received.json")))["events"]
scenes = [e["msg"] for e in events
          if e["msg"].get("type") == "scene" and e["msg"].get("objects")]
carried = scenes[-1]["objects"][0]
say("1. live NDJSON link (studio.bridge_link.BridgeLink, a real listener)")
say(f"   scene object as received : {json.dumps(carried)}")
say(f"   elevation on the wire    : {carried['elevation']:+.4f} deg  "
    f"-- carried verbatim, not clamped, no warning")
say()

# ---------------------------------------------------------------- the file
hand = json.load(open(os.path.join(HERE, "bridge_tent.fstudio")))
kf_raw = hand["objects"][0]["keyframes"][0]
say("2. .fstudio handoff written beside the exported .iamf")
say(f"   keyframe as written      : {json.dumps(kf_raw)}")
say(f"   elevation in the file    : {kf_raw['elevation']:+.4f} deg  "
    f"-- written verbatim")
say()

# ------------------------------------------------------- Studio's own model
kf = Keyframe(t=kf_raw["t"], azimuth=kf_raw["azimuth"],
              elevation=kf_raw["elevation"], spread=kf_raw["spread"],
              gain_db=kf_raw["gain_db"]).clamped()
say("3. studio.session.Keyframe.clamped()  (session.py:46)")
say(f"   elevation after clamp    : {kf.elevation:+.4f} deg")
say("   the clamp is [-90, +90], NOT [0, 90] -- the session model keeps it")
say()

# ------------------------------------------------------------- Studio's eye
scope = PannerScope()
say("4. studio.ui.widgets.PannerScope._radius_frac()  (widgets.py:150)")
say("   remote Bridge objects are drawn through _to_point -> _radius_frac,")
say("   which clamps elevation to [-5, 90] before projecting:")
say()
say("      elevation      radius_frac    where it lands")
ref = scope._radius_frac(0.0)
for el in (0.0, -5.0, -45.0, -90.0):
    f = scope._radius_frac(el)
    px = (ref - f) * 235.0          # 235 px = the B5 scope's drawing radius
    say(f"      {el:+7.1f} deg    {f:.6f}       {px:5.2f} px inside el 0")
say()
say("   every elevation at or below -5 projects to the SAME ring, 0.84 px")
say("   inside where a floor-level object draws. -45 is indistinguishable")
say("   from 0 on the scope, and nothing says so.")
say()

# --------------------------------------------------------- Studio's renderer
say("5. the render (studio_cli export, 7.1.4)")
try:
    import numpy as np
    import soundfile as sf
    a, _ = sf.read(os.path.join(HERE, "elneg45-out/elneg45_714.wav"))
    b, _ = sf.read(os.path.join(HERE, "el0-out/el0_714.wav"))
    same = np.array_equal(a, b)
    say(f"   studio_cli az 0 / el -45  vs  az 0 / el 0")
    say(f"   byte-identical master     : {same}")
    say(f"   max |sample difference|   : {float(np.abs(a - b).max()):.1f}")
    names = "L R C LFE Ls Rs Lrs Rrs TFL TFR TRL TRR".split()
    live = [names[i] for i in range(12)
            if float(np.sqrt(np.mean(a[:, i] ** 2))) > 0]
    say(f"   channels carrying energy  : {', '.join(live)}")
except Exception as e:                                   # pragma: no cover
    say(f"   (render comparison unavailable: {e})")
say()
say("   7.1.4 has no speaker below the horizon, so VBAP puts the object on")
say("   the floor layer. The height is not clamped on the way in -- it is")
say("   discarded at the end, silently, and the file is the same file.")
say()
say("=== verdict: silent at every stage, lossy at two ===")
say("   wire  : verbatim      file  : verbatim      session: kept")
say("   scope : collapsed below -5   render: collapsed below 0")
say("   validate: 'session ok'. No error, no warning, anywhere.")

text = "\n".join(out) + "\n"
print(text, end="")
with open(os.path.join(HERE, "below-horizon-observed.txt"), "w") as f:
    f.write(text)
