# Measuring the device

Split out of `CLAUDE.md`, which keeps the test-suite commands and the unified
logger, and points here.

Every switch below is **off by default** and armed by touching a file under
`/data/UserData/schwung/`. Disarm them when you stop measuring — `debug_log_on`
has itself caused the audio dropouts it was being used to hunt.

**On-device E2E tests** (opt-in, not in CI): `tools/pytest-schwung/` is a pip-installable pytest plugin that drives a real Move end-to-end through `schwung-testd`, an opt-in test-bus daemon (TCP loopback, started manually over SSH; built into the tarball but not auto-started). Tests inject MIDI, wait for SPI frames, snapshot pad LEDs, capture MIDI_OUT, and reset to a known-empty set (`pristine_set`). Run `pytest tests/e2e` against attached hardware. Full protocol, fixtures, and hardware pitfalls in `tools/pytest-schwung/README.md`.

**OTLP span tracing** (perf profiling, off by default): `touch /data/UserData/schwung/otlp_trace_on` makes **both** the shim and the `shadow_ui` process emit realtime-safe spans as OTLP/JSONL to `/data/UserData/schwung/traces/`, one file per service (`schwung-shim-*` / `schwung-shadow-ui-*`). Shim: `spi.pre`/`spi.post` roots + `shadow.mix_audio`, `midi.process`, `param.serve` children. shadow_ui: `js.tick` + `param.get`. Spans correlate **cross-process by trace_id** — the shim's `param.serve` is emitted as a child of shadow_ui's `param.get` (context propagated through `shadow_param_t`), so Tempo/Jaeger stitch the two files into one trace. JS modules (overtake/chain, incl. ion) can add spans via `host_trace_begin(name) -> handle` / `host_trace_end(handle)` (shadow_ui context only); balance the pair within one `tick()` (handles come from a 16-entry table reset each `js.tick`). `rm` the file to stop. Zero hot-path cost when off. See `docs/tracing.md`.

**Two more UI perf diagnostics** (both off by default):

- `touch /data/UserData/schwung/param_tally_on` — wraps `shadow_get_param` /
  `shadow_set_param` and reports once a second: reads and writes per tick, the
  keys by frequency with a sampled caller, any single call over 24 ms, and each
  key's value RANGE (`MOVING synth:timbre 0.115 .. 1.000`) so you can tell
  whether a value is actually moving — which is how the idle-LFO fix was
  confirmed. Needs a `shadow_ui` restart to install (it wraps at init). Writes
  `param_tally.txt` once per window. See `src/shared/param_tally.mjs`.
- `param_pages fps: 55 draws / 55 ticks / 1004ms` in `debug.log` while the knob
  grid is up. Draws vs ticks separates the two causes of "dropped frames":
  draws << ticks means something gates the redraw; draws == ticks and both low
  means the tick is slow or its pacing is wrong.

**SPI frame tally** (`touch /data/UserData/schwung/spi_tally_on`, off by
default) reports at ~1 Hz:

```
spi-tally: 345 frames / 345 irq  tx avg 389us (max 447us)  headroom 2513us  backlog 8
spi-tally: LATE 1 irq(s) arrived while busy — frames queued, not dropped (backlog 8, worst window 1)
```

(345/s is right: the block rate is **344.5 Hz**, not 44 — 128 frames per block
at 44.1 kHz, 2.902 ms per block.)

Both numbers come from ablspi itself, which ships in Ableton's GPL source drop
(see `docs/SPI_PROTOCOL.md`). `tx` is the driver's own `spi_tx_time`, stamped
after every transfer into `struct ablspi_sys_info` at the END of the mmap'd page
— one aligned 8-byte load of memory the shim already maps, no syscall. `irq` is
`/proc/ableton/ablspi0.0/irq_count`, read on the worker because it is file I/O.

**The gap between them is the point, and it exists because ablspi's IRQ is a
counting semaphore rather than a flag** (`atomic_inc` in the ISR, `atomic_dec`
in the wait). Overrun the budget and the frame is **not dropped** — it queues,
and the next waits return immediately, replaying back-to-back. So a late frame
never appears as a gap; it appears as a **burst**, which is exactly the shape
that gets attributed to somebody else's producer misbehaving. `backlog` is that
queue. Point it at the Link Audio dropouts before blaming Move's `Link Main`.

**Measured 2026-08-26, and it corrects the budget in `CLAUDE.md`'s Realtime
Safety section.** The ioctl takes
2569µs but only **389µs of that is the transfer** — the other ~2180µs is
`wait_event_interruptible` blocking for the next XMOS IRQ, i.e. frame pacing,
not work and not the wire. With ~140µs of our own compute (`pre` 97 + `post`
43), real slack is **~2370µs, not ~900µs**. See docs/REALTIME_SAFETY.md.

