# Spike — is `create_instance` safe to call off the SPI thread?

**Verdict: yes, for the entire current fleet, and the audit found a bug that
moving off the SPI thread would *fix* rather than introduce.** Residual 2.6
(RT-safe module loading, design B) is not blocked. Three thread contracts have
to be written down before code is written; none of them requires a lock.

This is the gate the handoff (`docs/plans/2026-08-21-handoff.md` §2.1) said to
run before writing any of design B: *"whether a third-party `create_instance` is
safe to call off the SPI thread at all. If that audit comes back badly the whole
design needs rethinking."*

## Method

The worry was that a plugin can call back into `host_api_v1_t` during create,
and some of those callbacks write structures the SPI thread owns —
`midi_inject_to_move` writes a ring the SPI thread drains, `mod_emit_value`
touches the chain modulation bus.

So: for every plugin repo in the parent directory, resolve the `.create_instance`
member of its `plugin_api_v2_t` to the actual function name (it is almost never
literally called `create_instance` — that is why a naive grep under-reports),
extract that function's body by brace matching, then transitively expand every
locally-defined function it calls within the same translation unit, and collect

- every `->` access to a `host_api_v1_t` member, and
- `pthread_create` / `dlopen` / `fopen` / `open` / `mmap` / `system` / `popen`.

Script: `tools/spike/create_instance_audit.py` (see §5). 67 create paths across
65 repos.

## 1. What the fleet actually calls during create

| host member | callers |
|---|---|
| `log` | ~50 — the overwhelming majority |
| `sample_rate` | chordism, hush1, mrsample, mverb, sf2, sfz (both), talkbox |
| `mapped_memory` + `audio_in_offset` / `audio_out_offset` | breath, performance-fx |
| `get_bpm` | ambiotica |
| `get_clock_status` | tb3po |

And the number that matters:

> **Zero plugins call `midi_send_internal`, `midi_send_external`,
> `midi_inject_to_move`, `mod_emit_value` or `mod_clear_source` during create.**

Not "few". None. Fleet-wide there are exactly three call sites of those
callbacks anywhere in plugin C at all — `schwung-tb3po` (`send_midi`, reached
only from the step sequencer in `render_block`), `schwung-fourtrack` (wrappers it
hands *down* to the sub-plugins it hosts), and `move-anything-jupiter` (an old
fork of the host itself, not a plugin). Everything else that greps positive is
the vendored copy of the struct declaration in each repo's own header.

**Do not take this as a permanent property.** It is a fact about today's fleet,
not a contract the API states — §3 is about turning it into one.

## 2. The finding that flips the argument

Seven plugins call `pthread_create` **inside create_instance**:
`schwung-airwindows`/`schwung-clap` (`clap_fx.cpp`), `schwung-jp8000`,
`schwung-nam`, `schwung-sfz` (`xsynth_plugin.c`), `schwung-tb3po`,
`schwung-virus`, `schwung-webstream`.

Today create runs on the SPI callback, which is **SCHED_FIFO 90 on core 3**. A
thread created there **inherits FIFO 90**. That is exactly the defect already
recorded for keydetect (memory `module_threads_and_logging_in_audio_path`) and
it is the same mechanism as the Link Audio starvation
(`link_audio_producer_burst_dropouts`: Move's `Link Main` is FIFO 35, our DSP is
70, and it starves).

So seven modules are currently spawning FIFO-90 worker threads, and nobody
declared it. Under design B, create runs on a plain SCHED_OTHER loader thread
and every one of those threads is born SCHED_OTHER instead. **Moving
`create_instance` off the SPI thread is a fix for a live priority-inheritance
bug, not just a latency change.** (The loader thread must therefore be created
*from* a SCHED_OTHER context, or explicitly reset — the same rule
`shadow_process.c` already applies before exec.)

Also present at create, all of which are the very cost design B exists to move:
`fopen` (13), `open`/`mmap` (norns, breakbeat, jp8000, virus, pipewire),
`dlopen` (chain host itself), and `system()` (schwung-rnbo).

## 3. The three contracts to write down

None needs a lock. Each is a sentence in `plugin_api_v1.h` plus a line in
`docs/REALTIME_SAFETY.md`.

**(a) `log` is already thread-safe, by construction.** `host->log` →
`shadow_log` → `unified_log_v`, which opens with
`pthread_mutex_trylock(&log_mutex)` and **returns without logging if the lock is
held**. A loader thread and the SPI thread can call it concurrently; the only
possible outcome is a dropped line, never a block on either side. This is the
one callback that is safe today with no change.

**(b) The scalar reads are benign races, and must be declared as such.**
`sample_rate`, `frames_per_block`, the two offsets and `mapped_memory` are set
once at init and never written again — not races at all. `get_bpm`
(`shim_get_bpm` → `sampler_get_bpm`) and `get_clock_status`
(`chain_get_clock_status`) read `int`/`uint64_t` globals the SPI thread writes.
On ARM64 these are single-instruction naturally-aligned accesses, so no tearing;
the worst case is a create observing a tempo one block stale, which no plugin
can distinguish from being created one block earlier.

One wrinkle worth a comment rather than a fix: `chain_get_clock_status` calls
`chain_refresh_clock_output_enabled`, which **writes** `g_clock_output_enabled`
and `g_clock_next_refresh_ms` and does an `access()` on a settings file. Called
from two threads it can duplicate one settings re-read. Benign, but it means
`get_clock_status` is not a pure read, and a future maintainer will assume it is.

**(c) The MIDI/modulation callbacks must be declared create-forbidden.** Nothing
calls them today (§1), so the rule costs nothing to adopt now and is unenforceable
later if we wait. Proposed wording for `plugin_api_v1.h`:

> During `create_instance` and `destroy_instance`, the host may call your plugin
> from a background loader thread. You may call `log` and the scalar queries
> (`get_bpm`, `get_clock_status`, `get_beat_position`, `slot_recv_channel`); you
> may **not** call `midi_send_internal`, `midi_send_external`,
> `midi_inject_to_move`, `mod_emit_value` or `mod_clear_source`, and you may not
> write through `mapped_memory`. Defer any of those to your first `render_block`.

A host-side test can pin this cheaply: build the fleet's create paths through the
same audit script in CI-for-modules, or — better, because it does not depend on
source access — hand plugins a **create-time host vtable** whose forbidden
members are stubs that log loudly and return failure, and swap in the real vtable
at commit. That converts an unwritten convention into an observable one, and it
is five lines at the call site.

## 4. What this does and does not settle

Settled: the gate in §2.1 of the handoff. Design B may proceed.

Not settled, and still carried from the handoff as lower-confidence: the
SET-replay buffer sizing (by inspection, not measurement); whether making
`<id>_module` report the *pending* name disturbs patch-save (untraced); and the
claim that `docs/REALTIME_SAFETY.md` is wrong to treat `load_patch` as already
safe.

Newly raised here: `schwung-fourtrack`'s create path hosts **sub-plugins**, so
its `create_instance` can transitively call another plugin's `create_instance`.
Design B stages one position at a time; a nested create inside a staged create is
still just work on the loader thread, so it should compose — but it is the one
case where "one pending load at a time" is not literally true, and the pending-
index retargeting through `map[]` should be checked against it.

## 5. Reproducing

```
python3 tools/spike/create_instance_audit.py    # run from schwung-parent/
```

Prints one line per create path: repo, file, resolved entry function, the
`host_api_v1_t` members reached transitively, and the risk calls. Re-run it
before adopting §3(c) — the point of writing the rule down is that the fleet is
allowed to change under us.
