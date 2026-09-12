# Clip-associated automation lanes — design

**Status:** design, not built. Project 2 of two; Project 1 (clip awareness) is
merged as #504 and is the input to everything below.
**Scope of the first PR:** one thin vertical slice — record a knob, hear it
play back in time with the clip, clear it. The step view and the hold-step
p-lock gesture are deliberately not in it.

## What Project 1 supplies

`src/host/clip_state.{h,c}` and `clip_regions.{h,c}`, for each of Move's four
tracks: which clip is playing, its `loop_start` / `loop_len` in beats, and the
current phase. Read
`docs/plans/2026-09-12-clip-awareness-design.md` before this file.

Three of its rules are load-bearing here and are not restated elsewhere:

- **Identity and anchor are separately valid.** A lane may know which clip is
  playing and still not know where in it we are.
- **An unknown anchor is not phase 0.** Phase has three answers — a number,
  "nothing playing", and "I could not tell" — and the third one must reach the
  lane as a refusal, never as a default.
- Phase is hardware-verified **absolutely** (97% over 230 samples, offset 0),
  so a lane that records in the wrong place is this design's bug, not
  Project 1's.

## What a lane is

One lane is **(clip position) × (target, param)**. `(target, param)` is the
address the knob grid, the mod bus and the E16 surface all already use — a
component address plus a parameter key — so a lane addresses a parameter the
same way everything else does.

Its content is an ordered list of breakpoints, each

    (phase_beats_from_loop_start, value_in_the_parameter's_own_units)

### It is ABSOLUTE

The lane *is* the value. The knob's setting is the base underneath it, and
when the lane is cleared the parameter returns to that base. This is what
"automation" means to everyone who has used a DAW, and it is the reason
`chain_mod` needs an override source class (below) rather than reusing the
offset arithmetic it has.

### Time-addressed, never step-addressed

Breakpoints are in beats. Steps are a quantized *view* of them. The
consequence worth stating because it is what keeps the feature cheap: **a lane
never reads Move's note content.** A lane value at a step is meaningful
whether or not a note sits there, and true per-trig p-locks — which would pull
Move's note data into scope — are a different feature.

### A loop-length change destroys nothing

Clip length is mutable from Move's step editor and extending a clip by adding
a note past the end is routine. So:

- breakpoints are stored **unbounded**; the lane has no length of its own,
  only the clip's;
- playback wraps at the clip's **current** `loop_len`;
- nothing is ever rescaled. Stretching a lane to a new length turns a filter
  sweep into a different filter sweep — it is the musically wrong answer even
  though it is the tidy-looking one;
- evaluation considers **only points with `phase < loop_len`**. A dormant
  point past the end is retained but cannot bend the audible curve, and in
  particular cannot do so through wrap-around interpolation, which is how a
  hidden point would otherwise become audible without appearing anywhere.

Growing a clip therefore reveals whatever was recorded there; shrinking it
hides the tail; neither loses data.

### Keying, and the fingerprint that refuses

**Move's clips carry no identity.** A clip in `Song.abl` has `name` (usually
`""`), `color`, `region`, `grooveId`, `stepEditorScrollPosition`, `notes` and
`envelopes` — no id, no uuid. So a lane cannot be bound to "this clip"; it can
only be bound to a position, plus evidence about what was there when it was
recorded.

    key = (set uuid, track, slot, target, param)
    fingerprint = (loop_start, loop_len, note count, first note)   at record time

On a mismatch the lane is **stale**: retained, silent, and never guessed at.
A clip copied into a slot that once held automation does not inherit it, and
the user is told rather than surprised. This is the same tri-state discipline
as everywhere else in this pair of projects — a lane playing the wrong clip's
automation is worse than a lane playing nothing, and worse again if it reports
success.

(`envelopes[]` in `Song.abl` is **Move's own** clip automation concept. We
never read or write it. Do not reuse the word "envelope" for a lane.)

### Interpolation

