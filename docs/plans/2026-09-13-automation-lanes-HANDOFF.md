# Automation lanes — handoff, 2026-09-13

Supersedes `2026-09-12-automation-lanes-HANDOFF.md`. Branch
`feat/automation-lanes`, PR #509, worktree `.../schwung-lanes`.
**HEAD `508590c9`, all three checks green. The device is on this build.**

Read this before touching the feature; the last section is the only open one.

---

## THE ONE OPEN QUESTION: p-lock on a module that draws its own UI

**9W9 can RECORD automation and cannot P-LOCK it, and cannot SHOW it.** That is
the whole of what is left. Charles confirmed all three on hardware.

Why recording works and the other two do not:

- **Recording is chain-side.** `lane_on_set_param` is called from
  `chain_host.c` on every `synth:*` / `fx*:*` / `midi_fx*:*` write, whatever UI
  made it. Nothing about it knows a UI exists.
- **The p-lock gesture was host-side, twice over.** `onValueWritten` (the hook
  that writes `lanes:plock_step`) lives in `componentParamPagesIo()`, the io
  the HOST builds; and `reconcileStepObserve()` arms `host_step_observe` only
  while `view === VIEWS.PARAM_PAGES`. A module that binds the controller from
  its own `ui_chain.js` — 9W9 does, via `createController` — supplies its own
  io and runs in `COMPONENT_EDIT`, so it has neither.
- **The driven mark is drawn by the param-pages layer**, so a module drawing
  its own screen cannot show it without reading `<key>:modulated` itself. That
  half is arguably not ours to fix.

This is the same blind spot as the enum peek (`docs/PARAM_PAGES.md`): it lived
in the same layer and was invisible to the same modules.

### What was tried, and REVERTED — read before re-applying

`132a2a10` moved the decision below the UI: a component write made while
exactly one step is held also becomes a p-lock, decided in
`shadow_lanes_plock_from_write()` in the shim, where every write already
passes. `8b551a6f` reverts it. **Do not simply re-apply it.**

It was reverted for two reasons, and the second is the important one:

1. **It did not actually work on 9W9.** The interception *ran* — verified by
   poisoning `lanes:plock_reason` with a deliberate bad request and watching a
   held-step write move it — but the translate then refused with `no_bar`, so
   no breakpoint was ever written. Interception running is NOT the feature
   working, and shipping on that partial signal was the mistake.
2. **It is capable of making things WORSE.** It adds a `lanes:plock` after
   EVERY component write while a step is held. A p-lock writes a stepped
   point; recording writes a swept span. So a stale or incidental held step
   turns ordinary recording into p-locks that replace it. Charles reported
   exactly that impression ("or even automation? worse than before"), which is
   why it came out rather than being debugged in place.

The diff is on the branch and readable; the design is still plausible. What it
needs is the missing fact below, plus a guard that a recording pass in progress
is never converted.

### THE MEASUREMENT TO TAKE FIRST

With **9W9 on screen AND its clip playing**, read the strip block:

```
touch /data/UserData/schwung/clip_state_on      # already armed
python3 -c "import json; d=json.load(open('/data/UserData/schwung/clip_state.json')); print(d['step_strip'], d['selected_track'])"
```

`step_plock`'s bar comes from `step_strip_displayed_bar(&ss, strip_track, slot)`,
which needs **`ss.valid`** and **`strip_track == slot`**. So the reading says
which of three it is:

- `valid=false, reject=1` (`STEP_STRIP_NO_STRIP`, nothing lit on row 59) —
  Move is not drawing a strip at all in that state.
- `valid=true` but `strip_track != slot` — the bar is being attributed to the
  wrong track and the refusal is correct but the pairing is wrong.
- `valid=true` and the tracks match — the bar is fine and the refusal is
  somewhere else entirely; instrument `shadow_lanes_plock_step_translate`.

**Measured 2026-09-13 with Move owning the screen and T3 selected but NO clip
in its editor: `valid=false reject=1 strip_track=3 selected=3`.** So "no clip
in the editor, no strip" is established. What is NOT established is the state
Charles actually hit, because recording worked for him — which implies a clip
WAS playing, which is the case where the strip should be present. That
contradiction is the open question.

### A CORRECTION worth carrying

I claimed mid-session that the strip "isn't readable while a module owns the
panel". **That is wrong.** It is decoded from `pin_display_frame()` — the PIN
scanner's reassembly of MOVE's frame, upstream of Schwung's compositor — which
is exactly why it survives Schwung owning the OLED, and it decoded fine
earlier in the session (`strip T3 2pg bold1`, with p-locks landing at exact
phases off it).

