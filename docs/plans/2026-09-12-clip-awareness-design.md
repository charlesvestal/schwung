# Clip awareness — design

**Status:** design, not built. Measured on hardware 2026-09-12 (Set 2, 120 BPM).
**Scope:** Project 1 of two. This delivers *which clip, where in it*. The
automation/p-lock lane that consumes it is Project 2 and is out of scope here.

## The problem

Move's sequencer emits no CC, and Schwung has never known anything about Move's
clips. Every coordinate an automation lane needs — which clip is playing, how
long it is, where in it we are, which 16 steps the buttons currently address —
was unavailable, and four separate reasoning-forward attempts to derive them
from the MIDI clock were wrong. The clock counts *song* position from `0xFA`; a
clip launched at bar 3 with a 2-bar loop sits a full page out of phase from it,
and `(pulses / 24) mod L` reports step 4 while the clip is at step 20. Silently.

Everything below was measured, not inferred. Where something is still an
assumption it says so.

## What Move actually tells us

All of it arrives on **Move's own MIDI_OUT, cable 0** — the stream that drives
its own control surface — and is already scanned once per SPI frame in
`shadow_clear_move_leds_if_overtake` (`src/host/shadow_led_queue.c:345`).

### The pad LEDs carry clip state, and the CHANNEL is the state

Pad note number maps to the grid as `note = 92 − 8×track + slot` (the same
mapping `src/modules/tools/song-mode/ui.js:41` already uses).

| MIDI channel | Meaning |
|---|---|
| 0 | base colour — clip exists / empty |
| **9** | **PLAYING** |
| **14** | **QUEUED** (launched, waiting for the quantize boundary) |
| 1 | unidentified; observed as note-off with `d2=1` on pads that are empty in `Song.abl`. **Not decoded, not relied on.** |

Verified against independent ground truth: on entering Session mode Move
re-emits the whole grid, and the four channel-9 ON events were Track 1 clip 3,
Track 2 clip 1, Track 3 clip 1, Track 4 clip 4. `Song.abl` carries `isPlaying`
on exactly `tracks[0].clipSlots[2]`, `[1].clipSlots[0]`, `[2].clipSlots[0]`,
`[3].clipSlots[3]`. **Four of four.**

**`move_note_led_state[]` is the wrong place to read this.** That array is
indexed by note and keeps only the last colour, so it collapses the channel —
i.e. it discards the entire signal one line after the scan sees it. The decode
must hook the scan, not the cache.

### A channel-9 ON is the phase ANCHOR

This is the number four earlier attempts went looking for in the wrong place.

```
seq=369  pul=684   ch9  OFF  Track 3 clip 1     old clip stops
seq=371  pul=684   ch14 ON   Track 3 clip 3     user pressed the pad
seq=374  pul=770   ch9  ON   Track 3 clip 3     <-- ANCHOR
```

Launch quantised to beat 32 = pulse 768; the LED reports at pulse 770. **Two
pulses late (~4 ms at 120 BPM), constant.** Correct for it as a fixed offset;
do not model it as jitter on this evidence, and do not treat a single
observation of it as having established the constant across tempos.

### The step LEDs carry a playhead, and it reveals Move's PAGE

Notes 16–31, `d2=126` is the playhead; `122` vs `102` distinguishes step
content from empty. The playhead advanced every 6 pulses — exactly 1/16 — over
200 events with **zero out-of-sequence jumps**.

The load-bearing observation is the *gaps*:

```
15 steps at dpul≈6, then one gap of ~102 pulses (17 steps) from idx15 to idx0
cycle = 192 pulses = 8 beats = 32 steps = 2 pages
```

Sixteen steps lit, sixteen dark, twelve cycles running. **The page does not
auto-follow** — Move shows the playhead only while playback is on the
*displayed* page. That is what makes it an oracle:

> the playhead is visible **iff** `floor(S / 16) == move_page`

so observing it at index `i` while we know the musical step `S` resolves Move's
page exactly. Self-correcting, at worst once per loop.

The measured 8-beat cycle also matches `Song.abl`'s 8.0-beat clip on that
track, so **loop length is derivable from the LED stream alone**, independent
of the file.

### It all survives Note mode

184 of 200 playhead events arrived with `move_ui_mode == 2`. This was the one
finding that could have killed the whole approach — pads mean *clips* only in
Session mode, so the fear was that leaving Session blinded us. It does not:
a clip cannot be launched **by a pad** from Note mode, so there is no *pad*
launch to miss, and the step LEDs keep reporting regardless.

