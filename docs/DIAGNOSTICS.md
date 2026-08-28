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
