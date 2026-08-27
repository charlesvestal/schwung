# RT-safe module loading — design B, and the fleetwide threading bug it fixes

Branch `fix/rt-safe-module-loading`. Residual 2.6, gated by
`2026-08-21-create-instance-thread-spike.md` and re-scoped by the measured
`2026-08-22-rt-thread-audit-findings.md`.

## The bug, stated once

`create_instance` runs on the SPI callback, which is **SCHED_FIFO 70** on core
3. POSIX's default is `PTHREAD_INHERIT_SCHED`, so **a `pthread_create` from
`create_instance` yields a worker at FIFO 70**. Move's own Link Audio publisher,
`Link Main`, is **FIFO 35**. Any module worker that runs long therefore
outranks, and starves, the thread Move needs to publish audio.

Nobody declared this, and nobody could have inferred it: the plugin API has no
thread contract, and every module vendors a frozen copy of the header that
would have carried one.

Same call site, second defect: the load itself **blocks the SPI callback for a
measured 673 ms** loading minijv — ~232 consecutive dropped frames, corroborated
by `link_subscriber cbgap 674.3 ms`. One fix addresses both.

## What the fleet actually does — measurement, not the source audit

The 2026-08-21 source audit predicted seven modules. Hardware found **five, and
not the same five**. Take the measured list as authoritative:

| Module | RT threads | Runs long? | Fixed by design B? |
|---|---|---|---|
| sfz | 5 × FIFO 70 | yes — loads samples | **yes** (pure inheritance) |
| osirus | 1 × FIFO 70 | yes — boots a DSP56300 | **yes** (pure inheritance) |
| minijv | 1 × **FIFO 45** | yes — reads ROM | **NO — sets its own priority** |
| po32-drum | 1 × FIFO 70 | no — parks on a condvar | yes, and it never mattered |
| fork | 1 × FIFO 70 | no — parks on a condvar | yes, and it never mattered |
| breakbeat | 1 × FIFO 70 | — | **not real** — zero threading primitives in the repo; a 1 Hz audit misattributing a 1/s sweep |

Two things follow, and both are easy to get wrong:

- **Existence is not the harm.** po32-drum and fork would have sat at the top of
  any headcount forever while costing nothing. What starves `Link Main` is a
  thread that *runs*.
