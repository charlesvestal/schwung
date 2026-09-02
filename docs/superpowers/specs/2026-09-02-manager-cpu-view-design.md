# Schwung Manager — CPU usage view

**Date:** 2026-09-02
**Status:** Design approved, not implemented
**Branch:** `manager-cpu-view`

A page in the web manager (`http://move.local:7700/system/cpu`) showing where the
device's CPU goes, broken down **by module** and **by system process**.

---

## The fact the whole design turns on

**Modules are not processes.** Every slot synth, slot FX, MIDI FX, Master FX and
overtake DSP is a `.so` loaded *inside MoveOriginal* and executed **on the SPI
callback thread**. `/proc` can say what MoveOriginal costs in total; it can never
split that by module.

So per-module CPU has to come from timing instrumentation in the shim, and the
page shows **two different quantities that must never be conflated**:

- **Frame budget (core 3)** — per-module µs as a percentage of the SPI frame
  period. This is the number that decides whether you get dropouts.
- **Process CPU (cores 0–3)** — ordinary `/proc` percentages. `link-subscriber`
  is a real process and appears here honestly.

Labelling these as one number would make the page lie.

---

## Is monitoring performance-impacting?

Asked during design; answered with the source rather than a guess, because the
answer determines what gets armed and what stays on.

### 1. Existing RT timing — already paid, today, unconditionally

`SHADOW_TIMING_LOG` is `0` (`src/schwung_shim.c:80`), but it gates only the
`fopen`/`fprintf` calls. All 35 `clock_gettime` sites and all 25
`TIME_SECTION_START()` macros compile in and run on every SPI frame right now.
With four active slots that is roughly **78 `clock_gettime` calls per frame** —
~50 for the host sections, 4 for the frame boundaries, 6 per active slot.

On ARM64, `CLOCK_MONOTONIC` resolves in the vDSO (a `CNTVCT_EL0` read, no
syscall) at roughly 20–40 ns, so ≈2–3 µs of a 2902 µs frame — **under 0.1%**.

> **The vDSO assumption is the one number to measure rather than reason about.**
> Task 0 measures it on the device before anything is built on top of it. If
> `clock_gettime` turns out to be a real syscall here, the existing baseline is
> ~50× worse than stated and *that* becomes the finding, not this feature.

### 2. What this feature adds to the RT path — effectively zero

- Sums alongside the existing maxes: a `+=` per slot per frame. Arithmetic, no
  new clock reads.
- Timing Master FX and the overtake DSP: **+4 `clock_gettime`/frame** at most
  (~0.005%). The MFX loop's `continue` guard means only *loaded* slots cost.
- Publishing: **no memcpy at all.** `spi_snap` stops being a static and becomes
  a pointer into the mapped segment. The same stores land in a different page.

### 3. The polling — the only real cost, and it is off the RT path

A 1 Hz `/proc` scan plus the SHM read runs in `schwung-manager`, `SCHED_OTHER`
on cores 0–2. Same class of load as the manager's existing 1-second htmx polls.
It never touches core 3.

### Conclusion, and what it decides

**Collection stays always-on. Polling is what the button arms.**

