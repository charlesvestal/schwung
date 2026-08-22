# RT-thread audit — what the fleet actually does, and what is still owed

Measured on hardware 2026-08-22. **Parked mid-investigation**; the instrument
is merged, the conclusion is not reached.

## Why this exists

The end goal is the **Link Audio dropouts** (open since 2026-08-19). The
3-point bisect there: stock Move = 0 stalls, Schwung with empty slots = 0
stalls, Schwung with modules loaded = a stall every 4–40 s. Move's `Link Main`
runs at **SCHED_FIFO 35**; Schwung's DSP runs at **70**. So a module worker
born at 70 that runs long is the mechanism that fits.

Module entry points ARE the SPI callback, and POSIX `PTHREAD_INHERIT_SCHED` is
the default, so a `pthread_create` from `create_instance` / `set_param` yields
a worker at the callback's priority. A 2026-08 source audit put this at seven
modules.

**That estimate did not survive measurement.** tablor was its headline case,
and by the time anyone looked, tablor had moved the call into
`create_instance`, was shedding priority on the worker's first line, and the
thing actually wrong with it — the thread was never *named*, so it wore the
parent's `comm` and hid from the very audit that found it — was not in the
estimate at all.

## The instrument

`src/host/rt_thread_audit.{c,h}`, reported from the shim worker. Off unless
armed:

```
touch /data/UserData/schwung/rt_thread_audit_on
touch /data/UserData/schwung/debug_log_on
```

**It cannot be a name check, and that is the whole design.** A child inherits
the parent's `comm`, so an inherited worker reports as `Audio Main/SPI` —
indistinguishable from the real SPI thread in `top`, in a thread list, or to
its own author. That invisibility is why these exist. The detector diffs the
SET of realtime threads by **tid**; `tests/host/test_rt_thread_audit.c` fails
if anyone reduces it to a name comparison.

Two reports:

- **arrival** — a realtime thread that was not there before, attributed to the
  module whose `<prefix>:module` write was most recent.
- **burn** — CPU consumed at realtime priority since the last sample, worst
  first, excluding everything present before the first module loaded.

## Hardware baseline (no modules loaded)

23 threads, **11 realtime**:

| tid | policy | comm |
|---|---|---|
| 939 | FIFO 10 | `MoveOriginal` |
| 967 | **FIFO 35** | **`Link Main`** |
| 977–979 | FIFO 70 | `Audio Worker` ×3 |
| 981–1005 | FIFO 70 (one at 45) | `Audio Main/SPI` ×6 |

**Nothing is at FIFO 90.** CLAUDE.md claimed both 90 and 70 in two places
three lines apart; 70 is what the hardware runs. Fixed in the same commit.

`Link Main` at 35 sharing a core with an `Audio Worker` at 70 is the
starvation relationship, visible.

## Fleet sweep — 96 modules

Ten realtime threads across six modules. Every one cross-checked against
source, because the audit is a **lead list, not a verdict**:

| Module | Threads | Source |
|---|---|---|
| **sfz** | 5 × FIFO 70 | sfizz's global `FilePool` `ThreadPool` (`FilePool.cpp:53`) ✅ |
| **osirus** | 1 × FIFO 70 | `pthread_create(&inst->boot_thread, nullptr, …)` (`virus_plugin.cpp:1886`) + 3 `restart_thread_func` sites ✅ |
| **minijv** | 1 × **FIFO 45** | `emu_thread`, `load_thread`, `NULL` attrs (`jv880_plugin.cpp:1465`, `:1591`) ✅ |
| **po32-drum** | 1 × FIFO 70 | `pthread_create(&m->render_thread, NULL, …)` in `create_instance` ✅ |
| **fork** | 1 × FIFO 70 | `pthread_create(&inst->io_thread, NULL, …)` in `create_instance` ✅ |
| **breakbeat** | 1 × FIFO 70 | **none — zero threading primitives in the repo** ❌ |

Against the estimate: **five confirmed, and not the same five.** sfz's five
threads were never counted; the module everyone was talking about is not in the
list.

### breakbeat is the audit being wrong, and why