**CORRECTED (2026-09-12).** The claim as first written -- "clips cannot be
launched from Note mode at all, so there is no launch to miss" -- is false,
and it is the assumption that caused a real defect. Pressing **Play** starts
each track's **selected** clip, in any view. A user built a clip in the step
editor, pressed Play, and automation recording was refused: Note view kept the
pad gate closed, so the track sat at `identity_valid` with `clip_slot == -1`,
`clip_state_on_transport_start` had nothing to anchor, and
`clip_state_anchor_pending` skips a track whose `clip_slot < 0`. Tracks that
happened to have identity when `0xFA` arrived anchored at pulse 0 and were
fine, which is why it looked like a per-track mystery rather than a hole in
this paragraph.

The fix is narrow and lives in `clip_regions_seed_state`: on a pending Start,
seed identity from the file for a track that has nothing, and let
`clip_state_anchor_pending` anchor it at pulse 0 / `CLIP_ANCHOR_START`. The
Start is the discriminator -- a real event that really does start those clips
-- so the mode gate and the file-vs-LED precedence are unchanged everywhere
else.

### `"Bar N"` on D-Bus gives the page while STOPPED

Move announces `"Bar 4"` as `com.ableton.move.ScreenReader.text` on every page
change — **absolute** (it names the bar, it is not a delta to accumulate), and
**emitted twice per change**, so it needs the same dedup the existing handlers
carry.

This covers the playhead's one hole: the playhead only exists while playing,
and editing a stopped sequencer is normal. Two independent absolute sources for
one number is also what we want for something that must not be silently wrong —
**if they disagree, we are lost, and we say so rather than picking one.**

`page = bar − 1` holds **only** at 1/16 and 4/4. `stepEditorResolution` is
song-global and the time signature varies, so the conversion takes both. A
hardcoded 16 is a bug waiting for the first 1/32 clip.

**This channel is NOT gated on a user setting, and the reason is structural.**
Move emits `ScreenReader.text` signals unconditionally; the screen-reader
toggle controls only whether a text-to-speech engine *speaks* them. So the
signal is there on every device whether or not anyone has ever enabled
accessibility — the same property that lets metronome detection
(`src/host/metronome_announce.h`) read Move's `"Metronome On"` / `"Metronome
Off"` on an untouched device.

Worth stating explicitly because the opposite assumption is the natural one and
would have been expensive: a page source that works only for users who happen
to run the screen reader is a setting-dependent feature, which is worse than no
feature, and the design would have had to demote `"Bar N"` to an optimisation
and give up stopped-state page resolution for everyone else.

## Cold start, and the anchor that is NOT an anchor

Measured 2026-09-12, round 3 (24,833 events; two transport resets and several
set loads).

### A transport reset re-anchors everything to zero

After `shadow_transport_pulses` resets, playing clips restart from their tops
in lockstep with the counter. Tested as `idx == (pulses / 6) mod 16` against
the step playhead: **24 of 24 exact**, across both reset regions, still locked
250 beats later.

So in the two common cold-start cases — **transport Start, and loading a set** —
`anchor = 0` for every playing track, *derived, with nothing to observe*. A user
who presses stop/play has re-anchored the whole device. The observed anchor
(below) is the exception path for clips launched mid-playback, not the main one.

Consequence worth stating plainly: `phase = (pulses / 24) mod L` — the naive
formula this design opens by rejecting — **is correct after a Start**. It was
only ever wrong for a clip launched mid-playback, which is precisely the case
the ch-9 anchor exists to cover.

### A bare ch-9 ON is a REFRESH, and anchoring on it corrupts every track

Entering Session mode makes Move re-emit the whole grid, including a ch-9 ON
for every clip **already playing** — observed as four ONs across four tracks at
pulses 487-488. A decoder that anchors on every ch-9 ON would re-anchor all
four tracks to that moment and destroy the phase of everything playing
correctly. Silently, and only when the user happens to visit Session view,
which makes it just about undiagnosable from the device.

The discriminator, supported in both directions by the captures:

| Shape | Meaning |
|---|---|
| `ch14 QUEUED` on the pad, then `ch9 ON` | **real launch → set the anchor** |
| bare `ch9 ON`, no preceding ch14, often several tracks at one pulse | **refresh → identity only, leave the anchor alone** |

Round 1's two hand-launched clips both carry the ch-14 first. Round 3 contains
**zero** ch-14 events in 24,833 rows, because no clip was launched by hand —
every ch-9 ON in it is a refresh or a set load.

`clip_led_decode` therefore tracks queued state per pad, and only a ch-9 ON
that *follows* a ch-14 on the same pad writes `anchor_pulse`.

### What is still unknown at cold start

