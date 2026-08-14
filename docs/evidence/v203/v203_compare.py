#!/usr/bin/env python3
"""V2-03 gate comparison: the live feed against the plugin's exported master.

The feed carries Eclipsa's MONITORING render of the bed. The exported .iamf is
KALA's render of the captured objects, loudness-normalised to the session's
target LKFS. Two different renderers and one of them has a mastering gain, so
the honest comparison is:

  * channel mapping   -- which channels carry energy, asserted exactly
  * channel balance   -- per-channel level RELATIVE to the loudest channel,
                         which the mastering gain cannot move, asserted to
                         0.5 dB
  * absolute level    -- reported with the gain between them stated, not
                         asserted, because normalising is what the export is
                         supposed to do

Tolerance is 0.5 dB: this is a monitor feed, not a null test.
"""
import json
import os
import subprocess
import sys

import numpy as np
import soundfile as sf

sys.path.insert(0, "/data/projects/friday/friday-bridge/docs/evidence/b2")
from null_harness import decode_714  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
NAMES = "L R C LFE Ls Rs Lrs Rrs TFL TFR TRL TRR".split()
TOL_DB = 0.5
WINDOW_S = 1.0
SR = 48000


def db(x):
    return 20.0 * np.log10(x) if x > 0 else float("-inf")


def rms_per_channel(block):
    return np.array([float(np.sqrt(np.mean(block[:, c] ** 2)))
                     for c in range(block.shape[1])])


def first_tone_window(audio, sr, seconds):
    """The first `seconds` starting where the tone actually begins. The capture
    brackets the transport with silence (offline bounce, pre-roll, post-stop),
    and averaging that in would understate every channel equally but pointlessly.
    """
    energy = np.abs(audio).max(axis=1)
    live = np.flatnonzero(energy > 1e-4)
    if live.size == 0:
        raise SystemExit("the capture is silent -- nothing to compare")
    start = int(live[0])
    end = min(start + int(seconds * sr), audio.shape[0])
    return audio[start:end], start


out = []


def say(s=""):
    out.append(s)
    print(s, flush=True)


feed, sr = sf.read(os.path.join(HERE, "feed-capture.wav"))
if sr != SR:
    raise SystemExit(f"feed sample rate {sr}, expected {SR}")
feed_win, feed_start = first_tone_window(feed, sr, WINDOW_S)

master = decode_714(os.path.join(HERE, "bridge_v203.iamf"))
master_win, master_start = first_tone_window(master, SR, WINDOW_S)

feed_rms = rms_per_channel(feed_win)
master_rms = rms_per_channel(master_win)

say("=== V2-03: live feed vs the plugin's exported master ===")
say()
say(f"feed   : {feed.shape[0]} frames, {feed.shape[1]} ch, tone from "
    f"{feed_start / SR:.3f} s")
say(f"master : {master.shape[0]} frames, {master.shape[1]} ch, tone from "
    f"{master_start / SR:.3f} s")
say(f"window : {WINDOW_S:.1f} s")
say()

# -- channel mapping ---------------------------------------------------------
feed_live = {NAMES[c] for c in range(12) if feed_rms[c] > 0}
master_live = {NAMES[c] for c in range(12) if master_rms[c] > 0}
mapping_ok = feed_live == master_live
say(f"channels with energy, feed   : {sorted(feed_live) or '(none)'}")
say(f"channels with energy, master : {sorted(master_live) or '(none)'}")
say(f"CHANNEL MAPPING: {'MATCH' if mapping_ok else 'MISMATCH'}")
say()

# -- balance, which the mastering gain cannot move ---------------------------
feed_ref = feed_rms.max()
master_ref = master_rms.max()
gain_db = db(master_ref) - db(feed_ref)

say(f"{'ch':<5}{'feed dBFS':>12}{'master dBFS':>13}"
    f"{'feed rel':>10}{'master rel':>12}{'delta':>9}")
worst = 0.0
worst_ch = "-"
for c in range(12):
    f_abs, m_abs = db(feed_rms[c]), db(master_rms[c])
    f_rel = db(feed_rms[c] / feed_ref) if feed_rms[c] > 0 else float("-inf")
    m_rel = db(master_rms[c] / master_ref) if master_rms[c] > 0 else float("-inf")
    if np.isfinite(f_rel) and np.isfinite(m_rel):
        d = abs(f_rel - m_rel)
        if d > worst:
            worst, worst_ch = d, NAMES[c]
        ds = f"{d:8.3f}"
    else:
        ds = "        -" if not (np.isfinite(f_rel) or np.isfinite(m_rel)) \
            else "     FAIL"
    say(f"{NAMES[c]:<5}{f_abs:12.2f}{m_abs:13.2f}{f_rel:10.2f}{m_rel:12.2f}{ds}")
say()
say(f"mastering gain between them : {gain_db:+.2f} dB  "
    f"(the export normalises, the monitor feed does not)")
say(f"worst channel balance delta : {worst:.3f} dB on {worst_ch}")
say(f"BALANCE (tol {TOL_DB} dB): {'PASS' if worst <= TOL_DB else 'FAIL'}")
say()

# -- spectrum ----------------------------------------------------------------
def peak_bin_hz(x, sr):
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w))
    return float(np.fft.rfftfreq(len(x), 1.0 / sr)[int(np.argmax(spec))])


loud = int(np.argmax(feed_rms))
f_hz = peak_bin_hz(feed_win[:, loud], SR)
m_hz = peak_bin_hz(master_win[:, loud], SR)
say(f"spectrum on {NAMES[loud]} : feed peak {f_hz:.1f} Hz, "
    f"master peak {m_hz:.1f} Hz  (stem is 997 Hz)")
spectrum_ok = abs(f_hz - m_hz) < 2.0 and abs(f_hz - 997.0) < 2.0
say(f"SPECTRUM: {'MATCH' if spectrum_ok else 'MISMATCH'}")
say()

report = json.load(open(os.path.join(HERE, "feed-report.json")))
c = report["counters"]
say("=== stream health over the run ===")
say(f"stream seconds        : {report['stream_seconds']}")
say(f"frames received       : {report['frames_received']}")
say(f"sequence gaps         : {c['sequence_gaps']}")
say(f"frames missing        : {report['frames_missing']}   "
    f"(a Bridge-side drop never gets a sequence number, so this is what "
    f"would notice one)")
say(f"Studio ring overflows : {c['dropped_frames']}")
say(f"Studio underruns      : {c['underruns']}   "
    f"(the puller outrunning the stream, not a loss)")
say()

ok = mapping_ok and worst <= TOL_DB and spectrum_ok and \
    c["sequence_gaps"] == 0 and report["frames_missing"] == 0
say(f"GATE: {'PASS' if ok else 'FAIL'}")

with open(os.path.join(HERE, "v203-compare.txt"), "w") as f:
    f.write("\n".join(out) + "\n")
sys.exit(0 if ok else 1)
