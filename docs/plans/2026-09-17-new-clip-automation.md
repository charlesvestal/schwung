# Automation on a NEW clip — what was wrong, and what is measured

A brand-new Move clip is second-class for the 8–12 s before Move writes it to
`Song.abl`. Everything that identifies a clip came from that file, so for that
window a clip the user is plainly looking at has no row, no identity and no
origin. Nine separate reports in one session were this one cause.

This file is the measured record. The war stories are in the commits; what is
here is the behaviour, the numbers, and the parts that are still open.

## The defects, and how each presented

| What the user saw | Cause |
|---|---|
| "my tune isn't taking effect on my med tom" | a p-lock was applied one block AFTER its own note; a drum voice latches pitch at note-on |
| "I lost my p-locks" mid-playback | "we cannot name the row" was compared as if it were a row, releasing every lane |
| locks came back ~10 s later | the file landed |
| a duplicated clip arrived silent | the copy generation was consumed before the reconcile that would have identified the source |
| locks on a new clip never played | an orphaned lane was un-orphaned only *with* a fingerprint, which the blind window has not got |
| p-locks landed on a clip the user was not editing | the PENDING placeholder was gated on the track having NO clips; any other track fell through to the file's stale `isPlaying` |
| a doubled clip's locks went silent | adoption demanded an equal loop length, and Double Loop doubles it |
| a duplicate's locks smeared across the bar | copy and Double Loop dropped the p-lock SPAN, and span 0 means "hold to the next point" |
| automation gone after a reload | a provisional lane was serialized; it came back as a zombie nothing could re-key |

## Loop geometry — measured, not assumed

`lane_adopt_slot` is pure, so this is a standalone probe rather than a device
run.

**Loop LENGTH.** A lengthened clip is the same clip; a shortened one is not.

```
unchanged        4 -> 4    adopts
Double Loop      4 -> 8    adopts
three bars       4 -> 12   adopts
NON-multiple     4 -> 6    REFUSED — stays PENDING, silent
NON-multiple     4 -> 5    REFUSED — stays PENDING, silent
shortened        8 -> 4    REFUSED  (correct: not this take's clip)
```

The length check exists to stop a clip deleted and REMADE inside the save
window inheriting a take. An integer multiple barely widens it — a remade clip
takes the DEFAULT length, which the old equality rule already accepted.

**A non-multiple resize is an open gap.** The lane stays provisional and is
therefore silent, with nothing said. It is the safe direction to fail in, but
it is a failure.

**Loop START.** This one was a real hole and is fixed. A blind clip has no
`loop.start` to read, so the write side is handed 0 and every point is laid
down in 0-space. `lane_eval` only plays points INSIDE the window, so on a clip
whose window does not start at bar 1 the take was silent for good:

```
window at 0    lock at 1.5 -> found
window at 4    lock at 1.5 -> OUTSIDE the window, never heard
window at 8    lock at 1.5 -> never heard
```

`lane_adopt_slot` now shifts the points by the real `loop_start` on the single
transition from "no identity" to "this clip" — once, counted in
`lane_t::reorigined`, and not at all when the origin really is 0, so the
common case stays byte-exact. The comment that used to sit there asserted the
opposite ("already true clip time"), which is true only when the origin is 0 —
precisely what a blind clip cannot tell us.

Note the two axes differ **after** adoption: phases are CLIP-relative, so
moving the window later makes points outside it dormant and the lock keeps its
absolute position. That is the designed behaviour, not a gap.

## The permutation matrix

`tools`-less, lives on the device as `matrix.py` + `clipkit.py`. Each case
makes a clip, p-locks it BEFORE `Song.abl` knows it exists, then does one
thing, and checks the lock is keyed to that clip and to no other.

All five pass on the deployed build, across three tracks — which is the
stronger result, since the behaviour is then not a property of one track:

```
stay-put       PASS   (track 1, row 7)
to-session     PASS   (track 0, row 3)
set-overview   PASS   (track 1, row 5)
other-track    PASS   (track 3, row 7)
double-loop    PASS   (track 1, row 6)
```

**The test scored the wrong thing for several runs**, and it is worth knowing
which: it checked the row it ASKED for, while Move creates the clip on
whichever slot is SELECTED — and the selection does not always follow an
injected tap. A working lifecycle was reported as a failure. It now finds the
row that appeared and scores that, and reports an unverifiable setup as NOT
SCORED rather than FAIL.

## Driving this from a script — traps that cost hours

- **`inject_as_hardware` (offset 108 of `/dev/shm/schwung-control`) resets on
  every shim restart**, i.e. on every deploy. Without it injected input never
  reaches the shim's own scans, so gestures silently do nothing. Not an error:
  it reads as "the fix didn't work".
- **`schwung-testd` serves ONE connection at a time.** Opening a param
  connection while injection holds its own makes the second hang until it
  times out — presenting as "the daemon is up but unreachable".
- **All injection must go over ONE connection.** `/schwung-midi-inject` is
  MPSC and refuses a packet pushed while another producer is mid-write
  ("prior producer stranded"). `INJECT_MIDI` answers ERR; discard that reply
  and a dropped packet is indistinguishable from a landed one.
