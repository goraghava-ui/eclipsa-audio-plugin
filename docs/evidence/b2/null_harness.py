#!/usr/bin/env python3
"""FRIDAY Bridge B2 — IAMF null harness (PRD-v2 gate GA).

Decodes two .iamf files with FFmpeg — an independent IAMF implementation, so
neither side is graded by its own encoder — reassembles both into 12-channel
SMPTE 7.1.4 order, and reports the per-channel maximum |delta| in dBFS.

FFmpeg presents our 7.1.4 element as seven audio streams, one per substream.
Whole-file energy comparison would hide a channel-mapping mistake, so the
substreams are placed back into SMPTE slots explicitly before comparing.

Usage:  null_harness.py <a.iamf> <b.iamf> [--frames N] [--gate-db -90]
Exit:   0 = null within the gate, 1 = FAIL
"""
from __future__ import annotations

import argparse
import math
import subprocess
import sys

import numpy as np

# substream index -> SMPTE channels it carries (app/export/iamf.py)
SUBSTREAM_SMPTE_714 = ((0, 1), (4, 5), (6, 7), (8, 9), (10, 11), (2,), (3,))
CH_NAMES = "L R C LFE Ls Rs Lrs Rrs TFL TFR TRL TRR".split()
N_CH = 12


def decode_substream(path: str, index: int) -> np.ndarray:
    cmd = [
        "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-i", path, "-map", f"0:a:{index}",
        "-f", "f32le", "-c:a", "pcm_f32le", "-",
    ]
    run = subprocess.run(cmd, capture_output=True)
    if run.returncode != 0:
        raise RuntimeError(
            f"ffmpeg failed on {path} substream {index}: "
            f"{run.stderr.decode('utf-8', 'replace')[:400]}"
        )
    return np.frombuffer(run.stdout, dtype=np.float32)


def decode_714(path: str) -> np.ndarray:
    """-> (frames, 12) float64 in SMPTE order."""
    planes: dict[int, np.ndarray] = {}
    frames = None
    for i, smpte in enumerate(SUBSTREAM_SMPTE_714):
        flat = decode_substream(path, i)
        n_ch = len(smpte)
        if flat.size % n_ch:
            raise RuntimeError(f"{path}: substream {i} not divisible by {n_ch}")
        block = flat.reshape(-1, n_ch)
        frames = block.shape[0] if frames is None else min(frames, block.shape[0])
        for j, ch in enumerate(smpte):
            planes[ch] = block[:, j]
    out = np.zeros((frames, N_CH), dtype=np.float64)
    for ch, data in planes.items():
        out[:, ch] = data[:frames].astype(np.float64)
    return out


def db(x: float) -> float:
    return 20.0 * math.log10(x) if x > 0.0 else -math.inf


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--gate-db", type=float, default=-90.0,
                    help="per-channel max |delta| must be at or below this")
    args = ap.parse_args()

    A = decode_714(args.a)
    B = decode_714(args.b)
    n = min(A.shape[0], B.shape[0])
    if A.shape[0] != B.shape[0]:
        print(f"note: length differs ({A.shape[0]} vs {B.shape[0]}), "
              f"comparing the first {n} frames")
    A, B = A[:n], B[:n]

    print(f"\n  A: {args.a}\n  B: {args.b}\n  {n} frames, {N_CH} ch @ 48 kHz\n")
    print(f"  {'ch':>3} {'name':<5} {'A rms dBFS':>11} {'B rms dBFS':>11} "
          f"{'max|delta| dBFS':>16}")
    worst = -math.inf
    worst_ch = 0  # a perfect null leaves every channel at -inf; ch1 is then
                  # as good a representative as any
    for ch in range(N_CH):
        d = np.abs(A[:, ch] - B[:, ch])
        dmax = db(float(d.max()))
        arms = db(float(np.sqrt(np.mean(A[:, ch] ** 2))))
        brms = db(float(np.sqrt(np.mean(B[:, ch] ** 2))))
        if dmax >= worst:
            worst, worst_ch = dmax, ch
        print(f"  {ch + 1:>3} {CH_NAMES[ch]:<5} {arms:>11.2f} {brms:>11.2f} "
              f"{dmax:>16.2f}")

    ident = bool(np.array_equal(A, B))
    print(f"\n  worst channel: {CH_NAMES[worst_ch]} (ch{worst_ch + 1}) "
          f"at {worst:.2f} dBFS")
    print(f"  sample-exact:  {ident}")
    ok = worst <= args.gate_db
    print(f"\n  GATE GA ({args.gate_db:+.0f} dBFS): "
          f"{'PASS' if ok else 'FAIL'}\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
