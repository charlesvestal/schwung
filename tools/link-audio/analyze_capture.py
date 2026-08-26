#!/usr/bin/env python3
"""
Score an align capture for Link Audio splices, starves and module cleanliness.

    tools/link-audio/analyze_capture.py move_track.pcm [synth_src.pcm]

Input is raw s16le stereo @44.1k, as written by the shim's align capture
(`echo 30 > /data/UserData/schwung/align_dump_trigger`).

WHAT IT MEASURES, AND WHY NOT SOMETHING SIMPLER
-----------------------------------------------
The obvious detector — "flag any sample-to-sample jump over N" — does not
work here, and believing it cost real time on 2026-08-27. A drum transient
has a huge legitimate jump, so an absolute threshold reports a kick drum and
a dropout identically.

What separates them is WHERE. Move publishes Link Audio in 125-frame blocks
(44100/125 = 352.8 blocks/s, which is exactly the observed `produced_count`,
and `max_frames=125` on every telemetry line). A splice between blocks lands
at a fixed offset mod 125. A drum cannot prefer one phase-of-125.

So the score is a PHASE RATIO: mean |diff| at the worst phase, over mean
|diff| everywhere. 1.0 means no phase structure at all. Measured values:

    module clean (synth_src, every run)      1.02 - 1.07
    braids in the slot   (move_track)        1.02
    osirus in the slot   (move_track)        1.73 - 5.61

TWO TRAPS THIS TOOL EXISTS TO AVOID
-----------------------------------
1. Phase is reported ABSOLUTE, never relative to a window. A 3 s window is
   132300 samples = 50 (mod 125), so consecutive windows of the SAME steady
   artifact print phases 59, 122, 0, 0, ... if you forget to add the window
   offset back. That looks like the lock is drifting when it is rock solid.

2. The "fraction of boundaries with a big step" number is only meaningful
   ONCE A PHASE LOCK EXISTS. With no lock it just counts ordinary transients
   at an arbitrary phase — a provably clean synth_src scores "20.3/s" by that
   metric. It is reported, but gated behind the ratio.
"""

import sys
import numpy as np

RATE = 44100
LA_BLOCK = 125          # Move's Link Audio block, in frames
LOCK_THRESHOLD = 1.25   # below this, treat "best phase" as noise, not a lock


def load(path):
    a = np.fromfile(path, dtype="<i2")
    if a.size % 2:
        a = a[:-1]
    return a.reshape(-1, 2).astype(np.float64)


def phase_profile(mono, offset=0):
    """Mean |diff| per phase-of-125, with phase reported ABSOLUTE."""
    d = np.abs(np.diff(mono))
    if d.size == 0:
        return None
    overall = d.mean()
    if overall == 0:
        return None
    idx = (np.arange(d.size) + offset) % LA_BLOCK
    means = np.array([d[idx == p].mean() if (idx == p).any() else 0.0
                      for p in range(LA_BLOCK)])
    best = int(np.argmax(means))
    return {"best_phase": best,
            "ratio": means[best] / overall,
            "overall": overall,
            "means": means}


def splice_fraction(mono, phase):
    """Fraction of block boundaries whose step dwarfs the local slope."""
    steps, locs = [], []
    for s in range(phase, len(mono) - 200, LA_BLOCK):
        if s < 9:
            continue
        steps.append(abs(mono[s + 1] - mono[s]))
        locs.append(np.mean(np.abs(np.diff(mono[s - 8:s + 1]))) + 1e-9)
    if not steps:
        return 0.0, 0
    steps = np.array(steps)
    locs = np.array(locs)
    return float(np.mean(steps / locs > 3)), len(steps)


def starve_report(mono):
    """A starved frame is written as SILENCE by the shim, so it is visible."""
    z = (mono == 0)
    if not z.any():
        return 0, 0
    edges = np.diff(np.concatenate(([0], z.view(np.int8), [0])))
    starts = np.where(edges == 1)[0]
    ends = np.where(edges == -1)[0]
    runs = ends - starts
    return int((runs >= LA_BLOCK).sum()), int(z.sum())


def report(path, label):
    a = load(path)
    mono = a[:, 0]
    secs = len(mono) / RATE
    prof = phase_profile(mono)
    print(f"\n{label}  ({path})")
    print(f"  {secs:.2f}s   peak={int(np.abs(mono).max())}   "
          f"rms={int(np.sqrt((mono ** 2).mean()))}")
    if prof is None:
        print("  SILENT — nothing to score")
        return
    ratio = prof["ratio"]
    locked = ratio >= LOCK_THRESHOLD
    print(f"  phase-{LA_BLOCK} lock: best phase {prof['best_phase']:3d} "
          f"= {ratio:.2f}x overall   -> {'SPLICED' if locked else 'clean'}")
    if locked:
        frac, n = splice_fraction(mono, prof["best_phase"])
        print(f"  spliced boundaries: {frac * 100:.1f}% of {n}  "
              f"({frac * RATE / LA_BLOCK:.1f}/s)")
    else:
        print("  (splice rate not reported — with no phase lock it would "
              "just count transients)")
    runs, samples = starve_report(mono)
    print(f"  starved frames (silence): {runs} runs, {samples} samples "
          f"({100 * samples / len(mono):.3f}%)")

    # Windowed, with the absolute-phase correction applied.
    W = 3 * RATE
    n_win = len(mono) // W
    if n_win >= 2:
        print("  per-3s windows (ABSOLUTE phase):")
        row = []
        for k in range(n_win):
            seg = mono[k * W:(k + 1) * W]
            p = phase_profile(seg, offset=k * W)
            if p:
                row.append(f"{p['best_phase']:3d}@{p['ratio']:.2f}")
        print("    " + "  ".join(row))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    report(sys.argv[1], "MOVE TRACK (Link Audio input)")
    if len(sys.argv) > 2:
        report(sys.argv[2], "SYNTH SRC (the module's own output)")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
