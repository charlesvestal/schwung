# Selected-vs-playing: the conflation, and a measured route out

One number answers two questions — *which clip is playing* (playback needs it)
and *which clip is selected* (a p-lock needs it). With clip A playing and clip B
selected, a p-lock lands on A.

`85cefb78` narrowed the damage but did not resolve it. The file's `isPlaying` is
now only trusted when the track has exactly ONE clip, because then there is
nothing to be wrong about. With two or more and the transport stopped we
**refuse** — and "step editing is mostly done stopped" is the common case, so
today a multi-clip track silently refuses p-locks it should accept. That is safe
and wrong, and it is the hole this work closes.

## The signal exists: Move marks the selection on its session pads

Measured 2026-09-17, driving the device. In **Session view**, tapping a clip
repaints that track's whole pad row, and the final values decode cleanly.

Track 1, whose row is pads `92 - 8*track + slot`:

```
tapped slot 3   s3=112  s5=24  s6=24   s7=123
tapped slot 5   s3=24   s5=112 s6=24   s7=123
tapped slot 0   s0=112  s3=24  s5=24   s7=123
tapped slot 7   s3=24   s5=24  s6=24   s7=123     <- genuinely EMPTY slot
```

So on that track: **the selected clip settles at 112, every other clip in the
row goes to 24**, and the one empty slot sits at 123 throughout.

**The empty-slot row is the contamination case, and it is decodable.** Tapping
an empty slot leaves NO pad at 112 — everything dims. That is precisely "clip A
was playing, the user switched to empty clip B", and the signal says so.

## DO NOT encode 112 and 24. They are per-track colour indices.

The same sweep on track 2 produced **126 / 122 / 123** and no 112 or 24
anywhere. A decoder keyed on the constants would have worked on track 1 and
silently mis-selected on every other track — the identical failure the map
records for the "empty step" value, which was reported as 98 and then as 102 and
is neither: it is per-track.

So the decode must be **differential**, not a constant: on a repaint, the
selected pad is the one that does NOT take the row's dim value, and "all pads
dim" means an empty slot is selected. Establishing that reliably needs a
per-track characterisation pass that has NOT been done — track 2's 126 may be a
playing marker rather than a selection one, and this survey did not separate
them.

## Next steps, in order

1. **Characterise the row per track**, with the transport stopped AND running,
   separating "playing" from "selected". Four tracks, clips and empties. Until
   that is done there is no decoder to write.
2. Decode it in `clip_state.c`, which already runs only in Session view and
   already reads `ch9` playing / `ch14` queued.
3. **Latch it.** Move emits no session pad LEDs in Note view, so the selection
   must be remembered from the last Session-view repaint. Decide what
   invalidates the latch (track change, set load, a launch from elsewhere) —
   a stale latch is the same class of bug as the file's stale `isPlaying`.
4. Only then widen the `clips_on_track == 1` gate in
   `shadow_chain_mgmt.c`.

## Instrumentation

`selmap.py` / `selmap2.py` are on the device. They tap a pad and report the
FINAL LED value per pad of a track's row — the row repaints on every selection
change, so arrival order is unreadable and only the settled value decodes.
