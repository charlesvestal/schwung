# Automation lanes — handoff

**State:** PR #509, branch `feat/automation-lanes`, 28 commits, 326 host tests
green, clean ARM64 cross-build. **The feature works on hardware** — Charles,
playing it: *"yes, it did. it works."*

Read `docs/plans/2026-09-12-automation-lanes-design.md` (design, with the
hardware findings folded in) and `docs/CHAIN.md`'s lane contract before
touching any of this. The plan doc
(`docs/plans/2026-09-12-automation-lanes-plan.md`) is annotated as-built but is
history now, not instruction.

---

## The four things to do next, in this order

### 1. `loop_start` out of the fingerprint — small, and it bites today

`lane_fingerprint_matches` (`src/host/lane_store.c`) compares `loop_start`:

```c
if (ds > eps) return 0;
```

So **moving a clip's loop silently kills its automation** — a different
`loop_start` reads as a different clip, the lane goes stale, and stale is
silent. `loop_len` is already excluded for exactly this reason (a clip that grew
is the same clip); a clip whose loop you moved is too.

**Do NOT also change the phase origin.** Phases are stored relative to the
**loop start** (`shadow_slot_clip_phase` does `ph - r->loop_start`) and that is
correct. I recommended clip-relative storage during the session and it was
wrong: `loop_start` is observable by **nothing** for a clip the user just made
and whose loop area they edited — not `Song.abl` (unsaved for ~35 s), not the
OLED (the bar strip does not show where the loop begins). Clip-relative would
have to guess, and a guess puts every value a bar or two out while looking
healthy. Loop-relative needs no such fact.

### 2. The phase check is scoring the wrong tracks — fix the instrument first

`clip_state.json`'s `phase_check` showed `seen 74, hit 21` (28%) on tracks 2 and
4 while `selected_track` was 1. The step editor shows **one** track, so its
playhead describes the selected track only — Project 1's own rule is that only
the selected track may be scored, and the within-bar check is not honouring it.

That number has been lying all session: I briefly offered the 28% as evidence
about Start-anchored phase accuracy, and it is noise. Earlier, correctly
attributed, it read **760/765 = 99.3%**.

Fix this **before** validating anything below, because it is the only
instrument that can tell you whether a derived phase is right.

### 3. The OLED reader — closes the "record on a clip I just made" hole

This is the substantial piece, and the session ended having just proved it
possible.

**What was broken:** make a clip in the step editor, press Play, try to record
automation → refused. `T1 -` (nothing playing), `loop_len 0.00`,
`has_phase false`. The clip is not in `Song.abl` yet (Move saves ~35 s after an
edit), so we have no length; with no length there is no phase; with no phase
recording correctly refuses.

**What we measured (hardware, 2026-09-12).** Move's step editor screen carries
both missing facts, and the shim can now read it while the shadow UI owns the
display (commit `2df7c6e3`):

```
row 59            (1-23) (26-49) (52-74) (77-100) (103-126)   <- 5 segments = a 5-bar loop
rows 58-60        thicker on one segment                       <- the bar being edited (bold)
playhead          a 1px INTERRUPTION in the strip, plus a stub at rows 55-57 / 61-63
                  x = 79 then 17 across two captures (wrapped) -- it MOVES
```

- **Bar count → loop length.** Segments of 23-24 px separated by 2 px gaps,
  spanning x=1..126.
- **Playhead x → loop-relative phase**, linear over 1..126. ~6.25 px/beat on a
  20-beat loop, i.e. 0.16 beats/px (~80 ms at 120 BPM). Coarse as an absolute
  readout; ample as an **anchor** that re-syncs while the pulse counter
  interpolates between updates.
- **It is page-independent**, which is what makes it better than the step LEDs:
  measured with the playhead drawn at bars 3 and 4 while bar **5** was the bold
  (displayed) bar. The step LED playhead is visible only while the displayed
  page IS the playing page, so it goes silent for 15 bars in 16 on a long clip.
- **The playhead is drawn as a gap, not a line**: `####.####` in the strip.
  Inside the bold bar it is a gap in a 3px-tall strip. Detect "which column
  interrupts the strip", which covers both.