- **minijv is not reachable from here.** It calls for FIFO 45 explicitly rather
  than merely inheriting, so a SCHED_OTHER loader leaves it exactly where it is.
  It can only be fixed in its own repo — and its 45 looks deliberate (above
  `Link Main`'s 35), so that is a conversation with its author, not a patch.

The burn number — CPU actually consumed at RT priority — **has never been
obtained on hardware**. The three that plausibly matter (sfz, minijv, osirus)
are a hypothesis. This plan does not depend on resolving it: the inheritance is
wrong whether or not it is the cause of the Link Audio dropouts.

## Design B — stage, don't swap

The loader thread **never touches a live position**. It builds the whole
instance into a staging record no render path can reach; the SPI thread commits
by swapping pointers, at the same place the synchronous load publishes today. So
*"only the SPI thread ever mutates a chain instance"* survives verbatim and
**neither side takes a lock**.

This is the point the audit-findings doc sizes wrongly — it costs "real
synchronisation on a realtime deadline ... for 113 modules whose internal state
assumes single-threaded access". That would be true of a design that ran
`create_instance` against a live position. Staging means the only object the
loader touches is one no other thread can name yet, and the only object the SPI
thread touches is one the loader has finished with. A module's internal state is
never reached from two threads.

The shape is not new here: `SHIM_EVT_OVERTAKE_DSP_LOAD`
(`schwung_shim.c:1651-1789`) already does loader-thread + staged instance +
detach-then-defer-free for overtake modules. The chain path never adopted it.

```
SPI thread                          loader thread (SCHED_OTHER, cores 0-2)
----------                          -------------------------------------
set_param("synth:module")
  fill request, gen++          -->  dlopen / init_v2 / create_instance
  return IMMEDIATELY                read chain_params + ui_hierarchy
                                    parse module.json capabilities
  ...render old synth...            if gen changed: destroy it, reload
  ...render old synth...       <--  publish staged record, state = READY
render_block:
  commit: swap pointers in
  retire old triple -->             destroy_instance + dlclose
```

### Decisions worth writing down

- **A generation counter, not a queue.** Spinning the module picker must not
  build a backlog of loads. The SPI thread overwrites a single-slot request and
  bumps `gen`; the loader, on finishing, discards its result if `gen` moved and
  loads the newest. Commit publishes only a result whose `gen` still matches.
- **`<prefix>_module` keeps reporting the COMMITTED name.** Reporting the
  pending name would make it a statement of intent, and two shipped things
  depend on it being a statement of fact: `component_load_gate.mjs` treats
  "named + no hierarchy" as *fall back immediately*, and the chain host's
  publish-after-`create_instance` ordering is cited in CLAUDE.md. Patch-save
  reads through this path too and was never traced.
- **`<prefix>:is_loading` becomes load-bearing for the first time.** The chain
  host has never implemented it — it never had to, because loading was
  synchronous. The UI already consumes it end to end (`page_controller.mjs:966`,
  `shadow_ui_param_pages.mjs:402`, the `Loading...` hold). Async loading is what
  finally gives that machinery something true to read. It must answer exactly
  `"1"` / `"0"`: any other answer latches the component as not-implementing-it.
- **A replace keeps the old module rendering for the whole load.** No silence,
  and no hole to skip. An append opens a genuine hole, which the existing
  hole-skipping already covers.
- **Nothing is freed on the SPI thread.** Retired triples go to the loader via a
  small ring. On overflow, leak and log loudly rather than destroy inline —
  the overtake path's rule, and its reasoning is unchanged: *a leak is
  recoverable; a use-after-free in the audio path is not.*
- **The loader thread demotes itself on its first line**, and is created with
  `PTHREAD_EXPLICIT_SCHED` + `SCHED_OTHER`. Belt and braces on purpose: the
  thread is created from `v2_create_instance`, which is *itself* on the SPI
  callback, so inheriting is the default failure and it would silently defeat
  the entire change. It also pins to cores 0-2, keeping core 3 for SPI.
- **One loader thread per chain instance**, created lazily on the first deferred
  load and joined in `v2_destroy_instance`. At most 5 (4 slots + Master FX),
  each parked on a condvar — which, per the audit, costs nothing.

### What a permutation does to a pending load

`chain_reorder.c` shifts every per-position array together. A pending load names
a position, so it must be shifted with them: the request's index is retargeted
through the same `map[]` that re-aims modulation targets and LFOs, and `-1`
means cancel-and-reap.

## Scope

**This branch — the synth position only.** All the measured harm is synths (sfz,
minijv, osirus) and the 673 ms measurement is a synth. Five load sites exist:

1. `chain_host.c:368` `v2_load_synth` — **this branch**
2. `chain_host.c:579` `v2_load_audio_fx` (append)
3. `chain_host.c` audio FX slot variant
4. `chain_midi.c:216` `v2_load_midi_fx_slot`
5. `shadow_chain_mgmt.c:1011` Master FX slot load (host side, not the chain host)

2–5 keep the synchronous path and are a follow-up. Splitting here is not
timidity: the staging record and commit point are shared machinery, and getting
them right against one loader with a real test is what makes the other four
mechanical.

**Also this branch:** the three thread contracts, written where authors will
actually read them.

**Not this branch:** the fleet repo patches (separate repos, separate releases),
and the ~143 non-thread violations (malloc / `fopen` / locks in `render_block`).

## The contracts

None needs a lock. From the spike, restated for the measured priority:

**(a) `log` is already thread-safe** — `unified_log_v` opens with
`pthread_mutex_trylock` and returns without logging if held. Concurrent calls
drop a line; neither side blocks.

**(b) The scalar queries are benign races.** `sample_rate`, `frames_per_block`,
the two offsets and `mapped_memory` are set once at init. `get_bpm` and
`get_clock_status` read naturally-aligned words the SPI thread writes — no
tearing on ARM64, worst case a tempo one block stale. One wrinkle worth a
comment: `chain_get_clock_status` **writes** `g_clock_output_enabled` and does an
`access()`, so it is not a pure read and can duplicate one settings re-read.

**(c) The MIDI/modulation callbacks are create-forbidden.** Zero plugins call
them during create today, so the rule costs nothing to adopt now and is
unenforceable if we wait.

Where they go — and the placement is the finding, not an afterthought. The
canonical contract lives at the top of `src/host/plugin_api_v1.h`. **Every
module vendors its own frozen copy of that header, and the contract is present
in 4 of 44.** Module repos' own `CLAUDE.md`: 1 of 35. Most of these modules are
LLM-authored, so the model's context *is* the vendored copy — which never told
it. Making the canonical header louder reaches none of the 40. So the contract
also goes in `docs/MODULES.md` and `docs/REALTIME_SAFETY.md`, and the fleet
patch below carries it into the vendored copies that matter.

## Tests

The fixture-plugin harness in `tests/host/test_chain_midi_fx_slot.sh` builds a
real `.so` and dlopens it through the real loader. Extended here:

1. **`create_instance` observes `SCHED_OTHER`.** A fixture that records
   `sched_getscheduler(0)` and its priority. This is the fleetwide bug asserted
   directly, on a dev machine, with no device — and it fails today.
2. **A worker `pthread_create`d from `create_instance` is `SCHED_OTHER`.** The
   actual harm, one level down from (1); (1) passing does not imply it if the
   loader is ever created wrong.
3. **`set_param("synth:module")` returns promptly** against a fixture whose
   `create_instance` sleeps 300 ms. Pins the 673 ms fix as a property rather
   than a story.
4. **The old synth renders throughout**, and the commit publishes exactly once.
5. **Supersede**: three rapid writes leave the third loaded, and the first two
   destroyed, not leaked.
6. **`destroy_instance` with a load in flight** joins cleanly — no use-after-free.
7. **`is_loading` answers exactly `"1"` then `"0"`** — an unexpected answer
   latches the component as not-implementing-it, so the literal matters.

Run under ASan/TSan locally where available; CI's `host-tests` gates the suite.

## Fleet repos — backwards compatible by construction

A plugin calling `sched_setscheduler(0, SCHED_OTHER, &(struct sched_param){0})`
as the first statement of its own worker needs **no `min_host_version` bump**:
on an old host it demotes a FIFO-70 thread (the fix), on a new host the thread
is already SCHED_OTHER and it is a no-op. It depends on nothing the host
provides.

Targets, in priority order: **minijv** (the only one design B cannot reach),
then sfz and osirus (defence in depth for users on older hosts). po32-drum and
fork are correct-but-harmless and can take the patch whenever convenient.
breakbeat needs a slow targeted re-run before anyone touches it.

Second, cheaper fix from the same audit: **name the thread**
(`pthread_setname_np`). An unnamed worker inherits the parent's `comm` and
reports as `Audio Main/SPI` — indistinguishable from the real SPI thread in
`top`, in a thread list, or to its own author. That invisibility is why these
survived. It is one line and it makes the next audit honest.

## Still owed, and deliberately not claimed here

- The burn number on hardware, and its correlation with the Link Audio stalls.
  Until then, "these five starve `Link Main`" is a hypothesis that fits.
- The realtime thread that **outlived its module** during the sweep (baseline 11
  before, 12 after). Unexplained. If it generalises, RT threads accumulate
  across a session.
- `breakbeat`, pending a slow targeted run.
- Load sites 2–5.