A corollary worth holding: **`total_us` is not a load signal.** Our work
growing shrinks the driver's wait by the same amount, so the loop total sits
near the period whatever we do. The old overrun counter compared it against
2000µs — below the 2902µs period — and so counted *every* frame: 43,986
"overruns" in two minutes on an idle device. Now `OVERRUN_THRESHOLD_US`.

Pure accumulator in `src/host/spi_tally.c` (no I/O, so `spi_tally_record` is
SPI-callback-safe and the whole thing is host-tested by
`tests/host/test_spi_tally.c`); `/proc` read and reporting in `shim_worker.c`.
**Arming gotchas, the measured table, and the two experiments still owed are in
`docs/plans/2026-08-26-spi-tally-followups.md`** — read it before re-measuring;
the tally stays silent for ~20 s after arming, which looks like a broken build.
The IRQ delta is a **32-bit** subtraction on purpose — that counter is printed
from an `int`, goes negative past 2^31 (~72 days) and wraps at 2^32, and only
modular arithmetic survives both. Widening it is the regression the test fails on.

## CPU usage page (`/system/cpu`, schwung-manager) — the one always-on diagnostic

Every switch above is off by default and armed by touching a file. This one is
not — **collection is unconditional**, and only the button on the page (`Measure
CPU`) arms anything.

**Why it can be unconditional: the timing was already being collected.**
`SHADOW_TIMING_LOG` is `0` in `src/schwung_shim.c`, but it gates only the
`fopen`/`fprintf` calls that used to write it to a log file. All 35
`clock_gettime` sites and 25 `TIME_SECTION_START` macros run on every SPI frame
regardless of that flag, today, whether or not anyone reads the result.
Publishing them costs one `mmap` at attach and two stores per ~1000 frames,
because `spi_snap` is now a **pointer** into `/schwung-perf` rather than a
static later copied there — no new memcpy. Arming collection the way every
other switch in this file arms would make the page blank until it warmed up,
which reads as a broken build — the exact failure the SPI tally documents above
("silent for ~20 s after arming"). What `Measure CPU` actually arms is the
manager's 1 Hz `/proc` polling — the only part of this feature with a real
cost — which runs `SCHED_OTHER` on cores 0–2 and never touches core 3.

**Measured on the device 2026-09-02, not assumed**, because the answer
determined whether the extra `clock_gettime` calls were free:

```
current_clocksource   : arch_sys_counter   (the only one available)
vdso mapped            : yes
kernel                 : 5.15.92-rt57-v8 aarch64 (PREEMPT_RT)

time.monotonic()  (vDSO clock_gettime)   340 ns/call
os.getppid()      (genuine syscall)     2133 ns/call
```

A real syscall costs **~1.8 µs** on this device; `clock_gettime` resolves in
the vDSO and is nowhere near that. The shim's ~78 `clock_gettime` calls/frame
are **≈2–3 µs of a 2902 µs frame — under 0.1%.** Worth stating the
counterfactual: **had the clocksource gone the other way**, glibc's vDSO path
falls through to a real syscall and 78 × 1.8 µs = **140 µs, 4.8% of every
frame, permanently, on the RT thread** — a pre-existing defect more
interesting than the feature itself, and invisible from the source; only
measuring on the device would have caught it.

**The page shows two numbers that must never be added together:**

- **Frame budget (core 3)** — per-module µs as a percentage of the measured SPI
  frame period (`frame_period_us`, not the 2902 µs nominal). This is what
  decides dropouts.
- **Process CPU (cores 0–3)** — ordinary `/proc` percentages for
  `MoveOriginal`, `link-subscriber`, `shadow_ui`, `jackd`, `schwung-manager`.

Adding them double-counts everything the shim does — `MoveOriginal`'s `/proc`
percentage already *includes* every module's SPI-callback time. They sit under
separate headings naming their cores for this reason, not decoration.

**MIDI FX are not separable.** They have no per-frame render — they run
event-driven inside the chain host's `on_midi` — so their cost lands inside the
`proc_midi` host section, attributed to no module. The page labels that row
rather than inventing a per-module figure for it.