**What it does NOT give:** where the loop begins in the clip. Charles confirmed
by eye — *"it does not however show you if it starts the loop on bar 1 or bar
3."* Which is fine, because of item 1: phases are loop-relative and never need
it.

**Suggested shape.** Opportunistic capture, not a live clock: read the bar count
when the step editor frame is available and **cache loop length per (track,
slot)**. The length does not change while you record, so a value from ten
seconds ago is as good as a live one, and the existing anchor machinery
(Start / witnessed launch / derived) supplies phase. That keeps the new
dependency to one read rather than a second position pipeline.

**Do not build a model of Move's sequencer UI.** Charles's standing objection,
and it is right: *"this is exactly what I wanted to avoid with all of this,
having to track every state of every Move sequencer UI."* Read Move's own
answer off the screen; do not maintain a parallel model of its loop points,
pages or modes. Every time this session drifted that way it produced a bug.

Precedent for the reading itself: `src/host/shadow_pin_scanner.c` already does
digit OCR by polynomial hash for the PIN screen, and `shadow_master_volume` is
scanned off the volume bar. Counting segments is easier than either.

### 4. Scale the budgets — designed, deferred, and the numbers are worked out

Current: `LANE_MAX` 32, `LANE_POINTS_MAX` 64 **per lane regardless of loop
length**, so a 16-bar clip gets 1 point/beat. Charles asked for "all 32 clips,
~8 things each, realistic granularity".

Requirement: 32 clips = 4 tracks × 8 slots, so **64 lanes per track**
(8 clips × 8 params), 256 device-wide.

| | worst case per track |
|---|---|
| points at 8/beat, 64 lanes × 64 beats | **32,768** |
| RAM at 16 B/point | 512 KB (2 MB device-wide) — fine against 1109 MB free |
| serialized at ~14 B/point | **459 KB** |
| current param value limit | **131,072** |

So the transfer is the binding constraint, not memory or CPU. Three options, in
increasing cost:

1. **Compact the encoding** — free, ~3×. Points serialize as
   `P %.17g %.9g` ≈ 48 bytes today, spending 20+ characters on a phase to
   round-trip a double *bit-exactly*. A phase quantised to 1/960 beat is
   accurate to 0.5 ms at 120 BPM, well under the 2.9 ms block at which the lane
   is evaluated. `P 40 0.385` is ~14 bytes. Costs one assertion relaxed from
   bit-exact to within-one-tick.
2. **A per-clip transfer key** (`lanes:state:<clip>`) — 8 reads of ~57 KB
   instead of one of 459 KB, no ABI change, and it makes the write path
   per-clip so only the clip you touched is rewritten (a 459 KB file on every
   autosave is real flash wear).
3. **Raise `SHADOW_PARAM_VALUE_LEN`** — **not** as costly as I first said: every
   value-sized buffer is already `static`, so the 1.2 MB-stack-frame problem
   from the last raise is gone. But it makes *every* param 4× bigger (~3.5 MB
   BSS) to serve one feature, needs an SHM resize with both ends shipped
   together, and 512 KB leaves only 11% headroom over 459 KB.

**Recommendation: 1 + 2.** And size the point budget as a **shared per-slot
pool** rather than a fixed array per lane, with a per-lane cap of
`clamp(loop_beats × 8, 64, 512)` — so short clips keep today's density, long
clips stop degrading, and the pool spends itself where the music is. 8
points/beat is set by what a hand can do (<10 Hz), not by a round number; 4/beat
flattens a fast gesture.

**Add a per-point `hold` flag while the format is open.** It is **free** —
`lane_point_t` is `{double phase; float value;}`, 12 bytes padded to 16, so
there are 4 spare bytes. A p-lock is a **rectangle**, not a point on a curve;
under linear interpolation two neighbouring p-locks ramp into each other instead
of stepping. `lane_eval` already takes a `stepped` argument (currently derived
from the parameter type), so honouring a per-point flag is a small
generalisation. Adding it later means migrating everyone's files.

---