Linear between breakpoints for float parameters; **stepped for int and enum**,
where a ramp between two options is meaningless. The parameter type comes from
`chain_param_info_t`, which the chain already resolves per key.

## Where the code lives

The lane engine sits **in the chain DSP, beside the LFOs**, not in the shim.
That is where `mod_tick` runs every block — including on a silent slot, via
`mod:tick` — where `chain_mod_emit_value` already is, and where a drain costs
no string formatting on the callback. Lanes are the same kind of object as an
LFO: a source that writes a parameter every block.

`src/host/lane_store.{h,c}` holds the pure parts (the breakpoint list, eval,
wrap, thinning, key and fingerprint comparison) and is compiled into `dsp.so`,
which already builds `src/host/unified_log.c` the same way. Everything in it
is runnable from `tests/host/`.

### The four seams

| Seam | Mechanism |
|---|---|
| clip phase → chain | a new `host_api_v1_t` callback **consuming `reserved[0]`** |
| arm → chain | `lanes:armed` written by set_param, on change only |
| lane → parameter | `chain_mod_emit_value`, with a new **override** source class |
| knob → lane | the existing `v2_set_param` path, plus a self-write guard |

**The phase callback consumes `reserved` from the front and is never
appended.** Appending is what boot-looped a device: a module's copy of the
header declared a field the host did not have, the caller's `if (host->fn)`
guard tested somebody else's memory, and the `blr` jumped into the heap. The
rule is recorded at `src/host/plugin_api_v1.h` and pinned by
`tests/host/test_host_api_reserved_tail.c`.

The callback is **instance-scoped**, like `slot_recv_channel`, so the shim
resolves slot→track and the chain needs no slot-id plumbing:

```c
/* Phase of the clip on the Move track that owns this slot.
 * Returns 1 and fills both outputs, or 0 for "could not tell" —
 * which is NOT phase 0 and must not be treated as it. */
int (*clip_phase)(void *instance, double *phase_beats, double *loop_len);
```

Slot *N* rides Move track *N*. That binding is not new: the shim already
builds a slot as `move_track[s] + synth[s]`, so the two are the same lane of
audio, and the four slot stems ARE the four tracks.

## The override source class

`chain_mod` computes `effective = base + Σ contributions`. A lane is not an
offset, so:

    effective = (override_active ? override_value : base) + Σ offset contributions

clamped to the parameter's range as now. One override per target — a lane —
and LFOs still sum on top of whatever the lane plays, which is the composition
you would want. Clearing a lane goes through the existing
`chain_mod_clear_target_entry(restore_base = 1)`, so the parameter returns to
the knob rather than sticking wherever the lane stopped.

Everything else comes free and must not be re-implemented: write throttling,
the float change epsilon, the int/enum minimum interval, and base tracking on
`set_param` are all already in `chain_mod_apply_effective_value` and
`chain_mod_update_base_from_set_param`.

### An unarmed knob turn must not be inaudible

With an absolute lane playing, a knob turn changes the base — which the
override masks. The parameter does not move, and the knob reads as broken.

So an **unarmed turn takes over until the next loop wrap**, then the lane
resumes: standard DAW punch behaviour, discoverable, self-clearing, and no
mode to enter or leave. The takeover is per (target, param) and expires on the
phase wrapping past the point where it started, so it survives a tempo change
and needs no timer.

## Recording

**Armed is Move's own Record button.** Move records notes, Schwung records
knobs, from one gesture — no new UI, no claimed button. The state is read off
the cable-0 LED stream, the same hook `clip_state_on_led` uses, and **this is
measured on the device before anything is built on it** (which note or CC, on
which channel, and whether armed is distinguishable from blinking while
recording). If it turns out not to be cleanly observable, the fallback is a
Schwung-side arm toggle in Slot Settings and nothing else in this design
changes.

While armed **and phase is valid**, a knob write to a parameter that resolves
through `find_param_by_key` creates a lane implicitly and appends a breakpoint
at the phase sampled **on the callback at the moment of the write** — exact,
not quantised to a UI frame.

Four rules:

