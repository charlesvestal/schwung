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

Updated 2026-09-18, after the design review. Two of the five are closed; the
other three cannot be solved without guessing, so what they got instead is a
voice.

- ~~**Two blind clips in one save window** share the single PENDING key per
  track~~ — **FIXED**, and it was worse than recorded: not a lost take but
  active mis-binding. Both clips' points landed in one lane and the second
  clip's `pending_len` overwrote the first's, after which the length gate
  compared the arriving clip against the wrong clip's length. The placeholder
  is a small RANGE now, one value per concurrent take, chosen by the clip
  length off the bar strip. Residue: two blind clips of the SAME length are
  still indistinguishable and still share a take — nothing observable
  separates them.
- ~~`clips_on_track == 1` refuses on a multi-clip track with no strip, and the
  refusal reads as the generic `no_clip`~~ — **NAMED**. The resolver reports
  it on the 1 Hz `lane-row:` line with the clip count, so "the clip is there
  and its row is not readable" is no longer identical, from outside, to "there
  is no clip".
- A **non-multiple loop resize** leaves a blind take provisional. Refusing is
  CORRECT — the alternative is inheriting a clip that is not the take's — so
  this stays, but it is no longer silent: `lanes:pending` counts it and the
  autosave says so once it is past the save window.
- A **note-free clip** is never written by Move, so there is never a row to
  key against. Structural, not a defect we can fix: with no row there is
  nothing to adopt. Reported the same way.
- **Create-then-duplicate inside one window** is still never detected: the
  duplicate test needs the source present in the PREVIOUS parse.
- **Armed recording** remains untested end to end (deferred deliberately).

### What "reported" means

`lanes:pending` serves `"<takes> <lanes> <stalled>"` per slot. `takes` counts
distinct blind CLIPS rather than lanes, because "three parameters on one new
clip" and "three new clips" are different sentences. `stalled` is the count
past `LANE_PENDING_STALL_BLOCKS` (~30 s, against a save window MEASURED at
8-12 s), which separates a take that is merely new from one that is never
going to resolve.

The autosave reads it in the branch that DROPS the take — a slot holding only
provisional lanes serves an empty document, so that branch is where the file
is deleted — logs the count every pass, and announces once per episode when
something is stalled. Once per episode and not once per pass: the autosave
runs every ~5 s and a stuck take stays stuck, so reporting on the condition
is how a useful sentence becomes noise.

### Not hardware-tested

None of the 2026-09-18 work has run on the device. Lanes are off by default
(`lanes_on`), and the take-selection change alters behaviour inside a live
blind window specifically — arm the switch and try two new clips in one window
before trusting it.

## 2026-09-18: can Move be asked to write the song? No.

The whole clip-identity apparatus exists because Move writes a new clip to
`Song.abl` 8-12 s late. If the firmware could be told to flush, the blind
window would close and the placeholder, the take range, the adoption ladder,
the length gate and the stall report could all be deleted rather than
maintained. So it was worth an afternoon to find out.

`com.ableton.move.Browser.saveSongIfDirty` looked like exactly that lever. It
is not. The measurement:

| trial | flush calls | edit -> Song.abl written |
|-------|-------------|--------------------------|
| B     | every 2 s   | ~20 s                    |
| control | none      | ~23 s                    |

During trial B the song was PROVABLY dirty -- a write did eventually arrive --
and the method was called about ten times before it did. A working on-demand
flush produces the write on the FIRST call. No acceleration, so this is a
clean negative rather than an absent measurement.

**We were calling it correctly.** A no-arg call is refused with
`InvalidArgs: expecting 's'`, so sd-bus validated our call against a real
registered vtable entry: the method exists, has a signature, and accepts our
string with no error. What it is NOT is a hook into the live sequencer.
`Browser` is Move's CONTENT-LIBRARY interface -- `importSongBundleFile`,
`refreshCache`, `replaceFileReferences`, `saveSongIfDirty` -- and
MoveWebService calls it immediately before serving a `.ablbundle`. Read that
way every observation fits: "if this library entry has unsaved metadata, write
it", a no-op when the library copy is already consistent. Its sibling
`refreshCache` is equally inert, while a `Settings` property read returns real
data, so the service itself is fine.

Static analysis agrees in an odd way and is recorded so nobody repeats it: the
method name is in MoveOriginal's `.rodata`, and NOTHING references it -- not
one of 5.68M instructions, no relocation addend, no data pointer. Consistent
with names held as pool offsets rather than pointers. An implementation
detail, not the answer; the empirical test above is what settles it.

**So the blind window is structural.** Move's own save is ~20 s for a note
edit, there is no observable way to hurry it, and even a working flush at 20 s
is far too slow for a gesture. The design consequence: KEEP THE DEFERRAL. A
lock on a brand-new clip records and plays immediately and binds to a row a
few seconds later; that is the honest answer and the only one available.