## Step p-locks — the second gesture, still wanted

Charles: *"we still need to do step p-locks too."* The design records why they
are second: they ride Move's own step buttons (a held step is also Move editing
notes) and they depend on the page oracle, the least certain part of Project 1.

The good news is that a time-addressed lane makes a p-lock a **view**, not a
second store: hold step 5, resolve it to a phase, write the same breakpoint.
With the `hold` flag from item 4 it writes a rectangle and sounds like a p-lock
rather than a ramp.

---

## Two open items nobody has verified

- **The on-screen lane-driven mark.** Task 9 concluded it needed no new code,
  because an override makes `<key>:modulated` answer `"1"` and the existing
  mark reads that. A later agent went looking for something actually *drawing*
  it and found nothing. Both may be true — but no one has seen it on a screen.
- **`lanes:orphan` does not exist.** Orphaning arrives only via the dlsym'd
  `chain_set_clip_deleted`, so a deleted clip's lane going quiet is observable
  by ear alone. Decide whether it needs a surface.

## Two values picked without evidence

- **`LANE_PASS_GAP_BEATS = 1.0`** — how long a pause ends a recording pass.
  Untested against a **slow deliberate sweep over a whole bar**, which is the
  case that would exceed it and stop erasing mid-pass.
- **`CLIP_OFF_GRACE_PULSES = 96`** (one bar) — how long a ch-9 OFF is held
  pending before committing as a stop. Sized by the asymmetric cost, not fitted
  to data.

## CPU: still unmeasured, and my arithmetic is not an answer

Readings taken were **not comparable** (5.8% / 154 µs with the transport
stopped and no synth; 13.5% / 361 µs playing with a lane, which includes the
synth's render, the clip and MIDI). The clean A/B is same-everything with and
without the lane — `Clear Lanes` works, so it is one reading either side.
`param-slow` logged **zero** serves over 1000 µs.

Two inefficiencies left unfixed **on purpose**, so the measurement decides:
`find_param_by_key` is a `strcmp` scan called **twice per lane per block** (once
in `lane_tick`, again in `chain_mod_emit_override`), and `lane_eval` rescans
from index 0 although phase only advances. Fixes: cache the
`chain_param_info_t *` on the lane; remember the last segment index.

Note a correction to something said mid-session: only one clip plays per
**track**, but four tracks means **four** clips playing, so the per-block cost
is (automated params on the playing clip) × 4 slots.

## How to measure any of this

`touch /data/UserData/schwung/clip_state_on` → `clip_state.log` carries
`rec=SOLID|FLASH|off|?` and per-track identity and phase. That `rec=` field
exists specifically so the next session can tell **"the arm never fired"** from
**"the lane never recorded"** — by ear those are identical, and that ambiguity
is what made Project 1's defects take a dozen device passes to find.
`/clip-state` in schwung-manager is the live readout; `clip_state.json` has the
regions grid and the phase check.

**Disarm what you arm.** An armed diagnostic has itself caused the dropouts it
was measuring.

## Lessons this session actually paid for

- **Five defects were in the plan, not the implementations**, and four were
  caught by implementers *reading the enclosing code* rather than trusting line
  numbers. The worst: the recording hook was specified inside
  `if (chain_mod_is_target_active(...))`, true only for a key that already has a
  mod source — so "a lane is created on the first armed write" was impossible in
  the host while every unit test passed. **A unit test that calls a function
  directly cannot see whether anything calls it in production.**
- **Three of the most important defects were found by Charles in minutes**, by
  playing it. None was found first by a test.
- **A probe blind to its own hypothesis reports clean either way.** My
  Record-LED capture filtered `cable == 0` and note/CC, so it could not see
  SysEx or other cables, and its empty captures proved nothing. Later I read
  **stale artifacts as evidence twice** — a peer session's committed file, and
  two OLED dumps I had not deleted before re-triggering. Delete the artifact,
  then measure.
- **"We stopped looking" is indistinguishable from "it stopped happening."**
  The OLED gate is the clean example: no frames accumulated with the shadow UI
  up, which read as "Move stopped rendering". It had not.
