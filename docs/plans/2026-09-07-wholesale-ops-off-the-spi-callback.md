# Wholesale module operations run on the worker, not the SPI callback

Status: PLAN. Nothing implemented yet.

## The report

"I get a click when restoring state from shift+delete."

## What it actually is, measured

Root cause is confirmed on hardware, four repeats plus an elimination test.

`dr32`'s state blob is the kit PATH plus param deltas:

```json
{"v":1,"kit":"/data/CoreLibrary/Track Presets/Drums/Electronic/606 Kit.json","params":{}}
```

`dr32_state_read` calls `load_kit` unconditionally whenever the blob carries a
path, so every recall re-reads the kit JSON and its 16 WAVs off the card —
**inside `set_param`, on the SPI callback.** The module logs its own cost:

```
14:39:36.715  dr32: kit '…606 Kit.json' loaded in 18.9 ms — 16 pads, 16 samples
14:44:43.009  … 18.9 ms     three deliberate repeats,
14:44:46.520  … 18.9 ms     3 s apart
14:44:49.495  … 18.9 ms
12:45:56.992  … 111.4 ms    COLD CACHE
```

and the shim's own `spi_timing` line for the same frame:

```
14:39:40  Frame(us): total avg=2680 max=20935 | pre avg=312 max=20352 | overruns=311 (+3)
          Pre(us): … param=27/20051 …
```

18.9 ms of kit load + JSON parse = the 20.05 ms param serve, against a **2.9 ms
block period**. Seven blown frames warm, ~38 cold. The gesture with dr32 removed
from the rig is silent.

### What it is NOT

Ruled out by measurement, not by argument — each of these was a live hypothesis:

- **Not a parameter discontinuity.** A snapshot recalled with nothing changed
  in between still clicks. The content of the state is irrelevant.
- **Not cumulative cost across many writes.** One preset load down the identical
  path (`My Presets` → Load) is silent. It is a single expensive call.
- **Not Master FX.** This rig's `master_fx_0.json` carries no `id`, so
  `parseMasterFxSnapshot` returns `[]` and Master FX contributes nothing to the
  recall at all.
- **Not the logger.** Idle baseline with `debug_log_on` armed was 345 frames /
  345 irq, backlog 0, headroom 2511 µs, unwavering for 45 s.
- **Not visible to the SPI tally.** `backlog` and `frames/irq` are 1 Hz
  aggregates; a single blown frame that drains immediately never showed up, and
  zero `LATE` lines were logged across the whole session. `spi_timing`'s
  `param=avg/max` is what caught it. Worth knowing before reaching for the
  tally next time.

## The general defect

This is the **third** occurrence of one defect class, and the second one we
have measured with the same instrument. The overtake path already fixed it and
recorded the identical signature:

> `dlopen()` plus a module's `create_instance()` is filesystem + allocation
> work. Measured on device it stalled the SPI callback for ~11.5 ms (param
> stage 7µs typical → 11513µs during a load) […] So the SPI thread only ever
> *requests*.

Module entry points **are** the SPI callback, and the ecosystem does not know
it — the 2026-08 audit found ~150 confirmed violations across 113 modules,
several carrying comments asserting the opposite. dr32's own comment reads
*"Load HERE, on the host thread."* There is nothing to correct in that author's
reasoning except the contract they were given.

So the host cannot keep assuming `set_param` is cheap. It has to stop calling
expensive things from the callback.

### The fade does not fix this, and does not fix what it already covers

Worth recording, because it was the obvious answer and it is wrong.

`shadow_process_fade_completions()` is called from `shim_pre_transfer`
(`schwung_shim.c:6171`) — **on the SPI callback, before the ioctl.** So
`load_patch` / `load_file` / module swap fade the slot to silence and then do
their disk read *still on the hot path*. The frame overruns exactly the same;
what the fade removes is the slot's own audio, so the remaining glitch lands in
Move's audio and the other three slots and gets attributed elsewhere.

Every wholesale operation on the box does this today. Recall is only the one
where nobody expected a hiccup, which is why it got reported.

**A fade makes an operation inaudible. It does not make it cheap.** The two are
independent and this subsystem needs both.