If Schwung attaches mid-session and the user neither starts the transport nor
launches anything, phase is **unknown** for every track — `anchor_valid = 0`.
Identity still recovers on its own at the next grid refresh. The correct
behaviour for a consumer is to **refuse to record** on a track whose phase is
unknown, never to record at a guessed zero.

There is one recovery path, with a limit: the playhead resolves phase with no
anchor at all, since `S = page × 16 + idx`. But the step editor shows **one
track**, so it recovers only the focused one, where the ch-9 anchor covers all
four. They are not interchangeable — the anchor is primary, the playhead is
recovery and cross-check.

## What `Song.abl` still supplies

`/data/UserData/UserLibrary/Sets/<uuid>/<name>/Song.abl`, already read for
tempo (`shadow_sampler.c:527`), mutes (`shadow_overlay.c:380`) and time
signature (`shadow_ui.js:25252`).

- `clip.region.loop.start` / `.end` — **the loop is not always at 0.0.** A clip
  whose loop starts at bar 3 is normal, and phase is relative to loop start.
  The first set inspected had every loop at 0.0, which would have hidden this
  until it was a field bug.
- `stepEditorResolution` — song-global.
- `timeSignature` — already parsed.

## BUILT — and what measurement changed

Implemented in `src/host/clip_state.{h,c}`, `clip_regions.{h,c}`,
`editor_bar_announce.h`; readout at `/clip-state`. Corrections to what this
document originally assumed, all from hardware:

### Anchors come from four places, in descending order of independence

| Source | When | Covers |
|---|---|---|
| `0xFA` MIDI Start | pressing Play, most set loads | **all four tracks at once** |
| a witnessed launch | `ch14` then `ch9`, Session mode | that track |
| a pending Start applied late | identity arriving after `0xFA` | that track |
| solved from the playhead | no Start, no witnessed launch | all four, via the common start |

**Pressing Play is the dominant path and needs nothing else** — no mode, no
track visit, no page. Verified: four tracks with loops 8/16/16/16 all anchored
`src=Start` with live phase, in Session mode, with the playhead never involved.
The derivation machinery below is the RECOVERY path for one narrow case, and
an early draft of this document let that edge case dominate the design.

### The playhead is NOTE MODE ONLY

Session mode produced **zero** playhead events over a full run. An earlier
reading of 16 events "in Session mode" was an artefact: `move_ui_mode` is set
from D-Bus and track presses, so the label lags the actual screen across a
transition. Generalising from those 16 samples was wrong.

### A set load does NOT always restart the transport

Measured both ways. When it does not, there is no Start and no witnessed
launch, and every track sits unanchored — the case the playhead solve exists
for.

### The editor page is PER CLIP, and a global "current bar" is meaningless

Each clip has its own loop length, so its own page count, so its own
remembered page; switching track shows that track's clip at the page it was
left on. A single global bar described whichever track was last paged while
the playhead being scored belonged to the track on screen now. That produced
~65% bar-level agreement, which was twice misread as a phase error. The bar is
kept per track, only the selected track is scored, and each track's page is
seeded from `stepEditorScrollPosition` — which `Song.abl` stores per clip, the
same fact from the other direction.

### Solve the START, never propagate a loop boundary

A playhead sighting gives the start only MODULO that track's loop. Propagating
that to a track with a different loop length is off by a multiple of the
source's loop — which is why an intermediate version needed a divisibility
rule and the awkward advice to "visit the longest loop".

Every clip in a set begins together, so there is ONE start. The sighting is a
congruence; the set-change poll is a bracket (~±1.4 s); together they name it
uniquely whenever the loop exceeds the bracket. Then it is the real start and
every track takes it whatever its loop length. Refused — not guessed — when
the loop is shorter than the bracket (two candidates, choosing is a coin flip
presented as a measurement) or when sighting and bracket disagree.

### Phase is verified ABSOLUTELY

`playhead_idx == floor(pos/res) mod 16` is mod ONE BAR and scores 100% on a
lane anchored exactly a bar out. Comparing our computed page against Move's
announced `"Bar N"` is what catches that. Selected track: **within-bar 97%,
bar-level 97% over 230 samples, offset 0**, with unselected tracks correctly
contributing nothing and DERIVED anchors excluded (they are computed from the
same playhead the score compares against, so including them would read 100% by
construction).

### Move saves `Song.abl` ~35 s after an edit

Not the 24 minutes an idle file suggested. So the geometry stale window is
seconds — which is why a file-diff is enough to notice a deleted clip and the
LED/gesture routes were not worth their permanent dependency.

## The interface (the seam)