- **The self-write guard is not optional.** `chain_mod_set_param_string` is
  the same `set_param` the recorder watches, so without a flag around the mod
  write a lane records its own playback and compounds it every loop. Silent,
  and worse every bar.
- **Thinning at record.** Points arrive at knob-detent rate, not block rate,
  so they are already sparse; collapse consecutive points closer together than
  a few milliseconds or below the change epsilon.
- **A second pass replaces the region it covers** rather than layering two
  curves over each other.
- **Phase unknown refuses.** No recording at a guessed zero, and the refusal
  is visible on screen rather than silent.

## Measured: Move's Record button (2026-09-12, on hardware)

**Record is CC 86, not CC 118.** `schwung-spi`'s header documents 118 as
"same physical button as Sample", and 118 never appeared in the arm sequence.

**The animation is carried in the CHANNEL nibble; the value is the colour it
animates to.** Same shape as Project 1's pad decode, where the channel carried
playing/queued and the colour byte carried nothing. The rates match
`schwung-spi`'s `SCHWUNG_ANIM_*` vocabulary (0x06-0x0A pulse, 0x0B-0x0F blink).

A full Session-view arm sequence, captured on the shim's existing cable-0 scan:

```
pul=921   ch=0  d2=122                    resting
pul=1043  ch=0  d2=0   + ch=10 d2=127     PULSE_HALF  -> armed, waiting
pul=1146  ch=0  d2=127 + ch=14 d2=0       BLINK_4TH   -> queued, counting in
pul=1154  ch=0  d2=127                    static      -> RECORDING (~13 beats)
pul=1471  0xFC stop, then ch=0 d2=0 + ch=10 d2=127    -> armed again
pul=1471  ch=0  d2=122, then ch=0 d2=124              -> disarmed, resting
```

Three rules fall out, and the second is the one that nearly went wrong:

1. **An animation channel (0x06-0x0F) means FLASHING** — armed or counting in,
   never recording. No rate measurement and no colour comparison needed.
2. **"Static" alone does NOT mean recording.** The resting state is *also*
   static and non-zero (122, 124). The discriminator is **full brightness**:
   recording is `d2 == 127`. That is read as a BRIGHTNESS, not a hue — an exact
   palette index is what Project 1 warns breaks the first time Move rethemes,
   whereas "the button is at maximum" is a design intent unlikely to invert.
3. **Evaluate once per FRAME, from the last CC 86 message in it.** Move writes
   the base colour statically and *then* applies the animation, so a burst
   contains `static 127` immediately followed by `blink`. Acting on each message
   in turn reports one frame of RECORDING every time the count-in starts.

### Consequences for the feature