## Design

Wholesale operations move to the shim worker (SCHED_OTHER, cores 0–2). Fast
params stay inline.

### Which keys

Wholesale = the operations that already tolerate latency because they replace a
component's sound wholesale:

- `*:state`  ← the one with no fade today; the reported bug
- `*:module`
- `load_file`
- `load_patch` / `patch`

Plus, later, anything a module declares. Everything else stays inline: a normal
param serve is ~10 µs, and routing those through a worker costs more than it
saves.

**Fast params must not be deferred.** Pausing a slot per knob turn would lose
whole 2.9 ms blocks — strictly worse than the bug.

### Relationship to PR #303 (open, unmerged, not hardware-verified)

#303 "Build modules off the SPI callback" solves the SAME DEFECT CLASS at a
DIFFERENT ENTRY POINT: it defers `create_instance`, measured at **672.9 ms for
minijv, ~232 consecutive dropped frames**. This plan is about `set_param`.

**It does not fix the reported click**, and the two do not overlap on disk (only
`CLAUDE.md`). Its own scope note is explicit: *"Scope is the synth position
only. Audio FX, MIDI FX, Master FX, `load_patch` and boot restore keep the
synchronous path."* So of the wholesale keys listed above it claims exactly one,
`synth:module`, and this plan should claim the remainder rather than restate it.

**Its mechanism is better than the lease sketched below, and where it applies it
should be copied rather than re-invented.** "Stage, don't swap": the loader
thread builds the instance into a staging record no render path can reach, and
the SPI thread publishes it by swapping pointers from `v2_render_block`. That
keeps *"only the SPI thread ever mutates a chain instance"* true verbatim — so
it needs NO gating at the 65 `.instance` call sites, which is precisely the hard
part identified below. No locks either, in both directions, which matters
because an RT thread blocking on a SCHED_OTHER thread's mutex is unbounded
priority inversion.