**The segment**: `/schwung-perf`, `schwung_perf_snapshot_t` in
`src/host/perf_snapshot.h`, version 1, one page (880 bytes used of 4096 —
headroom is free, see "An SHM buffer sized to `sizeof` reads as FULL" in
`CLAUDE.md`). `magic` and `version` are the first two fields, so a short
segment left by an older shim can be version-checked without touching its
tail. A seqlock on `seq` (odd = writing) makes cross-process reads tear-free —
**both failure shapes retry**: a torn read makes a fresh attempt, an in-flight
write (odd `seq`) makes a fresh attempt, and after 3 attempts the reader
reports the read as **failed**, never as zeros — "shim not running" and
"everything idle" must never render the same picture. Shim and manager ship
together (as `install.sh` already does); a version mismatch shows an explicit
"deploy both" message instead of garbage.

**A failed read must never become a picture — and four did, found only by
loading the page in a browser, not by reading the code or the tests.**
Discarded `ok` flags rendered: `/proc/loadavg` unreadable as "Load average:
0.00 / 0.00 / 0.00"; `/proc/stat` unreadable as a table of headings with no
rows; an unreadable `/proc` as five named processes reported "not running";
an absent `MoveOriginal` as "None found" for realtime threads. All four wore
the costume of a real measurement. `schwung-manager/perf_render_test.go` pins
all four.

**Three tests hold this together:**
- `tests/host/test_perf_snapshot_size.c` — the container can only grow.
- `tests/host/test_perf_shm_offsets.sh` — compiles a probe that prints
  `offsetof` for every field and diffs it against the hand-mirrored Go offset
  block in `perf_shm.go`. This is the one that matters: `shmparams.go` carries
  the same kind of hand-mirrored layout, and when two `uint64`s were added to
  `shadow_param_t` the Go side went unupdated — every key landed 16 bytes late,
  the shim read an empty key, and every GET returned empty, with nothing
  crashing and no test failing. Drift in a hand-mirrored layout is silent and
  total.
- `schwung-manager/perf_render_test.go` — pins the four rendered-failure cases
  above.

See `docs/superpowers/specs/2026-09-02-manager-cpu-view-design.md` for the full
design.

### Cross-core attribution, and why names are useless

**A fork-parallel module hides from the frame budget.** JP-8000 (JE-8086)
calls `fork()` from `create_instance` — which IS the SPI callback — and the
child forks again once per pipeline stage, pinning each to a core. The
frame-budget table above only measures time spent *inside* the callback, so
it showed JP-8000 at **~5%** while its real DSP was running on cores 0–2 the
whole time. The frame budget was never wrong about what it measures; it was
never going to see this.

**The children cannot be identified by name.** A fork inherits its parent's
`comm`, and JP-8000 never calls `prctl(PR_SET_NAME)`. Observed on the device,
MoveOriginal's children were `display-server`, `schwung-manager`,
`link-subscriber`, `shadow_ui`, and one reporting as **`Audio Main/SPI`** —
the same name **six of MoveOriginal's own realtime threads use**. Filtering
by name cannot separate a JP-8000 pipeline worker from Move's own audio
thread; the two are indistinguishable in `/proc`.

So attribution walks the process **tree** instead, recursively — the stage
workers are grandchildren of MoveOriginal, and stopping at one level misses
them — subtracts the four shim helpers named above, and treats whatever CPU
remains as forked by a module. **Ownership is layered**: a module declaring
`capabilities.forks_processes` (see `docs/MODULES.md`) wins; failing that,
the page infers ownership when exactly one synth is loaded, always marked
**inferred** on screen so a guess never reads as a fact; failing that, the
remainder is shown as an unattributed group. A process is never hidden
because the page cannot name it — an unattributed row with real CPU is the
honest failure mode, not a silently absent one.

### Identity comes from disk

Naming positions was the obvious way to resolve `capabilities.forks_processes`
against a loaded module — ask the shim over the param channel which module is
in each slot. Measured cost: 12 requests per refresh, each served by the shim
**on the SPI callback**, and the `Param requests` section's own max went from
~36 µs to ~140 µs once the page started polling it that way. The page reads
module identity from the on-disk set state instead — no param round-trip on
the steady-state path.

**The schema is not what you would guess.** Every line below was got wrong
once before being checked against the actual files on the device:

```
active_set.txt     line 1 = set uuid    NOT the newest mtime. 27 sets existed;
                                        mtime order and glob order each pointed
                                        at a DIFFERENT wrong one.
slot_N.json        chain.synth.module   NOT synth.module
                   chain.audio_fx[].type   the key is "type", not "module"
master_fx_N.json   module_id            a different key again
```

A param read still happens, but only on a **contradiction**: disk reports a
position empty while the perf snapshot shows measured time for it — the
hot-swap window where the on-disk mirror is momentarily stale relative to
what is actually running. Every other refresh is disk-only.