The sweep ran 96 modules in ~90 s — about one per second — against a 1 Hz
audit sample. Attribution is "most recently loaded module", so at that rate it
is close to a coin flip. Anything this tool names must be confirmed in source
before it is acted on. A targeted slow re-run is the fix; the general fix is
sampling synchronously around the load rather than at 1 Hz.

### Existence is not the harm

`Link Main` gets only the CPU a FIFO 70 thread leaves it, so what starves it is
a thread that **runs**, not one that exists. po32-drum's render worker and
fork's I/O thread park on a condvar immediately — they cost nothing, and on a
headcount they would have sat near the top of a suspect list forever.

The three that plausibly matter are **sfz** (loading samples), **minijv**
(reading ROM) and **osirus** (booting a DSP56300) — all of which do sustained
work. **This is a hypothesis, not a result.**

### A thread outlived its module

Baseline was 11 realtime before the sweep and **12 after**. Something spawned
during the sweep survived its module's unload. If that generalises, realtime
threads accumulate across a session, which is its own contribution.
Unexplained.

## What is still owed

1. **The burn number on hardware.** The measurement is written, unit-tested and
   mutation-checked; it has never produced a device figure. Three attempts
   failed for reasons unrelated to the code — see the gotchas below.
2. **Correlation with the dropouts.** Burn figures next to
   `link_audio_avail_log_on` stalls is what turns a suspect list into cause and
   effect.
3. **The leaked thread.**
4. **breakbeat**, resolved by a slow targeted run.

## Sizing the real fix (residual option 3)

Moving module loading off the SPI thread so `create_instance` / `set_param` no
longer run at realtime:

- **Fixes** sfz and osirus — pure inheritance, so a normal-priority loader
  makes the inherited priority harmless.
- **Does not fix** minijv, which is at FIFO **45**, i.e. it sets its own
  priority rather than only inheriting.
- **Does not touch** the ~143 non-thread violations (malloc, `fopen`, locks in
  `render_block`) — though it does make `fopen` in `set_param` legal, which
  retires a large part of the documentation problem.
- **Real cost is not the moving.** Thread-safety is currently *free* because
  everything is on one thread — `chain_reorder.c`'s permutation and the
  `dlopen`/`create_instance` path both rely on it. Taking loading off that
  thread means a loader and `render_block` touching one instance concurrently,
  needing real synchronisation on a realtime deadline, for 113 modules whose
  internal state assumes single-threaded access.

## The documentation problem this exposed

The RT contract lives at the top of `src/host/plugin_api_v1.h`. **Every module
vendors its own frozen copy of that header**, and the contract is present in
**4 of 44**. Module repos' own `CLAUDE.md`: **1 of 35** (80 repos, 35 have
one). Most of these modules are LLM-authored, so the model's context is the
vendored copy — which never told it.

So "the contract deserves louder placement" understates it: the contract is
*absent* from the file authors actually read. Making the canonical header
louder reaches none of the 40, and a helper added there would sit in a file
they do not have.

## Gotchas that cost time here

- **Arm order matters.** `unified_log` only starts writing once it notices
  `debug_log_on`, rechecked every 100 calls. Arming both flags together latched
  the baseline into a log still dropping writes, and the audit then ran twelve
  minutes reporting nothing — indistinguishable from a clean result. Fixed: it
  waits for the log before latching. Sequence that works is **reboot → arm →
  wait for a `rt-audit: armed` line with a FRESH timestamp → trigger**.
- **One trigger per boot.** `contractDumpDone` in `shadow_ui.js` latches for the
  life of the process; re-touching the trigger without a restart is a no-op.
- **`debugLog` is not in scope** in a script evaluated through that trigger
  (`new Function("os","std",src)`) — same trap the tool's own comments record
  for `os`. It throws into the trigger's `catch` and the run silently does
  nothing.
- **Never truncate `debug.log` while the shim holds it open.** It stops writing.
  And a truncated log leaves NUL padding, after which `grep` treats the file as
  binary and prints nothing — use `grep -a`.
- **Do not trust `set_state/` mtimes** to tell you which set is live. The
  newest-mtime directory was hours stale while the device was in use.