**But it cannot be transferred to a state apply, and that is the crux.** Staging
works by constructing a NEW thing. A state apply MUTATES A RUNNING INSTANCE;
staging it would mean creating a fresh instance, applying the blob off-thread
and swapping — i.e. reinstantiating, which cuts reverb tails and resets arp
phase. That is exactly what a recall exists not to do ("A recall writes STATE,
never SHAPE"). So the lease problem below survives #303 intact.

**What IS directly reusable is its concurrency discipline: no field has two
writers.** Its first version shared one `state` word between the two threads and
produced three real defects — a request lost so the position never loads with
nothing logged, a stranded staged module leaking a dlopen handle per swap, and a
segfault from nulling a reusable ~1.1 MB parameter block the loader then
memset through. Work is derived from generation counters advanced by one side
only, and "is a load outstanding" is `req_gen != committed_gen`. The lease needs
the same discipline; copy the structure.

**The attribution logging in this change is an instrument #303 can use.** #303
ships unverified on hardware, and a 672.9 ms `synth:module` write is far past
the 1000 µs threshold — so it will name itself in `debug.log` before the merge
and, after it, its absence is the regression test. Note the limit honestly: this
measures the BLOCKING half only. #303's own "still owed" is the *burn* number —
CPU spent at realtime priority by inherited-FIFO plugin threads — and its
correlation with the Link Audio stalls. Nothing here measures that; that is the
RT-thread audit's job.

### The lease, which is the hard part

While the worker holds an instance, nothing on the RT path may touch it.

The overtake precedent gets this for free: it **retires the pointer** on the SPI
thread, so every existing `api && inst` guard stops calling in "with no new
gating anywhere". That trick rests on an invariant overtake has — a module may
legitimately be absent, so every site checks.

**Slots do not have that invariant.** 65 uses of
`shadow_chain_slots[…].instance` across `schwung_shim.c` and
`shadow_chain_mgmt.c`, ~14 with an explicit NULL guard. A slot's instance is
created at boot and is never NULL in normal operation, so call sites got lazy
and pass it straight into `set_param`/`render_block`. NULLing it would
NULL-deref `chain_instance_t *inst` on the audio thread — which per the
breakbeat story is a SIGSEGV that boot-loops the device, because the slot is
restored every boot and crashes before the UI can remove it.

So: **do not copy the retire trick. Gate at the outermost RT entry points
instead**, where the slot loop already lives, rather than at 65 leaf sites.

Entry points to enumerate and gate (this inventory is the first task, and it is
most of the design — it belongs in review, not in a runtime discovery):

- the mix/render loop (`shadow_mix_audio`, both `same_frame_fx` branches)
- MIDI dispatch into slots (several sites, cable 0 and cable 2)
- `mod:tick` and `chain_take_midi_tick_wake` on the idle-probe path
- the param serve itself (`shadow_inprocess_handle_param_request`)
- `shadow_inprocess_handle_ui_request` / `shadow_process_fade_completions`
- `shadow_chain_refresh_wants_sysex_tick`
- the bus and per-voice send drains
- `shadow_ui_state_update_slot` and the capture/snapshot readers

### Sequence

1. SPI thread receives a wholesale set. It stages the value, marks the slot
   leased, ramps `fade.gain` to 0, and posts to the worker. Returns immediately.
2. RT path sees the lease and skips the slot everywhere above. It renders
   silence, which is why the fade must lead — the lease is a cliff otherwise.
3. Worker calls `set_param`. Disk I/O happens here. Frame budget untouched.
4. Worker clears the lease; RT path ramps `fade.gain` back to 1.

### Open questions to settle in review

- **MIDI during the window.** Drop or queue? Dropping loses note-offs and gives
  stuck notes; queuing needs a bounded ring and a replay order. Leaning: hold
  note-offs, drop note-ons, and panic the slot on resume.
- **Staging buffer size.** `SHADOW_PARAM_VALUE_LEN` is 128 KB. One staging
  buffer serialises the burst, which is acceptable because JS writes serially
  anyway — but it must be *stated* rather than assumed, and the second write
  must block or coalesce rather than clobber.
- **JS ordering.** `setSlotParam` currently returns when the value is applied.
  It would return when the value is *queued*. `snapshotRecall` calls
  `paramPagesRevalue()` immediately afterwards, which would then read pre-apply
  values. Needs a drain or a completion handshake.
- **Does `get_param` need the same treatment?** The fleet audit found
  directory-scanning getters, and *"a `get_param` that scans a directory is
  served once per repaint"* makes it worse than the equivalent set. Not in this
  change, but the lease should not be designed so as to preclude it.

## Companion work

### Attribution logging (small, independent, do it first)

When an inline param serve exceeds the frame budget, log module + key +
duration from the worker. RT-safe: record max + key in the callback, format and
emit off-thread.

This is how the wholesale-key list grows from evidence rather than guesswork,
and it turns "there's a click somewhere" into a named line. Today's diagnosis
took an afternoon and needed a differential experiment against the user's ears;
with this it is one grep.

### dr32, at source

A 110 ms cold kit load is antisocial wherever it runs. But the obvious fix is
wrong and would ship as a silent regression:

`dr32_state_write` deliberately emits **only deltas from the kit baseline** —
`if (base && strcmp(base, val) == 0) return n; /* unedited — kit restores it */`
— so the kit reload *is* the mechanism that resets everything the blob omits.
"Skip the reload when the path is unchanged" silently stops a recall reverting
pad edits.

Correct shape: dr32 already keeps `in->state_baseline` via
`dr32_capture_baseline()`. On a same-path restore, reset params from that
in-memory baseline and skip the *sample* reload — the samples cannot have
changed, only the params. Same semantics, no disk. Its `tests/test_state.c`
gives somewhere to pin it.

## Verification

- `tests/host/` unit for the lease state machine (transitions in a header, as
  `chain_idle_tick.h` does, so it can be driven off-device).
- Source pin: every enumerated RT entry point consults the lease.
- On hardware: arm `debug_log_on`, repeat the recall, assert `param` max stays
  in the tens of µs and `overruns` does not advance. Cold cache included — the
  111 ms case is the one that matters.
- Regression: confirm `load_patch` and set load stop advancing `overruns` too,
  since they have the same defect and no one has been counting.