### What else the afternoon bought

- **The Move HTTP API is usable.** With the Manager's
  `Ableton-Challenge-Response-Token` cookie: `/api/v1/data/Sets` enumerates
  every set (uuid, name, size, cloud state) and `/api/v1/data/Sets/<uuid>`
  returns a full `.ablbundle`. Its `Song.abl` matched disk exactly, which is
  itself a finding -- the export does not flush either.
- **`com.ableton.move` carries no clip or selection state at all**:
  `SongRenderer`, `Browser`, `Settings`, `ScreenReader`, `auth`, `cloudauth`,
  `perf`, `sshkeys`. That closes off "ask Move directly" as a design avenue
  rather than leaving it an open maybe.

### Three instrument failures, which cost most of the session

Recorded because each one produced a confident wrong conclusion, and all three
are things this repo already knew.

1. **Injected gestures were not reaching Move's firmware.** Proved by
   injecting Menu and watching the pad mode not change. Three runs had
   therefore "created clips" that were never created, so `saveSongIfDirty` was
   being tested against a song that was never dirty -- and a no-op is
   indistinguishable from a broken lever when there is nothing to save. AN
   INSTRUMENT NEEDS A POSITIVE CONTROL; this one had none for either the
   gesture or the dirtiness.
2. **`pkill -f <pattern>` kills its own ssh shell** when the shell's command
   line contains the pattern. It silently killed two watchers before they
   started and later killed a background flush loop. Do not use `pkill -f` on
   this device; kill by pid.
3. **Device-side logging to a file produced nothing** twice for reasons not
   worth chasing. Polling from the host over ssh, printing to stdout, worked
   first time. Prefer it.

The lasting fix for (1) is that the harness must verify its own gestures --
every scenario needs a witness that the gesture LANDED before anything it
causes is scored. Until that exists, on-device conclusions are not evidence.

## The remaining wrong-clip bug, located precisely

Two hardware runs on 2026-09-18 had a p-lock land on a clip the user was not
editing -- row 0 in one run, row 1 in another. The cause is NOT the ladder
inside `shadow_slot_clip_phase`'s `cslot < 0` block. It is that the block is
SKIPPED:

```c
int cslot = (tr->identity_valid && tr->clip_slot >= 0 && ...) ? tr->clip_slot : -1;
...
if (cslot < 0) {           /* the strip check, and PENDING, live in here */
```

So the moment a clip is PLAYING on that track, the live identity supplies its
row and every honest branch below -- including "the strip says a clip is being
edited, answer PENDING" -- never runs. A write then takes the playing row
while the user is step-editing a different clip. That is the whole defect, and
it explains both runs: something was playing in each.

**A deletion does not fix it, and one was tried and reverted.** Removing the
`clips_on_track == 1` fallback looked like removing a guess; it is not one.
That fallback only runs when the strip is DOWN, i.e. nothing is being
step-edited, and it answers the ordinary post-boot case where nothing has
played and the file's selected clip IS the clip on screen. Deleting it made
`tests/host/test_slot_clip_phase` fail on exactly the two assertions that pin
that case, and rightly: without it, automation on an existing never-played
clip can never bind, because `lane_new_row` only fires for a clip that newly
APPEARS in the file.

### The fix this needs

The resolver answers ONE row and two callers want different things: playback
wants the clip that is PLAYING; a write wants the clip on SCREEN. They differ
only while a clip plays and a different one is edited -- which is exactly when
the bug fires. So the resolver has to report the row AND whether a write may
use it.

That is the shape of the removed `lane_edit_unconfirmed`, and the reason it
failed the first time was its SIGNAL, not its shape: it read the session pad
decode, which Move only paints in SESSION view, while p-locks happen in NOTE
view -- so it was always a latch from whenever the user last visited Session,
and it withheld the row from gestures aimed squarely at the playing clip.

The strip does not have that problem. It is drawn by Move in Note view, which
is when step editing happens, and it reports the edited clip's BAR COUNT. So
the discriminator is a length comparison: a clip being step-edited whose
geometry disagrees with the playing clip's length is a DIFFERENT clip, and a
write must defer to PENDING. Agreeing lengths take the row, which keeps the
common case exact.

Residue, stated up front: a new clip whose length happens to equal the playing
clip's is still indistinguishable, so a write there still takes the playing
row. Same class of residue as two blind takes of equal length, and for the
same reason -- length is the only positive evidence available.

NOT IMPLEMENTED. It needs the flag published to the chain again and a write
path that reads it, and it must be verified per-guard on hardware rather than
assumed, which needs a harness that witnesses its own gestures first.
