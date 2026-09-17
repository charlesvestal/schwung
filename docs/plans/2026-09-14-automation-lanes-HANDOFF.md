# Automation lanes — handoff, 2026-09-14

Branch `feat/automation-lanes`, PR #509, **not merged** (main is protected;
CI runs on push). The device is running this branch's build. Everything below
through "What shipped" is committed and pushed.

The previous handoff is `2026-09-13-automation-lanes-HANDOFF.md`; it is still
accurate about the feature's shape. This one is about a day of USING it.

---

## THE ONE THAT MATTERS: automation needs an extra loop after recording

> **MEASURED 2026-09-17 — BOTH NAMED SUSPECTS ARE INNOCENT, and the bug does
> not reproduce on the current build.** Read this before acting on the
> hypothesis below; it is left in place because the reasoning is still worth
> having, not because it was right.
>
> **First, the instrument was lying.** `lane_trace` samples inside
> `shadow_lanes_publish_driving()`, which the shim calls every **16th** frame,
> and then gated on `frame % 17 == 0`. 16 and 17 are coprime, so it fired every
> 272 frames — **1.26 Hz against a documented 20 Hz**, 0.79 s between samples.
> The question here is whether a lane goes silent for one BEAT (~0.45 s) or one
> LOOP (~1.8 s), so the instrument could not resolve its own question. Fixed to
> count CALLS rather than frames; resolution measured afterwards at **0.023 s**.
> The old unit test asserted `frame % 17` in isolation and passed the entire
> time.
>
> With that fixed, driving takes through `v2_set_param` (the same entry a knob
> detent uses):
>
> - **`punch_until_wrap` does not outlive the take.** An unarmed turn armed it
>   (`punch=1`, `pph=2.875`); the very next wrap cleared it, *mid-take*, exactly
>   as the unconditional expiry intends.
> - **The recording pass releases in about a beat.** Last write at t=2.74,
>   `drv=1` at t=3.12 — **0.38 s**, which is `LANE_PASS_GAP_BEATS` at that
>   tempo, not a loop.
> - **A brand-new lane, recorded from nothing, played in the SAME loop**: writes
>   t=0.00–0.79 (n 1→20), `drv=1` at t=1.12, phase 3.75, before the wrap.
>   `stale=0 orph=0` throughout.
>
> So the extra loop was almost certainly the **file-sync trio** fixed in
> `85cefb78` on 2026-09-15 — one of which measured a p-lock inaudible for
> **7.1 s**, which at ~1.8 s a loop is about four playthroughs, and which is
> what "more than one playback loop" actually looks like. That fix postdates
> this handoff.
>
> **Not yet reproduced:** a take inside the blind window on a clip Move has not
> written yet. Launching an empty slot STOPS the track, and Play then toggles
> the global transport, so the scripted route kept ending with `ph=nan` — the
> gesture sequence needs a hand, or a better route to a playing new clip.


Reported, and not yet diagnosed:

> "still taking more than one playback loop to actually play recorded
> automation ... instead of playing it on the next playthrough after recording
> is done. maybe it's because i only record steps 2-12 or something and it
> waits for the full next 1-16 before, or if i overlap? idk, but the next time
> a recorded step is played back, it should have the automation, obviously."

That expectation is right and this is the core of the feature, so it is the
first thing to pick up.

### What the code says, which is NOT yet what the device says

Two mechanisms can silence a lane, and only one of them should be able to
cost a whole pass:

- **The recording pass** (`lane_pass_live_at`, `LANE_PASS_GAP_BEATS` = 1.0).
  Bounded to ONE BEAT of travel past the last write, so it cannot explain a
  whole loop. After the sweep ends the transport carries a beat past the last
  write, `pass_live` goes false, `lane_record_end` fires and the lane drives.