Collection is free, and arming it would produce the blank-page-looks-broken
failure `docs/DIAGNOSTICS.md` warns about ("silent for ~20 s after arming, which
looks like a broken build"). Polling has a real if small cost, so no poll
happens until the user presses **Measure CPU**.

---

## Architecture

```
SPI callback (core 3, FIFO 70)
  └─ existing per-slot + per-section timers, extended with sums,
     plus new Master FX / overtake DSP call sites
        └─ writes directly into  /schwung-perf
             seqlock: seq++ … stores … seq++    (odd = writing)

schwung-manager (SCHED_OTHER, cores 0-2)
  ├─ perf_shm.go   mmap /schwung-perf, seqlock read, retry on odd/torn
  ├─ perf_proc.go  /proc/stat, /proc/<pid>/stat, /proc/loadavg → CPU%
  └─ perf.go       joins the two + resolves slot → module id via ShmParams

GET /system/cpu          full page, idle, with a [Measure CPU] button
GET /system/cpu/values   htmx partial, polled 1 Hz only while measuring
```

---

## Data model

New shared header **`src/host/perf_snapshot.h`**. The existing
`spi_timing_snapshot_t` moves there out of `src/schwung_shim.c` (where it is
currently a file-static that nothing outside the shim can read), gains fields,
and becomes the SHM payload.

```c
#define SHM_SCHWUNG_PERF      "/schwung-perf"
#define SCHWUNG_PERF_MAGIC    0x50455246   /* "PERF" */
#define SCHWUNG_PERF_VERSION  1
#define SCHWUNG_PERF_SHM_SIZE 4096         /* container, not a fit */
```

`magic` and `version` stay the **first two fields**, so a short segment left by
an older shim can be version-checked without touching the tail — the
`LINK_AUDIO_IN_SHM_VERSION` SIGBUS lesson (see `CLAUDE.md`, "The IN ring is
sized for Move's jitter").

Size is a whole page deliberately: tmpfs allocates by page, so an 84-byte
segment already occupies 4096 on this device. Headroom is free. A
`_Static_assert(sizeof(schwung_perf_snapshot_t) <= SCHWUNG_PERF_SHM_SIZE)` plus
a floor assert means **only shrinking fails the build** — see `CLAUDE.md`, "An
SHM buffer sized to `sizeof` reads as FULL, and is not".

### Fields added to the existing snapshot

| Field | Why |
|---|---|
| `slot_synth_avg[4]`, `slot_fx_avg[4]`, `slot_render_avg[4]` | The existing entries are **max-only**. A max over a 3 s window is a spike detector; a load figure needs the mean. Most important addition. |
| `mfx_avg[8]`, `mfx_max[8]` | New call site — the loop at `src/schwung_shim.c:2733`. |
| `overtake_gen_avg/max`, `overtake_fx_avg/max` | New call sites. |
| `frame_period_us` | The denominator, **measured** rather than the 2902 µs constant, so the percentage cannot silently drift from reality. |
| `sample_window_frames` | How many frames the avg covers. A consumer that does not know the window cannot tell a real average from a partial one. |
| `magic`, `version` | New, first two fields. |
| `seq` | Already exists; becomes the seqlock counter (odd = writing). |

### Writer

`spi_snap` becomes a pointer into the mapped segment, falling back to a static
when the segment is not mapped. Same stores, different page — **no memcpy, no
added RT cost**. Pages are touched at attach so the callback never faults.
Attach happens on the worker thread (`shim_worker.c`), never the SPI path —
`shm_open`/`mmap` are not RT-safe.

### Reader

`perf_shm.go`: hand-mirrored offsets, the same house pattern as `shmparams.go`,
and carrying the same kind of comment saying what breaks if they drift.

Seqlock read: sample `seq`, read the payload, re-sample `seq`. Retry while odd
or changed; **bail after 3 attempts and report the read as failed.**

> **A failed read must be reportable as failed, distinct from "zero CPU."**
> Per `docs/SHADOW_UI.md` and the `param_read_null_vs_empty` rule: never let a
> read that did not answer become a picture. "Shim not running" and "everything
> idle" are different sentences, and a 0% bar would tell the same lie for both.

---

## What is *not* separable, and is labelled as such

**MIDI FX have no per-frame render.** They run event-driven inside the chain
host's `on_midi`, so their cost is already folded into the `proc_midi` host
section. The page surfaces that section by name and labels it *"MIDI FX and MIDI
routing — not separable per module"*, rather than inventing a per-module number
or leaving it looking like host overhead.

---

## The page

`GET /system/cpu` renders idle: an explanation, the frame-budget denominator,
and a **[Measure CPU]** button. Nothing is sampled.

The button is `hx-get="/system/cpu/values"` swapping in a partial that itself
carries `hx-trigger="every 1s"`. **[Stop]** swaps back to the idle partial.
Polling therefore lives entirely in the browser — close the tab and it stops.
**No server-side session state, no goroutine, no timer, nothing to leak on the
device.**

### The delta problem

A single read of `/proc/<pid>/stat` gives a process's *lifetime average*, which
is not what anyone means by CPU usage. The handler keeps one previous sample and
diffs against it — and **divides by measured elapsed time, never by an assumed
1 s**, so a slow poll, a second browser, or a paused tab yields a correct number
instead of a scaled-wrong one.

The first request after arming has no predecessor. It reports **"priming…"**,
not `0%`. Same rule as the seqlock failure.

### Panels

1. **Frame budget (core 3)** — the ranked list, and the reason the feature
   exists. Per slot: module id (via `synth_module` / `fx_module` from
   `ShmParams`), synth µs, FX µs, and **% of the measured frame period**, avg
   *and* max, with the max labelled as a spike rather than a load. Then the 8
   Master FX slots (`master_fx:N:module`), overtake gen/FX, and the host
   sections including the `proc_midi` caveat above.
2. **Processes** — `/proc` CPU% for every process above 0.5%, with
   `MoveOriginal`, `link-subscriber`, `shadow_ui`, `jackd` and
   `schwung-manager` **always listed even at 0%**, so a missing
   `link-subscriber` shows as absent rather than as silence. Discovered by
   scanning `/proc/*/comm`, not hardcoded PIDs.
3. **Per-core** — `/proc/stat` `cpu0`–`cpu3` plus load average, with **core 3
   flagged as the SPI core**.
4. **Module RT threads** — FIFO/RR threads inside MoveOriginal and the CPU they
   burned, from `/proc/<movepid>/task/*/stat`. These appear in neither panel
   above and are the `docs/plans/2026-08-22-rt-thread-audit-findings.md` failure
   mode: a thread that inherited FIFO 70 from the SPI callback and starves
   Move's own `Link Main` at 35.

Accessibility follows the house standard set by the file browser: real
`progress-bar` roles with `aria-valuenow`, spoken labels on every row, and a
table that reads correctly without the bars.

---

## The parser trap, pinned by a test

Panel 4 needs the `/proc/.../stat` parser reimplemented in Go — the manager is a
separate process, so the C one in `src/host/rt_thread_audit.c` (which reads
`/proc/self`) cannot be reused, and cgo is not worth it for ~40 lines.

`rt_thread_audit.c` delimits `comm` on the **last `)`**, never on whitespace,
because Move's threads are literally named `Audio Main/SPI` — with a space.
Tokenising on whitespace shifts every field after it and silently reports every
thread as `SCHED_OTHER 0`, which reads as a clean all-clear.

The Go port gets a golden-line test with a space **and** a paren in `comm`.

---

## Testing

- `tests/host/test_perf_snapshot_size.c` — the `<=` assert holds, a shrunken
  container fails, and `magic`/`version` are the first two fields.
- `tests/host/test_perf_shm_offsets.sh` — pins the Go offset mirror against the
  C header. This is the `shmparams.go` drift hazard (a 16-byte shift there once
  made every GET return empty), and it is the test that most earns its place.
- Go: seqlock reader against a synthetic buffer, including odd and torn `seq` →
  must report **failed**, not zeros.
- Go: `/proc` parsers against golden text, including the `comm`-with-space case.
- Go: delta math against two fixed samples with a known elapsed time.

Per `probes_that_measured_the_wrong_thing`: each test is mutated once to prove
it *can* fail before it is trusted.

CI gates `tests/host/`, so both new host tests run on every PR. The Go tests run
under the existing `go` check.

---

## Deploy coupling

Shim and manager ship together, as `install.sh` already does.

Growing the segment is safe for an old reader — `shadow_shm_map()` fstats on
attach and refuses a segment shorter than requested. The failure mode for a
version mismatch is an explicit page message, *"shim is older than this manager
— deploy both"*, never garbage and never a SIGBUS.

---

## Out of scope

- History / time-series storage. The page shows the current sample; it is not a
  recorder. Adding a rolling window is a later, separable change.
- Per-module CPU for MIDI FX (not separable — see above).
- Replacing `schwung-signalscope`, the community on-device OLED dashboard. That
  is process-level only and stays as it is.
- Any change to the on-device shadow UI.