- **Lanes record only while solid** (the user's call): knob automation is
  captured in exactly the window Move captures notes. Holding Record+track and
  the count-in capture nothing.
- **Record does nothing in set selection**, and Move lights nothing there, so
  the absence of the event IS the disarmed state — the mode gate Project 1
  needed for pads is free here.
- **In Note view, pressing Record starts the transport.** So arming and
  obtaining a valid phase are one gesture, and the phase-unknown refusal should
  be rare in practice rather than the common case.
- Stopping with Play leaves Record armed (back to the slow pulse), which
  correctly reads as "not recording" without any extra state.

### How it was measured, and one thing that made it slow

The instrument records every non-empty MIDI_OUT slot on **every cable,
including SysEx**. Its first version took only cable-0 note/CC, which is blind
to two of the four candidate mechanisms (another cable; SysEx) — so its empty
captures proved nothing and read as "no animation packets exist". Widen a probe
to cover the hypotheses before believing a negative result.

Move emits an LED packet **only when that LED changes**, so an idle device
produces nothing at all (4 s of idle: zero events). That makes the capture
clean, and it also means the first two attempts caught only screen-change
refresh bursts. `clip_state.log` served as the positive control: it showed all
four tracks with identity and phase throughout, proving the shared scan was
running while the Record log stayed empty.

## Persistence

The chain serves the whole lane set as one opaque `lanes:state` blob. The
shadow UI writes it to **`set_state/<uuid>/lanes.json`** through the existing
autosave, exactly as slot state is written. There is no second serializer, for
the reason the snapshot feature records: the existing writer already carries
every guard (bail-if-empty, skip-if-unchanged, shim-reports-empty).

A set that has never had automation has no file.

Lanes belong to the set, so they travel with it — the same argument that put
the recall snapshot in `set_state/` rather than in a global directory.

### A deleted clip orphans its lanes; it does not delete them

`clip_regions_forget_deleted` already distinguishes deleted from
not-yet-saved by **history** — a clip present in the previous parse and absent
from this one was deleted; one that never existed may simply be new and
unsaved. A lane whose clip is gone is kept and marked orphaned. Pruning is
only ever an explicit user action.

Deleting user data on a file-diff heuristic during Move's ~35-second
save-after-edit window is the wrong direction to fail in, and lanes are tiny.

## Proving it

In this order, on hardware, one behaviour at a time. Project 1's seven defects
were every one of them found by driving the device and none by a test first;
the tests here are regression protection and are not the proof.

1. **Measure Move's Record LED.** Five minutes, before any lane code.
2. **The slice's acceptance test is audible, and that is the point.** Record
   on, clip playing, sweep cutoff on beat 2, Record off — the next loop sweeps
   by itself, on beat 2, every loop, and stops when the lane is cleared.

   This is **destination-side** evidence. `<key>:effective` is not: it reads
   `chain_mod`'s own table — the bus computing `base + Σ contributions` and
   storing it — so it proves the chain calculated a number, never that a module
   received one. That read is what made the parked `feat/slot-mod-routes`
   verification hollow (reported 18/18, and the parameter never moved), and it
   is the third time in that feature that a probe measured the wrong thing.
3. Then, still on hardware: Session↔Note mode, a clip relaunch, stop/start, a
   set reload, and a loop extended while a lane is playing.
4. `tests/host/` covers the pure parts — eval, wrap at a changed length,
   fingerprint mismatch, thinning, the override arithmetic, and the refusal
   paths.

### One premise to re-check, not assume

The drain is claimed to be nearly free because `chain_mod_emit_value` already
works. It is on `main` and the shipped LFOs use it — but `feat/slot-mod-routes`
generalised the *sources* and did not reach the destination on device,
undiagnosed. Step 2 above is what settles it for lanes, and it settles it by
ear rather than by a read that cannot see the far side.

## Out of scope for the first PR

The step view; the hold-step p-lock gesture (see below); more than one lane
recorded in a single pass; Master FX lanes (a different host — the shim parses
`lfoN:` with a literal `strncmp`); lane copy/paste; and any lane UI beyond an
on-screen lane-driven mark plus Clear.

## Decisions already made, not to be re-litigated

- **Do not take the step LEDs.** Move's step editor *is* those buttons and
  their LEDs, so owning them destroys the thing the lane rides. We read the
  page; we do not assign one.
- **Hold-step + knob is the second gesture, not the first.** It is the better
  p-lock gesture and it works while stopped, which the live pass cannot. But
  it rides Move's own step buttons — a held step is also Move editing the
  clip's notes — and it depends on the page oracle, which is Note-mode-only
  for the playhead and `"Bar N"` otherwise. That is the least certain part of
  Project 1 and the wrong thing to put under the first hardware test.

## Known edges

- A clip can be paged past its end and a note added there extends it. A lane
  must tolerate a page with no clip time behind it yet.
- `shadow_transport_pulses` does not reset on `0xFB` Continue and there is no
  SPP, so anchors captured before a Continue are in a dead timeline. Project 1
  drops them; a lane must not hold a phase across that.
- The lane is silent while the transport is stopped and the parameter sits at
  the knob's value, so sound design while stopped behaves normally. The cost
  is an audible jump on stop if the lane was far from the base — accepted,
  because the alternative (freeze at an arbitrary value) leaves the knob and
  the sound disagreeing with nothing on screen to explain it.