This is the whole deliverable, and it is deliberately the boundary between the
two projects. One resolver, so the LED decode and the `Song.abl` read are two
sources behind one answer rather than two call sites.

```c
typedef struct {
    /* Identity and anchor are SEPARATELY valid. Entering Session mode
     * refreshes the grid and gives identity with no anchor; attaching
     * mid-playback gives neither. Collapsing the two is how a lane ends up
     * bound to the right clip at the wrong phase. */
    int      identity_valid;   /* 0 = we do not know which clip */
    int      clip_slot;        /* 0..7; -1 = nothing playing on this track */
    int      anchor_valid;     /* 0 = phase is UNKNOWN, not zero */
    uint32_t anchor_pulse;     /* shadow_transport_pulses at the ch-9 ON */
    double   loop_start;       /* beats, from Song.abl */
    double   loop_len;         /* beats */
} clip_state_t;

int  clip_state_get(int track, clip_state_t *out);
int  clip_phase_beats(const clip_state_t *s, uint32_t pulses, double *out);
int  clip_page_get(int track, int *out_page);   /* playhead / "Bar N" */
```

```
phase = loop_start + ( (pulses − anchor_pulse) / 24  mod  loop_len )
```

### Tri-state discipline, restated because it is the failure mode here

"Playing clip 3", "nothing playing", and "**I could not tell**" are three
different answers. A lane bound to the wrong clip is worse than a lane bound to
nothing, and worse again if it reports success. Every accessor above returns a
validity flag and **no function in this module may return a default**. This is
the same rule `docs/SHADOW_UI.md` records for `shadow_get_param`'s
null-vs-empty, and it cost three bugs in one day the last time it was ignored.

Specifically: an unknown anchor is **not** phase 0.

## Components

1. **`clip_led_decode`** — hooks the existing cable-0 scan in
   `shadow_clear_move_leds_if_overtake`, not the cache. Maintains the per-track
   playing/queued table and records the anchor pulse on each ch-9 ON. RT-safe
   by construction: a table write on the SPI callback, nothing else.
   *Note the scan is gated on `!overtake || skip_led_clear`, so identity goes
   stale during overtake — which must surface as `identity_valid = 0`, not as a
   stale answer.*
2. **`clip_regions`** — reads loop regions, resolution and time signature from
   `Song.abl` on SET_CHANGED, off the callback. Reuses the existing set-identity
   poll rather than adding a watcher.
3. **`clip_page`** — the page oracle. Playhead observations and `"Bar N"`
   announcements, each absolute, cross-checked; disagreement yields invalid.
4. **`clip_state` accessors** — the header above.

## Proving it

The rule this project exists under: **a resolver with green tests and no caller
is the failure mode where a hundred passing assertions sit on something that
does not function.** So Project 1 does not land without a real consumer.

`src/modules/tools/song-mode/ui.js` is the candidate. It already parses
`Song.abl` for its clip grid (`ui.js:328`) and drives playback off wall-clock
time (`ui.js:59`) rather than clip state. Pointing it at this resolver makes
the contract visibly work or visibly fail.

Unit-testable off-device in `tests/host/`: the decode is a pure function of a
MIDI event stream, and we have real captures to replay. The phase arithmetic
and the page oracle likewise — same treatment as `recall_quantize.h` and
`transport_grid.h`.

## Out of scope

The lane itself — store, recorder, step view, drain into
`chain_mod_emit_value`. Two decisions already made that belong in that doc:

- **Do not take the step LEDs.** An earlier version of this design had Schwung
  own notes 16–31 so its page cursor would be authoritative. That died on the
  observation that **Move's step editor *is* the buttons and their LEDs** — so
  owning them destroys the thing the lane is supposed to ride. We read the
  page; we do not assign one.
- **The lane is time-addressed**, in beats within the loop, with steps as a
  quantized view. It therefore never reads Move's note content, and a lane
  value at a step is meaningful whether or not a note is there. True per-trig
  p-locks would pull Move's note data into scope; that is a different feature.

## Known edges

- **Clip length is mutable from the step editor.** Paging to a bar past the end
  is allowed, and adding a note there extends the clip. A lane must tolerate a
  page with no clip time behind it yet.
- `shadow_transport_pulses` is not reset on `0xFB` Continue and there is no SPP,
  so bar alignment after a mid-song Continue is wrong until the next `0xFA`.
  Anchors captured before a Continue must be invalidated, not carried. On
  `0xFA` the counter DOES reset, so anchors from before it are in a dead
  timeline and must be dropped — but that costs nothing, because a Start
  re-anchors everything to 0 anyway (see Cold start).
- The anchor's 2-pulse lag is measured at one tempo only.