---

## What SHIPPED this session, all hardware-verified

- **P-locks work in any time signature.** `stepEditorScrollPosition` is the
  page origin in quarters, per clip, so a held button is
  `scroll + index * resolution` — no bar, no signature, no page count. This
  replaced a bar-and-page reconstruction that could only REFUSE a bar wider
  than 16 buttons, i.e. every bar of an 11/8 set at 1/16.
- **A p-lock edits the SELECTED clip, not the playing one** (`isPlaying` in
  the file), so it works with the transport stopped — which is how most step
  editing is done.
- **Triplet grids deactivate every fourth BUTTON** (12 steps per page), so
  `button != step`. Measured: one arrow at 1/16t moves the scroll 0 -> 2.0.
- **A one-bar loop draws no thickening**, so `bold_segment` 0 means both "bar
  1" and "cannot say"; `step_strip_displayed_bar()` is the only thing that
  tells them apart.
- **Four clearing grains** — `clear` (slot), `clear_clip`, `clear_param`,
  `clear_target` (one component, offered on the module's own page) — plus
  **`lanes:undo`, which SWAPS its buffer so the same verb is redo**. Verified
  with two clips x two modules x seven lanes, each grain checked for what it
  must not touch.
- **A full store evicts an ORPHAN before refusing**, never one still driving.
- **Refusals have names**: `lanes:plock_reason` (the step->phase translation)
  and `lanes:plock_refused` (the write: `unknown_param` etc.).
- The clip lifecycle sweep: loop length changes, dormancy and revival,
  quantize, note-length edits, Capture, delete+undo, Double Loop, clip Copy.

## What it COSTS (measured, and mind the denominator)

    0 lanes/  0 pts ->  2.0us | 4/32 -> 15.3 | 8/48 -> 21.9 | 8/108 -> 23.1

≈ **6.7us fixed + 1.65us per automated PARAMETER + 0.02us per breakpoint**.
Sixty extra breakpoints cost 1.2us between them, so a lane's LENGTH is nearly
free and its EXISTENCE is what costs (`find_param_by_key`, a strcmp scan twice
per lane per block).

**Never quote this as % of frame.** A 2134us frame is 1845us of idle IRQ wait.
Schwung's real per-frame work is **266us clean / 287us with 8 automated
params** — the same 21us is **+8% of what we do**. So: caching
`chain_param_info_t *` on the lane would pay; `lane_eval`'s rescan is NOT worth
fixing.

## Traps this session paid for

- **`dump_display` CANNOT SEE THE SHADOW UI.** It writes the PIN scanner's
  buffer, which is MOVE's frame. Checking a Schwung screen with it reports "it
  did not open" whatever is lit — which produced a chain of wrong conclusions
  and a long detour. Read `/dev/shm/schwung-display-live` for the panel.
- **A track long-press is a TOGGLE**, so pressing it when the session is
  already up dismisses. `clip_state.json`'s `gesture` block now reports the
  five gate terms, which is what settled it.
- **CC 50 is Move's Note/Session toggle**, not just "Menu". In Session Mode the
  pads launch clips: rows descend by eight (92=T1, 84=T2, 76=T3, 68=T4),
  column = clip slot. Pressing a playing clip RETRIGGERS it.
- **`Play` does not launch a clip that undo restored** — the pad does.
- **A shared library links clean with UNDEFINED SYMBOLS.** One `static` on a
  function called cross-TU built green and crash-looped the device (~7s a
  cycle, nothing in dmesg), because an LD_PRELOAD shim that cannot resolve a
  symbol stops MoveOriginal from starting. `-Wl,--no-undefined` is on the shim
  link now, and it immediately found a pre-existing one: `tts_save_config()`,
  called from the screen reader's speed and pitch setters and defined nowhere.

## Smaller items still open

- Cache `chain_param_info_t *` on the lane (now justified by the measurement).
- `LANE_PASS_GAP_BEATS = 1.0` is untested against a slow deliberate sweep over
  a whole bar — the case that would exceed it and stop erasing mid-pass.
  `CLIP_OFF_GRACE_PULSES = 96` was sized by cost, not fitted.
- Scale caps are deliberate: `LANE_MAX` 32, 64 points per lane.
- PR #509 is unmerged: ~40 commits onto a protected `main`.