- **A param read is starved while the shadow grid is up** — it returns "OK"
  with no value. A p-lock needs the grid; reading needs it gone.
- **`ui_mode` can only ever observe SESSION view.** Move paints no session
  pads in Note view, so `clip_state` sees no event and can never report
  `ui=0`. And `Menu` TOGGLES: tapping it blindly lands in Session only if you
  were not already there, which is how clip-pad taps became drum-pad taps.
- **Clip deletion cannot be driven by injection at all.** Neither `Delete`
  alone (CC 119, per `MOVE_UI_MAP.md` §7.4) nor hold-Delete-plus-pad nor a
  600 ms press removed a clip, and loading a set from Set Overview did not
  take either. A human has to clear clips. **This belongs in the map.**

## For `docs/MOVE_UI_MAP.md` (that branch, not this one)

- **The session pads' BASE COLOUR carries the selection**, and `clip_state.c`
  discarded it saying it "carries no state". Selecting a clip repaints that
  track's whole row.
- **The values are per-track colour indices.** Track 0 idled at 17 with the
  selection at 98 (nothing playing) or 122 (a clip playing); track 1 used
  24/112 entirely. A decoder keyed on any of those numbers works on the track
  it was written against — the same trap already recorded for the "empty step"
  value.
- **122 appears on more than one pad**, so "which pad is selected" is not
  always answerable. "No existing clip is selected" — the empty-slot case, and
  the one this feature needs — still is.
- Announcements fire on the `com.ableton.move.ScreenReader` D-Bus `text`
  signal (`"Session Mode"`, `"Dynamics"`), but **selecting a clip announces
  nothing**. D-Bus exposes no selected-clip property; introspection of
  `/com/ableton/move/{screenreader,settings}` shows only that signal and
  `isMoveRunning`.

## Fifteen scenario paths, run on the device

Beyond the five new-clip permutations, fifteen scenarios over the gesture,
target, lifetime and concurrency axes. **14 PASS, 0 FAIL, 1 not set up.**

```
plock-lands         PASS   a lock creates a lane on the playing row
plock-replaces      PASS   a second lock on the same step replaces it (n stays 1)
two-params          PASS   two params on one clip are two independent lanes
clear-param         PASS   clears that param, leaves the other
clear-clip          PASS   clears every lane on the clip
undo-restores       PASS   lanes:undo puts back what the clear took
probe-exact         PASS   reports the locked value AT the step, and says exact
double-loop         PASS   copies the clip's points one loop later
double-one-track    PASS   and does NOT touch another track's lanes
fx-target           ----   not set up: no FX would load on the slot (channel
                           starved during the load); the code path exists —
                           chain_host.c calls lane_on_set_param from the fx
                           branch — but it is UNTESTED on hardware
unsaved-counter     PASS   0 on an identified clip
state-roundtrip     PASS   lanes:state serves a document with the lane in it
discarded-counter   PASS   lanes:discarded is served
no-stray-rows       PASS   no lane is left keyed to another row
refusal-named       PASS   a bad param is refused as "unknown_param", not silently
```

**Three of those started as failures and two were the harness, which is worth
recording separately.** `probe` answers `"<value> <exact>"` and an empty answer
means "nothing to say at that phase" — asserting with a substring scored a
correct `55 1` as a failure. And `probe` and `double` both take the clip's
geometry from the instance, which is NaN with the transport stopped, so a
scenario that neither starts the transport nor passes an explicit loop scores
"the verb did nothing".

**The third was real, and it was a regression from this branch** — see below.

## `edit_unconfirmed` was removed, not tuned

Making the clears agree with the writes (so Delete + step could not delete the
playing clip's lock) exposed that the rule underneath them could not work.
`g_edit_unconfirmed` withheld the row from a write when the screen said a
different clip was being edited — and `clip_state` decodes the selection from
SESSION pad LEDs, while Move paints none in NOTE view, which is the only view
a p-lock happens in. So the answer is always a LATCH from whenever the user
was last in Session view.

Measured: `write_row=-2 unconf=1` with the lane plainly on row 0, and
`clear_param` reporting success having removed nothing. It had also read
`selnow != cslot`, so an UNKNOWN selection (-1) counted as "a different clip" —
the null-vs-false mistake again, one level up.

A rule that can only ever fire on stale data is not a rule. The new-clip case
it was meant to serve is covered where it belongs: when the clip is not in the
file the resolver answers with the PENDING placeholder and the write keys to
that, with no guess about selection involved. The decode itself is kept and
tested; nothing in the resolver reads it.

## Still open

- A **non-multiple loop resize** leaves a blind take provisional and silent.
- **Two blind clips in one save window** share the single PENDING key per
  track; nothing distinguishes them and the length check usually cannot.
- **Create-then-duplicate inside one window** is never detected: the duplicate
  test needs the source present in the PREVIOUS parse.
- A **note-free clip** is never written by Move, so a clip used purely as an
  automation carrier stays blind and its lanes are not saved.
- `clips_on_track == 1` still refuses on a multi-clip track with no strip, and
  the refusal reads as the generic `no_clip`.
