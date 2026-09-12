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
clips **cannot be launched from Note mode at all**, so there is no launch to
miss, and the step LEDs keep reporting regardless.

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
  Anchors captured before a Continue must be invalidated, not carried.
- The anchor's 2-pulse lag is measured at one tempo only.