- **`punch_until_wrap`** — an UNARMED knob turn hands the parameter to the
  hand until the transport wraps. That is EXACTLY one loop, which matches the
  report. The record path deliberately leaves the punch alone ("the unarmed
  punch and the armed pass are two different mechanisms"), so a punch armed
  before or during a take survives it and suppresses the lane for the rest of
  that loop.

So the leading hypothesis is: **the take is preceded (or accompanied) by an
unarmed turn that arms a punch, and the punch — not the pass — is what eats
the loop.** The "steps 2-12" detail fits it: the sweep does not reach the loop
end, so the wrap that expires the punch is further away than the end of the
gesture.

A second candidate worth eliminating in the same sitting: whether `rec_active`
is cleared where the code claims when the sweep ENDS MID-LOOP rather than at
the end of one.

### How to measure it rather than reason about it

Do NOT patch forward from the hypothesis. The instruments exist:

```
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'
# then, while recording a sweep over part of the loop:
printf "SLOT 0\nGET_PARAM lanes:driving\n" | python3 /data/UserData/schwung/testd2.py
```

`lanes:driving` is a COUNT of lanes currently holding an override. Sample it
across the pass and the following loop: the question is whether it goes to 0
and stays there for a whole loop (punch) or only for about a beat (pass).
If more is needed, add a temporary getter for the lane's `punch_until_wrap` /
`rec_active` rather than inferring — both are one line in `chain_lanes.c`'s
getter block, and a temporary probe is cheaper than a wrong fix.

---

## Also open

### Delete + knob with NO step held should clear that parameter's lane

Asked for directly. The scope rule then reads as one sentence — the held step
narrows it to a step, the knob narrows it to a parameter:

| gesture | clears |
|---|---|
| Delete + step, release | everything on that step *(exists)* |
| Delete + step + knob | that knob, on that step *(exists)* |
| **Delete + knob, no step** | **that knob's whole lane for this clip** *(missing)* |

CC 119 + a knob touch with no step held is currently unclaimed, so nothing
conflicts. It must ANNOUNCE what it cleared and stay undoable (`lanes:undo`):
a brush of a knob while Delete is held would otherwise wipe a lane silently.
The chain already has a per-parameter clear verb — see `lane_is_for_param` in
`chain_lanes.c`.

### Should removing a NOTE remove that step's locks?

Asked, not decided. My recommendation is NO, and the reasoning is worth
keeping whichever way it goes:

- A lock is a lane point at a phase, with no knowledge of a note. That
  independence is what makes a TRIGLESS lock possible at all — Elektron's own
  category, and a thing we support.
- **We can only see a note removal SOMETIMES.** The only evidence is Move's
  step LEDs (`d2=122` content vs `102` empty, recorded in the clip-awareness
  design doc, never decoded). That is visible only while that clip's page is
  on Move's screen. A rule that fires most of the time is worse than one that
  never fires, because the user cannot tell which state they are in.
- There is already an explicit, reliable gesture (hold step + Delete), and the
  lock map now makes an orphaned lock visible rather than hidden.

If it is wanted anyway: decode the step-content LED and remove locks ONLY when
we witness the step going empty on the page in front of us — never inferred,
never retroactive — and announce it.

### Not hardware-verified from today

- The **launch-boundary snap** (below). Unit-tested against the measured lags;
  no clip has been launched against a known lane since it deployed.
- The **flashing outline** and the **map staying up during the gesture** —
  verified by rendering offline, pixel-exact, but not watched on the device.
- CI was queued at the time of writing; **the branch has not been seen green.**

---

## What shipped today

Eight commits, all deployed to the device.

- **The lock map peek** — hold a step with the grid up and a strip rises over
  the footer showing which of the sixteen steps carry locks: solid for a lock
  on a parameter of the page you are looking at, a thin bar for one elsewhere
  in the slot, the held step framed and flashing. It stays up through the
  lock gesture (dismissing on knob touch hid it during the one action it
  exists to support) and refetches after a clear.
- **The step buttons came back.** `step_observe` is written by shadow_ui from
  its `view`, and a VIEW OUTLIVES A DISMISS — so the flag sat at 1 with Move
  on screen and the shim went on withholding every bare step press. Taps under
  the threshold still replayed, which is why it read as a broken sequencer
  rather than a stuck flag. Now gated on the display in BOTH the UI and the
  shim. The tap window went 250 ms -> 500 ms: the question is not "what does a
  quick tap look like" but "what is the slowest press meant as one", because
  everything past the threshold silently does nothing.
- **A punch cannot outlive the transport.** The only thing that ends a punch
  is a phase comparison inside `lane_tick`, which returns BEFORE it when the
  phase is unknown — so a punch armed before a stop survived it and the lane
  stayed silent until the transport passed back under `punch_phase`. (Note the
  relationship to the open bug above: the punch is implicated there too.)
- **A laned parameter reports as MODULATED even between its points.** The grid
  asks `<key>:modulated` about once a second; the mod bus answered from live
  sources, and a p-lock's override is up for one step (~60 ms) per loop. So
  the flag was essentially never sampled true, the key never joined the fast
  read lane, and the cell showed the base while the ear heard the lock —
  "I hear it but I don't see it".
- **Refusals carry provenance.** The check read the refusal registers on the
  same tick as the write; the param channel is a queue, so the read could be
  served first and report the PREVIOUS gesture. One real refusal was then
  re-reported forever ("hank's tone knob says can't automate, that's wrong").
  Now `plock_seq` — bumped only on a confirmed landing — proves the write
  took, and a lock that lands costs no round trip at all.
- **Notices are sentences.** Sentence case, and they WRAP: the box was one
  line and clipped in silence, so "Step automation cleared" lost its last
  word.
- **A launch anchors to its quantize BOUNDARY.** See the measurement below.
- **An extended clip is as long as the SCREEN says.** `Song.abl` is written
  ~35 s after an edit, so locking a step in newly added bars was refused as
  past the end. The strip was already consulted for a clip ABSENT from the
  file; it is now consulted when the entry is merely STALE.
- **A clip that has never PLAYED still has an identity.** `identity_valid`
  needs a ch-9 ON, and the decoder only runs in Session view — so a new clip,
  or one playing since before you last looked at the session grid, had none,
  and every p-lock was refused with "no clip on this track". The write path
  had already decided the other way ("a p-lock edits the clip on SCREEN"); the
  two halves of one gesture disagreed and the refusing half won.

---

## The launch-lag measurement (do not redo it, DO extend it if you touch this)

15 launches, three tempos, from Move's own LED stream with pulse stamps —
the `ch-9 ON`'s offset past the bar boundary it belongs to:

```
 60 BPM   1 1 1 1 1        mean 1.00   = 41.7 ms
120 BPM   1 1 2 1          mean 1.25   = 26.0 ms
180 BPM   1 2 3 2 2 1      mean 1.83   = 25.5 ms
```

**The lag is NOT a fixed number of pulses** — it grows with tempo, which is a
roughly constant ~25 ms delay counted in pulses. The clip-awareness design doc
had ONE observation at ONE tempo and proposed a fixed 2-pulse correction;
applying it would have put an 83 ms error at 60 BPM, four times the defect.
One tempo cannot separate a constant from a proportion.

So nothing is corrected by a constant: a launch is quantised, so the true
anchor IS a grid boundary, and the anchor snaps to it
(`CLIP_LAUNCH_SNAP_PULSES`). To the BEAT, not the bar, because we are not told
the user's launch quantize and every grid Move offers is a whole number of
beats.

---

## Driving the device — the traps this session paid for

- **`pgrep -f` / `pkill -f` MATCH THE SSH SHELL ITSELF.** `pkill -f
  schwung-testd` inside an ssh command kills that command, so everything after
  it in the same line silently does not run — which is how a tempo restore
  went missing and how a stopped daemon reported itself as still up. Use
  `pgrep -x`, and put a kill last.
- **An injected press must be TIMED.** Each `testd2.py` invocation carries its
  own ~0.35 s sleep, so press and release in two invocations is a HOLD, not a
  tap. A held Menu opens Master FX; a 90 ms tap reaches Move and toggles
  Session/Note. `tap.py <press-hex> <hold-ms> <release-hex>` does it on one
  connection.
- **`move_ui_mode` is INFERRED from Move's screen-reader announcements**, not
  read from Move. It can say Session while the screen says otherwise. Confirm
  with the LEDs (`ch=9`/`ch=14` rows in the capture), not the flag.
- **Killing `shadow_ui` does not restart it.** It came back only via a deploy.
  Patching the deployed `.mjs` and killing the process proves nothing if the
  process is the same one — check its age against the file's mtime before
  believing any on-device JS experiment. Three of mine were void.
- **`console.log` from an IMPORTED `.mjs` at module scope is not routed** to
  `debug.log` (the logger is installed later). Logging from inside a function
  works.
- **Move emits NO session pad LEDs in Note view**, and the step playhead only
  in Note view. Whichever you need decides which view the device must be in —
  and that is also the root of the never-played-clip bug above.
- `desired-tempo` + a touch gives programmatic tempo control through the Link
  sidecar; `last-tempo` confirms Move took it.

Device is currently clean: tempo 132, no diagnostic flags armed, testd
stopped, `inject_as_hardware` 0.
