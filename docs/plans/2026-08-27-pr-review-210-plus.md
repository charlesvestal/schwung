# Review of the outside-contributor PRs, #210+

Seven open PRs from contributors other than charlesvestal, reviewed 2026-08-27.
All seven are CI-green and mergeable (#307's checks had not run at review time).

**Every claim below was verified against the tree at `32a60f3a` unless it is in a
"Not verified" block.** The verification is the expensive part and it is the part
a session boundary loses, so the file cites line numbers and quotes the comments
that settle each question. Where the first-pass review was wrong, the correction
is recorded rather than the finding silently dropped — a withdrawn finding that
leaves no trace gets re-raised by the next reader.

## Session split

Grouped by what actually interacts, not by size:

| Session | PRs | Why together |
|---|---|---|
| 1 | #292, #291 | Both edit the same cable-0 filter block in `shim_post_transfer` |
| 2 | #281 | Independent (shadow_ui.js editor return path) |
| 3 | #210 | Independent (Python test tooling only) |
| 4 | #221 | Independent, but needs a design conversation not a patch |
| 5 | #293, #307 | Both add producers competing for the same MIDI_OUT mailbox |

### Cross-PR interactions — read before merging any of these

**#291 + #292 conflict textually and semantically.** Both add conditions inside
the mode-2 / mode-1 branches of the cable-0 filter loop in `shim_post_transfer`.
Beyond the merge conflict: #292 places `if (power_sysex_hit) filter = 0;` *after*
every branch, deliberately, so that it outranks them. #291's finding 3 below is
that later unconditional `filter = 0` writes defeat earlier guards. The two PRs
are on opposite sides of the same design question — whether that loop's
precedence is "last writer wins" — and whoever lands second should make the
ordering explicit rather than appending another clause.

**#293 + #307 both add mailbox producers.** #293 adds a dedicated overtake ring
drained *before* the shared inject ring. #307 adds chain-knob producers to the
ROUTE_EXTERNAL ring, drained at `schwung_shim.c:5992` — before
`shadow_inject_ui_midi_out()` at `:6090`. With both landed, MIDI_OUT's 80 bytes
have three competing producers and the last one in line loses packets outright
(see #307 finding 2). Settle the drain order once, with #293 first.

**Corrected 2026-08-27 (Session 5).** They are two *different* mailboxes and
they do **not** conflict textually — #293 touches `schwung_shim.c:1692`, #307
`:4549`, and the docs/API.md hunks are 100 lines apart. The real coupling is
narrower and sharper than "three producers":

- **MIDI_IN (Move's inbox)** is #293's alone. #307 does not touch it.
- **MIDI_OUT (80 bytes, USB-A)** is #307's. #293 does not touch it either.
- What actually binds them is `overtake_ext_ring`: #293's finding 1 asks for a
  flush on overtake unload by pointing at the ROUTE_EXTERNAL ring's existing
  reset as the template — and #307 is what makes that reset's stated
  justification false, by giving the ring a producer (the chain) that outlives
  the overtake module (#307 finding 2b). Land #293 first and the second
  reviewer sees a template that has quietly stopped being one.

So the sequencing still holds, for a different reason than first recorded.

---

## Session 1 — DONE 2026-08-27. Both MERGED, findings fixed in-house (#309)

- **#292 merged** as `fd9baa54`. Merging it is a strict improvement even if the
  open id-byte question resolves the unfavourable way: Move's shutdown prompt
  only appears on a **hold**, which is the case the author device-verified, so a
  tap-only failure is exactly `main`'s existing behaviour, not a regression.
- **#291 merged** as `8588382f`, and its findings fixed by **#309** rather than
  sent back to the author.

**The hold-for-the-author call was wrong, and the reasoning is worth keeping.**
The case for holding #291 was "its first consumer is movy, and master volume
jumping mid-gesture points nowhere near the display scanner, so shipping it costs
DimaDake the debugging session." That argument evaporates the moment we fix it
ourselves — it was an argument about who pays, not about whether the code is
safe to land. And the fix is cheap to land safely: every line of it is gated on a
flag that defaults off and that nothing in the shipped fleet sets, so its blast
radius is movy and nothing else. **A contributor round-trip is not the default
just because the finding is the contributor's.**

#309 carries: the `native_display_visible` fix (the real bug); the suppression
moved into a documented `=== PRECEDENCE TAIL ===` so `button_passthrough` and the
co-run cede cannot silently defeat it — which is also the ordering ask this file
left for whoever landed second; the clear on ANY overtake-mode change; the two
comment corrections; CORUN.md + API.md; and
`tests/host/test_suppress_master_volume.sh`, mutation-checked against all three
invariants including the exact bug that shipped. Finding 2a (asymmetric note-8
across a held touch) is **documented at the field, not fixed** — it needs a
synthesised release and device time.

**Correction — #291 and #292 do NOT conflict textually.** The cross-PR note above
says they do. After #292 landed, #291 re-reported `MERGEABLE` / `CLEAN`: the two
diffs touch adjacent hunks of the same loop but never the same lines. The
*semantic* half of that note still stands — both append a clause to a block whose
precedence is "last writer wins", which is what #309 makes explicit.

Everything in 1a/1b below was re-verified against `32a60f3a` and held. Three
things the file did not have:

- **#292 has a one-line general fix that has none of the holes**, and it is the
  main ask of that review. Only the SysEx **lead** packet is ever filtered — the
  continuation payloads (`1D`, `3A`, `00`) are all `< 0x80` — so
  `if (status >= 0x80 && !(cin >= 0x04 && cin <= 0x07)) filter = 1;` fixes the
  whole class with no lookahead, no run-length state, no slot-27 boundary and no
  id-byte question. Checked first: `overtake_suppress_sysex` gates the *outbound*
  LED path (`shadow_led_queue.c:508`) only, so there is no deliberate inbound
  cable-0 SysEx policy to preserve — `0xF0 >= 0x80` catching it is incidental.
- **#291's finding 1 is confirmed at the mechanism, not just the predicate.**
  `shadow_volume_knob_touched` is set from the *hardware* buffer (`:4861`,
  `:7404`), independent of the filter, so it still goes to 1 under suppression —
  which is what leaves `native_display_visible` true while the frame on screen is
  Schwung's. Also confirmed: `CORUN_GRP_MASTER` is CC 79 (`shadow_constants.h:233`)
  and `CORUN_GRP_TOUCH` covers note 8 (`:237`), so finding 3's co-run half is real.
- **#291 carries the flag across a 2 → 1 transition** (module back to the Tools
  menu); the clear only fires on `→ 0`, so the shadow menu's volume knob is dead
  until exit. New, folded into that review's finding 2.

Also confirmed and *not* worth re-raising: no null-deref risk in either PR (the
filter block is inside `if (shadow_display_mode && shadow_control)` at `:6610`),
#292's `j + 24 < SHADOW_MIDI_IN_BYTES` bound is exactly right, and both PRs are
`MERGEABLE`/`CLEAN` with all three checks green.

---

## Session 1a — PR #292, power button reaches Move's shutdown prompt

DimaDake, +47/-0, `src/schwung_shim.c` only.

Two changes: a lookahead that exempts the power-button SysEx's four packets from
the overtake filter's blanket `status >= 0x80 -> suppress`, and a `cable == 0x0E`
skip in the overtake module-dispatch loop.

### Verified sound

- **The cable-14 claim is correct.** The dispatch loop at `schwung_shim.c:7735`
  filters only on `cin` when `overtake_mode` is set; the non-overtake branch
  already had `if (cable != 0x00) continue`. So cable 14 genuinely did reach
  `onMidiMessageInternal`, and the new skip is a no-op outside overtake.
- **The bounds check is right.** `j + 24 < SHADOW_MIDI_IN_BYTES` guards the
  furthest read (`hw_midi[j + 24]`) and short-circuits ahead of it.
- **No other MIDI_IN filter site can corrupt the SysEx** — the Shift+Menu and
  sampler filters are both `cin == 0x0B`-only.

### Finding 1 — the comment is off by one; the CODE IS CORRECT

The first-pass review flagged this as a possible bug ("a short tap never matches
and its SysEx is still corrupted"). **That was wrong.** Resolved by the sibling
message:

`src/host/shadow_led_queue.c:908` documents the LED SysEx as
`F0 00 21 1D 01 01 3B 10 <idx> ... F7` — payload byte 6 is the **command**
(`0x3B`), byte 7 the subcommand. USB-MIDI packs SysEx three payload bytes per
packet, so byte 6 is packet 3's first payload byte, i.e. `j + 17` at an 8-byte
MIDI_IN stride. `0x3A` at `j + 17` is therefore the power **command**,
structurally identical to `0x3B` for LEDs, and the varying id sits at `j + 18`
where the code correctly does not match.

So the code is right and the comment ("The id byte at offset 17 varies") is off
by one. Fix the comment.

**One question still worth asking the author**, because it is cheap and the
answer decides which of us is off by one: they report observing `0x2A` on a tap
and `0x3A` on a hold. If that is the byte they were watching at `j+17`, their
offset is right and the match only ever succeeds on a hold — which is the only
case they device-verified. If it is `j+18`, everything above holds.

### Finding 2 — a straddled message still corrupts, intermittently

`power_sysex_remaining` is function-local and re-zeroed every SPI frame, and the
lookahead needs all four packets present in the frame it is scanning. A SysEx
starting past slot 27 (the lookahead needs `j + 24 < 248`) or split across two
frames under heavy surface traffic is filtered and corrupted exactly as before.
Low severity — MIDI_IN is near-empty when someone presses power — but it is a
latent intermittent recurrence of the bug being fixed, and worth a comment
acknowledging it if not a fix.

---

## Session 1b — PR #291, `shadow_set_overtake_suppress_master_volume(flag)`

DimaDake, +49/-7, `shadow_constants.h` / `schwung_shim.c` / `shadow_ui.c`.

New control-surface flag mirroring `overtake_suppress_sysex`. Excludes Move from
CC 79 and master-touch note 8, and gates the plain-volume-touch OLED handoff in
`shadow_swap_display()`. Default off, reset at shim init and on overtake exit.

### Verified sound

- **The struct still fits.** `CONTROL_BUFFER_SIZE` stays 84 and the field
  consumes existing trailing padding. Proven by CI, not by hand: the static
  assert at `shadow_constants.h:774` is compile-time and `cross-compile` passed.
- Both filter sites (mode 1 and mode 2) are gated, and the reset on overtake exit
  is more careful than `overtake_suppress_sysex`'s equivalent — correctly, since
  a stuck flag here breaks the volume knob for every module loaded afterwards.

### Finding 1 — `native_display_visible` was not updated (LEAD WITH THIS)

`schwung_shim.c:5638` computes:

```c
int native_display_visible = (!shadow_display_mode) ||
                             (shadow_display_mode &&
                              shadow_volume_knob_touched &&
                              !shadow_shift_held &&
                              shadow_control) || ...
```

That is the same condition `shadow_swap_display()` tests at `:3630`, which this
PR gates on the new flag. The comment directly above it states the coupling in
so many words:

> shadow_swap_display() hands the frame back to Move on plain volume touch in
> overtake too, so we scan the volume bar regardless of overtake_mode —
> otherwise audio scales to whatever volume was active when overtake engaged.

With suppression on, the shadow UI stays on screen but the volume-bar scanner
still runs — against a frame that is now **Schwung's own OLED output**, not
Move's overlay. A false bar match rewrites `shadow_master_volume` and jumps the
mailbox gain. This is the strongest finding in the whole batch: the tree already
documents the dependency that the PR breaks.

### Finding 2 — note 8 can be filtered asymmetrically across a touch

The flag can flip between note-on and note-off (touch the knob, then press the
track button — the movy gesture ordering). Move then never sees the release and
latches volume-touch state. The codebase already knows this failure mode:
`schwung_shim.c:6492` defines `vol_touch_off[4] = {0x08, 0x80, 8, 0}` among the
overtake-exit release injects, for exactly this reason. The new flag has no
equivalent compensation.

### Finding 3 — later unconditional `filter = 0` writes defeat the guard

In the mode-2 branch, both of these run *after* the new guards and set
`filter = 0` unconditionally:

- `if (cin == 0x0B && type == 0xB0 && d1 < 128 && overtake_passthrough_ccs[d1])`
- `if (corun_target(...) == CORUN_TARGET_MOVE_NATIVE && corun_event_owner(...) == CORUN_OWNER_PEER)`

So suppression is silently defeated for any tool that also declares CC 79 in
`capabilities.button_passthrough`, or that cedes the master group under co-run.
Narrower than it first looks (it takes one of those two opt-ins), but real — and
see the cross-PR note above, because #292 adds a third such write.

### Finding 4 — the rewritten `CONTROL_BUFFER_SIZE` comment loses the derivation

The diff changes `shadow_constants.h:55` from "corun masks widened to uint32 +
flags byte" to "overtake_suppress_master_volume added into existing trailing pad
byte". But 84 was **not** bumped for this field — it consumes padding. The real
derivation now survives only in the `overtake_suppress_sysex` block at :192.
Keep the original attribution.

---

## Session 2 — PR #281, string cell's keyboard returns to the grid

athousanddetails, +19/-1, `src/shadow/shadow_ui.js`.

### No correctness bug found

The reviewer traced the full flow (`openParamEditorFromGrid` ->
`openHierarchyParamEditor` string branch -> `openTextEntry` -> `onConfirm` /
`onCancel`) and every claim in the PR body checks out:

- `closeTextEntry()` (`src/shared/text_entry.mjs:295`) only clears its own state,
  and runs *after* the callback — so switching view from inside it is safe.
- `handleTextEntryMidi` consumes and returns (`shadow_ui.js:20106`), so nothing
  downstream re-handles Back after `onCancel` navigates.
- `exitParamPages` nulls the controller, so the enum-picker's stale-touch hazard
  (which needs `clearParamPagesTouch()`) does not apply here.
- The `paramEditorOpenedFromGrid` guard is correctly scoped — set only in
  `openParamEditorFromGrid`, cleared in `exitHierarchyEditor` — so a
  list-originated string edit still stays in the list.

### Finding 1 — it bypasses `closeOwnViewEditorToCaller()`, so the test cannot see it

`closeOwnViewEditorToCaller()` (`shadow_ui.js:2724`) is the documented single
return path, with three call sites (:2757, :12462, :14606), and
`tests/host/test_editor_returns_to_caller.sh` lifts and drives only the functions
that call it. This PR open-codes
`if (paramEditorOpenedFromGrid) returnToParamPagesFromEditor()` twice instead.

Consequence: **delete or invert either of the two new lines and CI still
passes** — precisely the regression the helper and its test exist to catch. The
helper did not exist on this branch's base, so this is a rebase ask, not a
criticism of the author.

Ask: rebase onto `closeOwnViewEditorToCaller()`, and extend
`test_editor_returns_to_caller.sh` with a `string` case (it currently drives the
filepath browser, the canvas view, and the edit-mode toggle).

**Why the test cannot see it, precisely** — it *does* lift the whole of
`openHierarchyParamEditor`, so the new lines are inside the lifted body. But
`runEditModeClick` drives it with `hierEditorEditMode: true` and
`forceOpen: false`, which takes the toggle branch and `return`s before the
string branch is ever reached. The undeclared-identifier trick that makes that
case fail loudly protects the toggle only. `closeOwnViewEditorToCaller()` is a
drop-in for both new lines — it is `if (!paramEditorOpenedFromGrid) return
false; returnToParamPagesFromEditor(); return true;` and neither callback uses
the return value.

Two things the second pass checked and found clean, so they need not be
re-examined: `refreshHierarchyVisibility()` runs on state about to be torn down
but costs no IPC (`applyHierarchyVisibilityFilters` + a cache invalidation), and
returning straight after a fire-and-forget `setSlotParam` is the shape the
filepath door already has — `restorePage`'s remembered one-shot restore
(`page_controller.mjs`, the granny comment) covers the case where the contract
re-read has not settled. No new exposure.

### Finding 2 — WITHDRAWN: the screen-reader clobber cannot happen here

The first pass raised this as a double `host_send_screenreader` in one tick:
`announceParameter` at :2219, then `returnToParamPagesFromEditor` ->
`enterParamPages` -> `controller.load()` -> `announcePageChange()`
(`page_controller.mjs:840`). **The mechanism is real and the exposure is not.**

Mechanism confirmed: `announce()` (`screen_reader.mjs:13`) has no queue, and
`js_host_send_screenreader` (`shadow_ui.c:1878`) `strncpy`s into a single SHM
slot and bumps `sequence` — the second call overwrites the first outright.

But **a TTS user is never on a component knob grid**, so the second utterance is
never reached from this path. `paramPagesEnabled()`
(`shadow_ui_param_pages.mjs:142`) is `if (tts_get_enabled()) return false`, and
all four entries into the grid are gated on it (`shadow_ui.js:10843`, `:12049`,
`:12123`, `:15692`). `paramEditorOpenedFromGrid` is set in exactly one place,
`openParamEditorFromGrid` (:2971), which is only reachable from the grid — and
which returns early for the two ungated synthesised contracts (`slot`,
`master_settings`) before any string branch. So with TTS on, both new lines are
dead; with TTS off, `host_send_screenreader` writes an SHM slot nobody speaks.

Do not re-raise. (The `global_settings` contract is *not* gated on
`paramPagesEnabled` — deliberately, :8926 — and would fall through
`openParamEditorFromGrid` into `enterHierarchyEditor(0, "global_settings")`.
That is pre-existing, declares no divable string today, and is not #281's.)

---

## Session 3 — PR #210, test-bus waits on overtake DSP readiness

timncox, +253/-4, `tools/pytest-schwung/` only. Follows up your note in #190.

## Session 3 — DONE 2026-08-27. #210 MERGED, findings fixed in-house as #310

- **#210 merged** as `05e14028` (squash, branch deleted). It is inert: nothing
  in the tree calls `wait_for_overtake_dsp` or even `set_open_tool`, and the
  pytest-schwung suite is not a CI gate — so merging changed no behaviour and
  the three findings were fixable before the first caller existed.
- **The findings were fixed by us, not routed back to timncox.** The first-pass
  recommendation was to hold the PR because "the author is well-placed to make
  them". That was wrong, and the correction is worth keeping: **the fixes
  depended on tracing only this review had done** — that `unloadModuleUi()`
  never touches the overtake mode, and that `param GET error from peer` sits
  outside `_PARAM_TRANSIENT_ERRORS`. Handing three findings back meant the
  author re-deriving that from a comment, over days, to produce changes we
  could write immediately. Blast radius, not authorship, is what should decide.
- **#310** — all three fixed, 32 → 37 tests, CI green on all three checks.

One thing #310 does NOT close, recorded so it is not rediscovered as a bug: a
module with **no `dsp.so`** never drives `__ready` to `"0"`, so on a *reload* it
is indistinguishable from a stale mode. It waits out a bounded settle and
proceeds, degrading to the pre-fix behaviour rather than hanging. The airtight
signal is shadow_ui auto-clearing `shadow_control.open_tool_cmd` when it picks
the command up (`shadow_ui.c:214`) — exactly the edge — but the daemon exposes
no way to read that byte back, so closing it is a C/device change, not a
harness one.

**REVIEWED AND POSTED 2026-08-27.** All three findings were reproduced by
driving the real `wait_for_overtake_dsp` through the PR's own `make_bus`
helper on a checkout of the branch — not inferred from the diff. `pytest
tools/pytest-schwung/tests` is 32 passed on the branch. The line numbers the
PR body cites (`shadow_ui.js:3635` / `:3685`) are stale; the current tree has
`shadow_set_overtake_mode(2)` at `:6454` and `loadOvertakeDsp(dspPath)` at
`:6504`, 50 lines apart in the same synchronous call, so the claim holds.

Adds `SchwungBus.wait_for_overtake_dsp()`, gating on `overtake_mode == 2` *then*
`overtake_dsp:__ready`. The rationale is correct: `__ready` answers `"1"` whenever
nothing is loading, which includes the window before the load is requested
(`loadOvertakeModule` sets the mode at `shadow_ui.js:3635` and requests the load
at `:3685`), so a bare `__ready` poll passes against the previous module.

### Finding 1 — gate 2 swallows every bus error as "old host" (LEAD WITH THIS)

```py
try:
    ready = self.get_param("__ready")
except SchwungBusError:
    # Unknown param => host predates __ready.
    return
```

`_param_request_with_retry` (`client.py:551`) retries three times on
`_PARAM_TRANSIENT_ERRORS` — `"param SHM busy"`, `"param GET timeout"` — and then
**re-raises `SchwungBusError`**. Its own comment says the contention comes from
"shadow_ui mid-call", and the load window is exactly when `/schwung-param` is
most contended (shim worker doing `dlopen()`, shadow_ui mid-tick).

So a ~400 ms contention burst is misread as "host predates `__ready`", the helper
returns immediately, and the test presses a pad into the shim's missing-instance
guards — **the exact lost-first-press failure this PR exists to prevent, now
silent.**

Note gate 1 catches the same class and *keeps polling*. The asymmetry is visible
on the diff and is the tell.

Fix: discriminate on the message (the daemon's genuine old-host case is
`param GET error from peer` / error 14) rather than catching the base class.

**Verified, and the discrimination is clean.** `commands.c:637` emits
`"param GET error from peer"` when the shim sets `shadow_param->error = 14`
(`schwung_shim.c:4185`, the no-handler-claimed-the-key branch). That string is
**not** in `_PARAM_TRANSIENT_ERRORS`, so the old-host case propagates on the
first attempt with no retry delay while contention burns three retries first —
the two are distinguishable on both message and latency. Reproduced with
`make_bus(modes=[2], readys=[SchwungBusError("param GET timeout")])`: returns
"ready" after one poll.

### Finding 2 — both gates share one deadline

`deadline` is computed once (line 77) and both loops test it. If the mode flips
near the deadline — a slow shadow_ui load, which is the normal case here — gate
2's body never executes, no `__ready` GET is ever issued, and the helper raises
*"DSP still loading after 10s (overtake_dsp:__ready=None)"*, blaming the module's
`dsp.so` for pure budget exhaustion. Either give gate 2 its own timeout, or make
the message distinguish `ready is None` (never polled) from a real `"0"` stall.

**Verified** by giving `state()` a 90 ms round-trip against a 200 ms timeout:
raises with `__ready=None` and `ready-polls = 0`, i.e. gate 2's body never ran.

### Finding 3 — gate 1 cannot detect a transition

It only tests `mode == OVERTAKE_MODULE`, with no pre-read. If a module->module
switch keeps the mode at 2 throughout, gate 1 passes on the first poll against
the stale mode, `__ready` still reads `"1"`, and the helper returns before
anything has happened.

**VERIFIED — it does NOT evaporate.** `set_open_tool` lands at the tick handler
(`shadow_ui.js:19331`), which does `unloadModuleUi()` then `loadOvertakeModule(ot)`
(`:19363`). `unloadModuleUi()` (`:4566`) clears the UI refs and param shims and
**never touches the overtake mode** — none of the seven `shadow_set_overtake_mode(0)`
call sites is on this path. And both statements sit in one synchronous tick, so
even a reset would be externally unobservable. That block is after the tick's only
early returns (splash, analytics prompt, upgrade overlay), so it is reached with a
module already running — which is what its own fallback comment exists for.

Reproduced: `make_bus(modes=[2], readys=["1"])` returns after `state-polls = 1`,
`ready-polls = 1`. Reachable in practice because `fresh_move` is documented as
skippable ("3 s reset × N tests adds up in CI"), so a file opening tool A then
tool B without it hits this.

---

## Session 4 — PR #221, persist Master FX from the shim, not the in-file mirror

## Session 4 — DONE 2026-08-27. #221 MERGED, findings 1-5 fixed in-house as #311

- **#221 merged** as `29eb4bd4` (squash). It fixes user-reported data loss and
  the shim genuinely is the authority; the remaining findings were about cost
  and about invariants the sibling sites carry, none of which is a reason to sit
  on the fix. Verified before merging: merges clean against the moved `main`
  (which had grown the whole `writeChainShape` / `MASTER_CHAIN_TARGET` path
  through `applyMasterFxModuleSelection` since this PR's base), and 173/173
  `tests/host/*.sh` plus the C units pass on the merged result.
- **Findings 1-5 fixed as #311**, not routed back to the author — they land in
  core autosave and param-service machinery rather than in his patch, and
  finding 1's answer is a new shim param.
  - `master_fx:modules` (GET only) answers with the whole chain in one string,
    so the saver pays 1 IPC read instead of 8+N. Builder is header-only
    (`src/host/master_fx_snapshot.h`) and host-tested: positional (never
    compacted), refuses rather than truncates, escapes quotes/backslashes.
  - That also makes the id and the path ONE fact, which is finding 4.
  - Plus: an id with no path preserves the file instead of blanking it, the
    adopt invalidates the display-name caches, and `masterFxModuleWriteAt` +
    `CONTRACT_SETTLE_MS` stops a fire-and-forget overtake write being read back
    over. The per-position reads stay as the version-skew fallback.
  - Test moved to `tests/host/` and now DRIVES `saveMasterFxChainConfig` through
    a fixed dependency list. Mutation-checked; the first cut passed with the
    cache invalidation deleted (it seeded caches only for positions the mirror
    knew about, and the reported case has an empty mirror).

**Correction — the cost suggestion in finding 1 below has a HOLE, and it was
posted to the author before it was caught.** "Ask the shim only where the write
would be destructive (mirror empty AND the file is not already `{}`)" protects
against loss but not against persistence: a tool loads a module while the file
is already `{}`, the read is skipped, and the chain still never saves. Withdrawn
in a follow-up comment on #221. The answer that works is reducing the read COUNT,
not the read's frequency.

Noted in passing, not changed (out of scope, and other sessions are editing that
file): **CLAUDE.md's "Master FX still has no insert, remove or move" is stale.**
`applyMasterFxModuleSelection` calls `writeChainShape(MASTER_CHAIN_TARGET, …)`
and the master_fx param dispatcher handles `fx:insert` / `fx:remove` / `fx:move`
under `!has_slot_prefix`.

**Second pass, 2026-08-27 (session 4).** Re-verified against `32a60f3a`. The PR
still **merges clean** (`git merge-tree`) despite main having moved under it —
its base is `eef4e969` and `applyMasterFxModuleSelection` has since grown the
whole `writeChainShape` / `MASTER_CHAIN_TARGET` permutation path, which does not
touch the hunks. All three CI checks green. Findings 1–5 below hold; corrections
and sharpenings are marked inline.

DimaDake, +87/-1 (the tree reports +44/-1 in `shadow_ui.js` plus a 43-line test).
**Fixes a real, user-reported data-loss bug** (two Discord
reports, tracked as DimaDake/schwung-movy#9): a master module loaded by writing
`master_fx:fxN:module` straight to the shim — how an overtake tool does it — is
invisible to `masterFxConfig`, so the saver takes its empty branch, writes `{}`,
and the whole master chain is gone on the next boot.

The approach is right and the `null` vs `""` distinction is correctly respected
(`masterFxShimValue` returns null on a failed read, and only a real answer is
adopted). The issues are about cost and about invariants the sibling sites carry.

### Finding 1 — the read cost lands on the one tick that was fixed for this

The PR body says "four param reads". It is **eight**: `MASTER_FX_SLOTS` is 8
(`shadow_chain_mgmt.h:29`, `shadow_ui.js:1719`), the `:name` read is
unconditional per slot, plus a `:module` read per loaded slot.

Worse, `saveMasterFxChainConfig()` **is autosave job 5** (`shadow_ui.js:19438`) —
the tick the one-slot-per-tick split was created to protect. That comment
(:19412) measures the original:

> It used to do all four slots and the master FX chain in a single tick.
> Measured on device that was ~70 sequential IPC reads landing on one frame —
> ~200ms, i.e. about eleven dropped frames, every five seconds. It is the visible
> hitch while nothing is being touched.

At ~2.8 ms per read, 8+ reads is ~25-45 ms added to that frame. There are 18
call sites total, many interactive.

**Second pass — counted exactly.** New reads per save = **8** (`:name`, one per
slot, unconditional) **+ 1 per loaded slot** (`:module`). Today an EMPTY slot
costs zero reads and a loaded one costs `:state` (+ `chain_params` fan-out when
there is no state) + `:bypassed`; the typical device has 0–2 loaded, so the save
goes from roughly 2–8 reads to 10–18. That is **~22–28 ms added**, not 25–45 —
still one to two dropped frames every five seconds, on the frame the
one-slot-per-tick split exists to keep clean.

**A cheaper shape worth putting to the author, because a round-robin does not
work.** Spreading the `:name` reads over successive autosave passes converges
eventually but leaves the destructive write in place meanwhile — an unreconciled
loaded slot still takes the empty branch and `{}` still lands on its state file,
so a reboot in that window still loses the chain. The invariant the PR body
itself identifies is the lever: **the empty branch has no `snapshotOk` guard.**
Ask the shim only where the write would be destructive — i.e. when the mirror
says empty *and* the existing `master_fx_<N>.json` is not already `{}`. On a
device with nothing loaded in Master FX (the common case) that is zero IPC
reads, and it still cannot erase a slot the shim has.

**And it cannot be scoped to "after an overtake session".** Drift is not
overtake-only: `handleSetMasterFxParam` (`schwung-manager/remote_ui.go:1395`)
forwards **any** key to `setParam(0, ...)` with no allowlist, so a Remote UI
websocket client can write `master_fx:fx1:module` out-of-process while the
shadow UI sits idle in normal mode.

### Finding 2 — the adopt skips the cache invalidation every sibling site does

Both `loadMasterFxChainConfig` (:9048-9052) and `clearMasterFx` (:7689-7693)
follow a `masterFxConfig[key].module = ...` with:

```js
delete fxDisplayNameCache[`master:${key}`];
delete fxDisplayNameSkip[`master:${key}`];
delete fxDisplayNameBackoff[`master:${key}`];
```

and a comment explaining why ("Different module — it may implement display_name
even if the last one didn't"). The new adopt at :9181 does none of it and sets no
`needsRedraw`, so the newly adopted module keeps announcing and labelling as the
previous one.

### Finding 3 — the save can adopt pre-write state under overtake/co-run

`shadow_set_param` is fire-and-forget under overtake (`shadow_ui.c:1016`), and
`saveMasterFxChainConfig()` runs in the same tick as the module write (:9210,
:9215, :7686-7696, :7714-7778). The new read can therefore observe the state
*before* the write lands and adopt it — silently reverting the module the user
just picked, in both the mirror and the state file. Same class of bug as the
"timed-out read must not empty a chain position" work in #298.

**Second pass — real, but narrower than written, and say so to the author.**
`shadow_set_param_common` is fire-and-forget *only* at `overtake_mode >= 2`
(`shadow_ui.c:1014`); everywhere else it is a blocking round-trip, and the
master-FX load is performed inline by the param service
(`shadow_master_fx_slot_load_with_config`, `shadow_chain_mgmt.c:987`, which sets
`module_id` at :1058 after `create_instance` returns). So the ordinary picker
path — `setMasterFxSlotModule` then `saveMasterFxChainConfig` in the same tick —
cannot observe pre-write state. And the periodic autosave **abandons itself
under overtake** (`autosaveJob = null` when `isOvertakeActive`,
`shadow_ui.js:19434`), so job 5 never fires during a takeover either. What is
left is a save reached from an overtake tool through the published `ctx` — which
is exactly what Movy does today — where the adopt reads the *pre-write* id and
writes it back over a mirror the tool has just updated. Worth a guard, not worth
blocking on.

Related and worth affirming rather than filing: `loadMasterFxChainConfig` uses
`getMasterFxSlotModule`, which collapses `null` into `""` (:9046-9047), so a
timeout there zeroes the mirror. With this PR the saver re-reads the shim and
adopts the truth back, so the PR **mitigates** a pre-existing hazard as a side
effect.

### Finding 4 — `shimId` and the DSP path are independent round-trips

If the `:name` read fails and `:module` lands, the file gets the mirror's id
paired with the *previously loaded* module's path. The shim's boot restore is by
path, so the wrong module comes back. They need to be read as a unit, or the
fallback needs to refuse a mismatched pair.

**Second pass — confirmed, with the mechanism nailed down.** The pair is written
into `master_fx_<N>.json` as `{module_path, module_id}` (`shadow_ui.js:9341`),
and the boot restore parses **`module_path` only** and skips the slot when it is
absent (`shadow_chain_mgmt.c:1567-1590`) — `module_id` is never consulted. So the
mismatch loads the module the *path* names while every JS surface labels it with
the *id*. Requires exactly one of the two consecutive reads to fail, so it is
rare, but it is silent when it happens.

### Finding 5 — the test is not CI-gated and does not run from anywhere

`tests/shadow/test_master_fx_save_reads_shim.sh`:

- lives in `tests/shadow`, which CI does not run (CLAUDE.md, Testing section)
- only passes when invoked from the repo root
- pins exact source strings, so any refactor fails it

Ask for it in `tests/host/` with the `cd`.

**Second pass — one clause here was WRONG; do not repeat it to the author.**
"missing the house `cd` that every other file in that directory opens with" is
false: only **16 of 62** `tests/shadow/*.sh` carry
`cd "$(dirname "$0")/../.."`, and the two sibling master-FX tests the PR body
cites do not. The author matched the majority style. (The 0-vs-16 disagreement
came from `grep` being a wrapper here that swallows output — see
[[grep_is_wrapped_and_swallows_output]]; `rg` gives 16.) Verified by running it:
green from the repo root, `exit 1` with an `rg: No such file` from anywhere else.
The substantive half stands — **CI runs `tests/host/*.sh` only**
(`.github/workflows/ci.yml:41`), so as filed this test never executes, which is
the same "a probe that cannot fail reports green" shape as #281 finding 1.

### Corrections to the first-pass review — do not re-raise these

- **"Adopting `null` could still pass the test" is FALSE.** The test pins the
  literal string `if \(shimId !== null && ...\)`. Adopting null would fail it.
- **The dropped `?.` in `masterFxConfig[key].module = shimId` is a NON-ISSUE.**
  `makeEmptyMasterFxConfig()` populates `fx1..fx8` unconditionally (:2612-2618),
  and both sibling sites (:9047, :7689) also write plain `.module =`. The PR
  matches house style.
- **"MASTER_FX_OPTIONS is scanned once at startup" is very slightly
  overstated** — it is also rescanned after a store install reached from the
  Master FX picker (:9852) and at :18380. But **schwung-manager is the single
  install path now and it does not restart `shadow_ui`**, so a module installed
  over the web really is absent from the list until a restart. The PR's second
  fix (prefer the shim's own DSP path) is therefore worth having on its own, and
  the reason it only ever bites via a direct shim write is that a module missing
  from `MASTER_FX_OPTIONS` cannot be picked in the UI at all.

---

## Session 5a — PR #293, fix overtake DSP MIDI injection into Move

lukeco11, +234/-75, 13 files. **Fixes a regression from #190**: overtake DSPs'
`midi_inject_to_move` still pointed at the shared `/schwung-midi-inject` ring,
which the overtake publisher now consumes — so generated MIDI loops back into the
takeover instead of reaching Move, and only drains after exit. Visible as Chord
Finder being silent until Back. Adds a dedicated bounded MPSC ring
(`src/host/shadow_overtake_midi.c`) and repoints `schwung_shim.c:1692`.

Device-validated by the author on 0.12.1 with Chord Finder 0.4.3.

**Session 5 verification pass, 2026-08-27.** Both PRs re-checked against
`32a60f3a` and against their own branches. Both are green locally:
`make -C tests/host test` plus all 176 `tests/host/*.sh`, 0 failures, on
`pull/293/head` and `pull/307/head` alike. **#307 has no CI runs on its branch
at all** (`gh pr checks 307` → "no checks reported"), so that local run is the
only evidence it has; #293's three checks pass on GitHub.

### Finding 0 — DISPROVEN on hardware 2026-08-28; the guard does not stall external MIDI

**RESOLVED, and I was wrong — recorded rather than deleted, because every fact
that made it convincing is still true and the next reader will re-derive it.**

Tested with Chord Finder on `7291a6d7` (v0.12.1 + #312/#313): pads, sustained
mod wheel, sustained pitch bend. **205 drains, 3 packets each, no dropouts, no
aborts.** The test was valid — the keyboard's notes reaching Move's own
instrument proves cable-2 events really were in the mailbox MIDI_IN, which is
exactly the buffer the guard scans. The guard saw the traffic; the drain kept
running.

What the analysis missed is the **RATE**. Move consumes MIDI_IN every frame, so
holding `hw_cable_active` true needs a packet in essentially every 2.9 ms frame
— ~340/second, sustained. A held note is ONE packet; a wheel sweep is 100-200
per second. Both leave the two clear frames the counter needs, with room.

Worse: **the repro I specified could not exercise it at all.** I asked for a
held key, which is a single note-on — so the first hardware answer ("no drops")
was not evidence either way, and it took a second, rate-based test to get a real
one. A wrong repro that returns the right verdict is still a broken probe; see
[[probes_that_measured_the_wrong_thing]].

The lesson, since this is a guard read as a bug from its denial condition alone:
**a guard is not a bug because it CAN deny — it is a bug when something real
makes it deny. Compute the rate before writing the warning.** Comment corrected
in #318.

<details><summary>The original finding, as written</summary>


Not in the first pass, and it is ahead of everything below because it can make
the feature not work at all in the case it was built for.

Removing the `if (sc->overtake_mode) return;` early exit does not just let the
dedicated ring drain — it lets the *whole rest of the function* run during
overtake, and two guards sit between the top of the function and the drain
call. The second one (`shadow_midi.c`, `DEFER_FRAMES`) is:

```c
for (int j = 0; j < MIDI_IN_MAX_BYTES; j += MIDI_IN_EVT_STRIDE)
    if (midi_in_scan[j] != 0) { hw_cable_active = 1; break; }
if (hw_cable_active) { defer_counter = 0; return; }
if (defer_counter < DEFER_FRAMES) { defer_counter++; return; }
```

So the dedicated ring only drains after **three consecutive frames with an
entirely empty mailbox MIDI_IN**, and any non-zero slot on *any cable* resets
the counter to zero.

Move's own surface is filtered out of the mailbox during overtake, which is
why the author's Chord Finder test passes — driven from Move's pads, MIDI_IN
stays empty. But cable 2 is **not** filtered in overtake ("Overtake: all
cables forwarded", CLAUDE.md), so playing an external USB keyboard into an
overtake module puts a note event in MIDI_IN on essentially every frame you
are playing — and the ring stalls for exactly as long as you play. That is the
Chord Finder use case.

I have not measured this on device; the guard is unambiguous but whether
cable-2 events actually persist in the *mailbox* MIDI_IN through an overtake
frame is the one link I could not settle from source. **It is a cheap check
and it should be made before this merges**: hold a note on a USB keyboard and
see whether the DSP's injected output still reaches Move.

Note the pre-existing shim path deliberately dodges this —
`shadow_deliver_pending_to_move` has its own all-slots-empty test and is
called *before* both guards, precisely so it "must still reach Move while an
overtake module is up". The new ring wants the same treatment, or an
overtake-specific relaxation of the defer rule.

</details>

### Finding 0b — a hold that has been dead code since #190 comes back to life

The doc's earlier "not verified" note about hoisting `sc` is **confirmed, and
it is not about `sc` at all** — it is about the early return being deleted.

`prev_overtake_for_hold` is updated *inside* the exit-hold block, which the
early return made unreachable during overtake. So `prev_overtake_for_hold` was
permanently 0, `exit_hold_frames` was never armed, and the whole
`OVERTAKE_EXIT_HOLD_FRAMES` block has been dead since the return was added.
With the return gone it runs every frame, latches non-zero during overtake,
and arms a real 3-frame hold on the overtake→0 transition.

This is the **safe** direction (it holds more, and it restores what the
comment says the block is for), so it is not a defect — but it is an
unremarked, unvalidated behaviour change, and it compounds finding 3: the
overtake-exit releases are now delayed by the hold *and* queued behind the
dedicated ring.

### Finding 1 — nothing flushes the dedicated ring, and there is a template for it

`shadow_overtake_midi.c` has `init`, `send` and `drain` — no clear.
`shadow_overtake_midi_init()` is called once from `midi_routing_init()` at shim
startup, and nothing touches the ring when an overtake module exits.

The codebase already solved this for the sibling ring —
`schwung_shim.c:1799`, in the DSP unload path:

> Discard any ROUTE_EXTERNAL packets the unloaded DSP left in the ring. Without
> this, the next overtake load would drain the previous module's leftover packets
> into Move's MIDI_OUT region — up to 64 stray events shipped to USB-A across the
> first ~4 audio blocks after load. The producer (destroyed instance) can no
> longer fire, so the ring is inert here and a non-atomic reset is safe.

That is the template, and the same argument applies verbatim.

**Correction to the first-pass framing:** the review described this as causing a
stuck note (DSP pushes note-on, user exits before note-off). A flush alone would
*not* fix that — during the takeover packets now drain to Move immediately, so
the note-on has already gone out. The complete fix is an all-notes-off on the
overtake->0 transition, with the discard for whatever is still queued.

### Finding 2 — `drain_ring` is bounded by a count, not by the buffer

```c
uint8_t *slot = &midi_in[copied * MIDI_IN_EVENT_STRIDE];
```

bounded only by the caller-supplied `max_events`, with no buffer-length
parameter. The sole caller passes 31, so it is safe today. But CLAUDE.md states
the rule flatly — "Bound every 8-stride walk with `SHADOW_MIDI_IN_BYTES`" —
because the RX display-status word sits at +248 and this is how it gets
clobbered. Pass the byte length, or clamp to `SHADOW_MIDI_IN_BYTES / 8`. The
local `#define MIDI_IN_EVENT_STRIDE 8` is a third copy of that constant.

**Verified: safe today, and the arithmetic is worth stating so nobody re-raises
it as a live overflow.** `copied < max_events` with `max_events == 31` caps
`copied` at 30, so the furthest write is `midi_in[240..247]` — inside the 248
bytes. The finding is about the missing *bound*, not a present overrun.

**Also verified: the `saw_existing` guard is not weakened.** The old loop
scanned forward for an empty slot and bailed if it had passed any occupied one;
`drain_ring` instead breaks when its target slot `midi_in[copied * 8]` is
non-empty. Those are equivalent here, because the two guards above the drain
already guarantee every slot is zero, and both walks are monotonic — the new
form cannot inject past a pre-existing event any more than the old one could.
No finding; recorded because it looks like a dropped safety check on the diff.

### Finding 2b — the PR deletes a device measurement instead of narrowing it

The removed block does not only contain the early return. It carries:

> An injected packet written here could therefore never reach the module, and
> with the firmware suppressed during overtake it had nowhere useful to go
> either. Measured on device 2026-07-29: injected pads moved neither a
> parameter nor any of the 32 pad LEDs.

That measurement is about **cable-0 pads**, which cannot reach track
instruments at all (Move's prefix protocol), so it does not contradict the
author's Chord Finder result on pitched cable-2 notes — the two are about
different traffic. But deleting it outright removes the only record of *why*
someone added the early return, which is what makes it likely to be added back.
Narrow it to cable 0 and keep it, next to the new drain.

### Finding 3 — priority inversion at overtake exit

`shadow_overtake_midi_drain` drains the dedicated ring first (up to 64 packets),
then the shared ring only when `!overtake_active`. The shim's overtake-exit
releases — `shift_off`, `vol_touch_off`, `back_off`, `jog_click_off` at
`schwung_shim.c:6491-6503`, commented as "the highest-consequence injects: a
dropped release leaves Move believing a control is still held" — go to the
*shared* ring, and are now queued behind whatever the departing DSP left behind
(31 packets max per frame). Draining the shared ring first when `!overtake_active`
preserves the old priority.

**Verified, and bounded tighter than "up to 64".** `SHADOW_MIDI_INJECT_SLOTS`
is 64 (`shadow_constants.h:576`), so a full dedicated ring is 64 packets — three
frames at 31/frame, on top of the 3-frame exit hold that finding 0b brings back
to life. So the worst case for a release inject is ~6 frames (~17 ms), not
indefinite. Low severity on its own; it is the *combination* with 0b that is
worth naming to the author, since neither is visible from the other's diff.

### Finding 4 — scope gap: the chain path still loops back

`shadow_chain_mgmt.c:1362` still wires
`shadow_host_api.midi_inject_to_move = shadow_chain_midi_inject` — the shared
ring. Chain slots keep running during a takeover, so a chain MIDI FX in Pre mode
injecting while an overtake module is up has its packets popped by the overtake
publisher (`schwung_shim.c:7976`) and fed into the overtake module as if they
were hardware presses. Same bug class this PR fixes for overtake DSPs, left in
place for the chain path. Either route it too, or document the remaining
exposure.

### Finding 5 — stale load-bearing comment

`schwung_shim.c:7959` still asserts "`shadow_drain_midi_inject` returns early
while `overtake_mode` is set, so the ring keeps its single consumer." After this
PR the function no longer returns early — it selectively skips only the shared
ring. The invariant still holds; the sentence a future reader would verify it
against is now false.

### Finding 6 — a constant printed as if it were a measurement

The debug log now hard-codes `0` into a format string still reading
`"drained %d pkts at offset %d"`. The starting offset was the useful half — it is
what distinguished the safe offset-0 injects from the SIGABRT-inducing non-zero
ones. Return the offset or drop the field.

**Verified with a wrinkle in the PR's favour**: the constant is now *honest* —
`drain_ring` always starts at index 0, so the offset genuinely is 0 every time.
The problem is that a reader cannot tell that from the call site, and the field
still reads like a measurement. Drop the field; do not "fix" it by returning
the offset, because the value it used to distinguish no longer varies.

### Finding 7 — the ring test's oldest packet carries the same value as the memset

`test_full_queue_preserves_fifo` pushes `packet[2] = i` for `i` in `0..63`, then
asserts `midi_in[2] == 0` for "ring overflow does not disturb the oldest
packet". The expected value is also the value `memset(midi_in, 0, ...)` leaves
behind, so on its own that check cannot fail. It is rescued by the
`drain(...) == 1` assertion immediately above it, and it *does* discriminate
FIFO-vs-LIFO (a LIFO drain gives 63). Not a broken probe, but one detent away
from being one — seeding the first packet with a non-zero note makes it a real
assertion for free.

### Verified sound (#293), beyond the PR body

- `shadow_overtake_midi_send` and `shadow_overtake_midi_drain` are both on the
  SPI callback thread, so the in-process ring is genuinely single-producer /
  single-consumer and needs no more synchronisation than it has.
- Deleting `if (!inject_shm) return;` is a real improvement, not an oversight:
  `drain_ring` handles a NULL `shared`, so the dedicated ring now drains on a
  host with no inject SHM, where previously nothing did.
- The overtake publisher (`schwung_shim.c:7976`) and the shared-ring drain stay
  mutually exclusive — both read the same `sc->overtake_mode` — so the
  documented single-consumer invariant on `/schwung-midi-inject` holds.

---

## Session 5b — PR #307, chain knobs emit CC 102-109

jeffclementson-cloud, +566/-11, 13 files. From your Discord thread. Opt-in per
patch via `knob_cc_out`, off by default, UI at Slot Settings > Knobs. Author has a
Roto-Control to test against.

### The diagnosis is exactly right

`shadow_inprocess_load_chain()` (`shadow_chain_mgmt.c:1352`) memsets
`shadow_host_api` and then assigns eleven fields — `midi_send_external` is not
among them, so the chain DSP has always seen NULL. `overtake_host_api` has had it
all along (`schwung_shim.c:1689`). Verified.

Also verified sound: the emit path is allocation/lock/IO-free, `knob_find_param`
is a cached lookup, the `-1`/`-2` recv-channel guards match
`shadow_chain_slot_recv_channel`'s contract, the CC-resolution round trip is a
true inverse of the inbound scaling, and the new `knob_cc_out` branches sit ahead
of the generic `knob_` handler in both param dispatchers.

The "inbound absolute CC is deliberately not echoed" decision is right and the
test pinning it is the correct instinct — a later refactor would absolutely
"fix" it into symmetry.

### Finding 1 — a dropped packet does not self-heal for the packet that matters

Emits are event-driven with no reconciliation. The claim that a drop self-heals
holds only if the knob moves again — and the packet most likely to be dropped
under ring pressure is the **final** CC of a fast sweep, which is the one the
motor must land on. That is the exact staleness the feature exists to remove.
Needs either a periodic reconcile or a guaranteed-final emit.

### Finding 2 — it can silently eat shadow-UI MIDI out (LEAD WITH THIS)

ROUTE_EXTERNAL now has producers in **normal shadow mode**, not just overtake.
Its drain runs at `schwung_shim.c:5992`; `shadow_inject_ui_midi_out()` runs at
`:6090`. And that function is lossy by construction — `shadow_midi.c:494`
memsets the source SHM buffer *before* the copy loop:

```c
midi_out_shm->write_idx = 0;
memset(midi_out_shm->buffer, 0, SHADOW_MIDI_OUT_BUFFER_SIZE);
...
if (hw_offset >= HW_MIDI_OUT_SIZE) break;  /* Buffer full */
```

so anything that does not fit is **gone, not delayed**. A knob-sweep or
patch-load burst filling MIDI_OUT's 80 bytes therefore silently loses shadow-UI
MIDI out for that frame. See the cross-PR note — #293 adds a third producer to
the same contention.

**Verified, and worse than the line numbers suggest.** In *overtake* mode
`shadow_clear_move_leds_if_overtake()` runs first and frees the region, which is
the regime this ring was designed and measured in. In **normal shadow mode there
is no such clear** — Move's own MIDI_OUT traffic already occupies slots when the
drain arrives, so the ring is competing for what is left of 20 slots rather than
for a fresh 20. The drain's own bound is per-block (`if (slot >= 80) break;` —
leaves the rest in the ring, so ROUTE_EXTERNAL itself is only *delayed*); it is
`shadow_inject_ui_midi_out`, running 100 lines later, that **drops** its overflow.
The LED queue flush is a fourth claimant on the same 80 bytes.

**The cheapest mitigation is already in the tree and unused**: `overtake_ext_drops`
is incremented on ring-full and the comment notes it has "no get_param binding
yet". If this lands, bind it — a feature whose failure mode is a silently
stale motor should not also be silent about its drops.

### Finding 2b — the non-atomic ring reset now has a live producer

`shadow_overtake_dsp_unload()` (`schwung_shim.c:1799`) zeroes
`overtake_ext_ring.head/tail` and justifies the non-atomic reset in so many
words:

> The producer (destroyed instance) can no longer fire, so the ring is inert
> here and a non-atomic reset is safe.

After this PR the chain is a **second** producer of that ring, and it is not
destroyed when an overtake module unloads. No tearing results (both run on the
SPI callback), so the reset is still *safe* — but its stated reason is now
false, and the reset silently discards any chain knob CCs queued at that moment.
Directly in #293's blast radius, since #293's finding 1 asks for a matching
flush on the sibling ring.

### Finding 2c — every chain sub-plugin gains a capability it never had

`chain_host.c:82` does `memcpy(&inst->subplugin_host_api, g_host, sizeof(...))`
and overrides only the four mod/clock fields, so `midi_send_external` is handed
straight down to every synth, audio FX and MIDI FX loaded in a chain slot. It
has been NULL there since the chain existed, and every module guards on NULL, so
today it is inert everywhere — **I grepped all 70-odd fleet repos and exactly
one module calls it, `schwung-fourtrack`, which is `component_type: utility` and
therefore never loads in a chain slot.** So there is no live behaviour change.

It is still worth a sentence in the PR: this is a silent widening of the chain
sub-plugin contract, into the same contended mailbox as finding 2, and the next
chainable module that calls it will start emitting with nobody having decided
that it should. `plugin_api_v1.h` should say what `midi_send_external` means
inside a chain slot.

### Finding 3 — remapping a knob publishes the old parameter's value

`chain_host.c:1171`: remapping an existing knob leaves `current_value` from the
previous parameter, so the emit publishes that value rescaled into the new range
(cutoff 0.9 -> octave => CC 92) for a parameter the plugin was never told about.

**Verified against the source.** The `found >= 0` branch updates `target` and
`param` and nothing else; the sibling "Add new" branch four lines down seeds
`current_value = pinfo->default_val`. The PR adds `last_cc_out = -1;
knob_emit_cc_out(inst, found);` to the update branch, which is what makes the
stale value externally visible. The stale `current_value` itself is
**pre-existing** — the PR does not cause it, it publishes it. Adopting
`pinfo->default_val` in the update branch too would fix both halves, but that is
a behaviour change to the inbound path and belongs in its own commit.

### Finding 3b — `memset(m, 0, ...)` makes the sentinel mean its opposite

`last_cc_out` uses `-1` for "nothing sent yet", but `chain_patch.c:1239` clears
each parsed mapping row with `memset(m, 0, sizeof(*m))` and
`v2_load_from_patch_info` memcpys those rows into the instance — so every
patch-loaded knob arrives claiming it has already told the controller **CC 0**.
Harmless today only because `knob_emit_cc_out_all` forces `-1` before it emits,
on both patch paths. A future per-knob emit that does not go through the dump
would silently swallow a genuine 0. Cheapest fix is a sentinel that survives
zeroing, or an explicit reset in the copy loop.

### Finding 4 — a failed read is collapsed into "Off"

`shadow_ui.js:7385` and `:10987` treat `getSlotParam` returning `null` as
absent/"Off", so an autosave taken during a read timeout drops `knob_cc_out` from
the patch and it comes back Off after reboot. The tri-state rule again — see
CLAUDE.md, "A param read has THREE answers, not two."

**PARTLY WITHDRAWN — the persistence half is house style, not a new defect.**
`buildSlotPatchJson` writes `if (knobCcOut !== null) patch.knob_cc_out = ...`,
which is byte-for-byte the pattern `midi_fx_pre_mode` uses on the two lines
directly above it: a failed read **omits** the key rather than writing 0. The
outcome the finding describes (setting lost after a timeout + reboot) is real,
but it is a pre-existing gap in `buildSlotPatchJson` shared with at least one
sibling key, and it is not this PR's to fix. Do not ask the author for it.

**The UI half stands, and is this PR's own.** `loadKnobAssignments` does
`knobEditorCcOut = (ccOut !== null && parseInt(ccOut)) ? 1 : 0`, which collapses
a failed read to Off *for display*. That is not merely a wrong label: the
trailing row is a toggle, so a user seeing a false "Off" and clicking it sends
`knob_cc_out = 1` to a slot that was already on — the toggle flips the wrong
way off a timed-out read. One extra state (leave the row blank / re-read on the
next entry) is enough.

### Verified sound (#307), beyond the PR body

- **Both PRs pass the full host suite locally**, including each other's new
  tests: `make -C tests/host test` plus all 176 `tests/host/*.sh`, 0 failures,
  on `pull/293/head` and `pull/307/head`. #307's own
  `test_chain_knob_cc_out.sh` compiles the real `chain_params.c` and passes all
  27 checks.
- `knob_mappings` is a flat `[MAX_KNOB_MAPPINGS]` array, not per-position, so
  adding `last_cc_out` does not touch `chain_permute.h` or the owned/value split
  that `test_chain_permute.sh` derives.
- The `handleJog` change incidentally wraps `const knobNum` / `const assignment`
  in a block. They were previously bare `const`s inside a `switch` case — a
  latent lexical-scope hazard the PR removes for free. (`assignment.label` is
  `undefined`, since `loadKnobAssignments` pushes `{target, param}` — also
  pre-existing, also not this PR's.)
- `overtake_midi_send_external` is documented SPSC with "producer writes (any
  audio thread)". The chain is now a second logical producer, but it runs on the
  same SPI callback, so the single-producer property survives. The comment
  should say two producers, or a future reader will assume the ring is safe for
  a worker thread it never was.

---

## What was checked and how

Verification was against the working tree at `32a60f3a`, using `gh pr diff` for
PR-side code (no branches were checked out). The questions that turned out to
decide findings:

- `grep -rn "define MASTER_FX_SLOTS"` — 8, in two places that must agree.
- `grep -n "saveMasterFxChainConfig" src/shadow/shadow_ui.js` — 18 call sites,
  one of which is `:19438`, autosave job 5.
- `sed -n '900,925p' src/host/shadow_led_queue.c` — the `0x3B` LED SysEx layout
  that settles #292's offset question.
- `sed -n '478,530p' src/host/shadow_midi.c` — the memset-then-break that makes
  #307's mailbox contention lossy rather than merely delayed.
- Session 5 (2026-08-27) DID check branches out, in
  `.claude/worktrees/pr-review-5`, because #307 had no CI at all. Both branches
  build and pass the whole host suite. The questions that decided Session 5's
  new findings:
  - `sed -n '576,675p' src/host/shadow_midi.c` on `pull/293/head` — the two
    guards that now sit between the top of the function and the dedicated
    drain, which is finding 0.
  - `grep -n "SHADOW_MIDI_INJECT_SLOTS" src/host/shadow_constants.h` — 64, which
    bounds finding 3's delay at ~6 frames rather than "indefinite".
  - `sed -n '1432,1470p' src/schwung_shim.c` — the ROUTE_EXTERNAL drain's own
    `if (slot >= 80) break`, which is a *delay*; the loss is downstream.
  - `sed -n '75,110p' src/modules/chain/dsp/chain_host.c` — the
    `subplugin_host_api` memcpy that propagates `midi_send_external` to every
    chain sub-plugin (finding 2c), plus a `->midi_send_external(` grep across
    all fleet repos that found exactly one caller, and it is not chainable.
- `sed -n '1795,1812p' src/schwung_shim.c` — the ROUTE_EXTERNAL discard that is
  the template for #293's missing flush.
- `sed -n '528,570p' tools/pytest-schwung/src/schwung_bus/client.py` — the retry
  helper that re-raises, which is what #210's gate 2 swallows.
