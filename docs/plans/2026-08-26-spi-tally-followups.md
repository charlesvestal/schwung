# SPI frame tally — what it measured, and the two tests still owed

2026-08-26. Companion to PR #296. The tool is `spi_tally_on`; this file is the
part that is *not* in the code: what was measured, what it invalidated, and the
two experiments that were not run.

## Arm it

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "touch /data/UserData/schwung/spi_tally_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log | grep spi-tally"
```

**Both flags are required** and the order matters in practice: `unified_log`
only starts accepting after it notices `debug_log_on`, and it rechecks every 100
calls. The tally refuses to measure into a log that is still dropping writes
(same rule as `rt_thread_audit`), so for the first several seconds after arming
you get *nothing* — which looks exactly like a broken build. Wait ~20 s before
concluding anything. This cost a cycle the first time.

Disarm both when done. `debug_log_on` writes ~270 B/s with the tally up, and
leaving it armed has previously caused the dropouts it was hunting.

## Reading a line

```
spi-tally: 345 frames / 345 irq  tx avg 389us (max 464us)  headroom 2513us  backlog 6
spi-tally: LATE 1 irq(s) arrived while busy — frames queued, not dropped (backlog 6, worst window 1)
```

- **345/s is correct.** The block rate is 344.5 Hz — 128 frames per block at
  44.1 kHz, 2.902 ms per block. Not 44. (I got this wrong first time and it is
  an easy mistake: "128 frames" is the block size, not the rate.)
- **`backlog` is cumulative since arming**, not a level. It only rises.
- `frames > irqs` in a window is the queue *draining* and is reported as clean.
- `max` is the worst transfer since arming and never resets per-window — a
  spike once an hour is the finding, and a per-window max throws it away.

## What it measured — idle device, three empty slots

| | µs |
|---|---|
| Frame period (344.5 Hz block rate) | 2902 |
| `ioctl`, as `[spi_timing]` measures it | 2569 |
| …of which **actual transfer** (ablspi `spi_tx_time`) | **389** (max 464) |
| …of which **blocked in `wait_event_interruptible`** | **~2180** |
| Our compute (`pre` 97 + `post` 43) | ~140 |
| **Real slack** | **~2370** |

Backlog at idle creeps ~1 per 10 s, each a single-frame lateness.

### Two things this invalidated

1. **The ~900 µs frame budget.** It was derived from "SPI ioctl baseline ~2ms",
   which is the *ioctl*, not the transfer — most of it is the driver waiting for
   the next XMOS IRQ. Real slack is ~2370 µs. Every "X% of budget" figure
   against 900 µs is ~2.6× too pessimistic.
2. **`total_us` as a load signal.** It is not one. The loop is paced by the
   blocking ioctl, so our work growing shrinks the wait by exactly as much and
   the total stays pinned near the period whatever we do. Use `mix_buf` /
   `render` from `[spi_timing] Pre/Post`, or `backlog`.

That second point is why the old overrun counter tested `total_us > 2000` — below
the 2902 µs period — and counted **43,986 overruns in two minutes on an idle
device**, while sitting on the same log line as `ioctl avg=2568` looking like
corroboration. Fixed; verified 43,986 → 1.

## Test 1 — the loaded reading (the one that matters)

**Not run.** Doing it would have meant loading modules into a live set.

Question: when Link Audio stalls, is `backlog` climbing in the same windows?

Method: reproduce the dropouts per
`memory/link_audio_producer_burst_dropouts.md` (minijv + keydetect +
airwindows was the configuration that did it, ~1 stall per 4–40 s), arm the
tally alongside the sidecar's `max_gap` / `max_burst_run` logging, and line the
timestamps up.

- **Backlog climbs with the stalls** → our own frame overruns are implicated,
  and the tally is a far cheaper probe than the `[spi_timing] mix_buf`
  instrumentation currently used to watch the load knob.
- **Backlog stays flat through them** → our SPI servicing is fine and the stall
  is entirely Move-side (`Link Main` at FIFO 35 missing its timer, then
  `SinkProcessor` draining its 128-slot queue in one greedy burst). That is the
  standing diagnosis and this would corroborate it independently.

**Do not conflate the two queues.** ablspi's IRQ semaphore and Move's sink
queue *both* turn a delay into a catch-up burst. Two burst-shaped phenomena in
one system is exactly how you talk yourself into the wrong one.

## Test 2 — re-derive the tapedelay instance ceiling

`memory/tapedelay_cpu_two_instances.md` says Tape Echo 2 is 41% of budget and
only two instances fit. The per-instance measurement (0.36–0.37 ms/block) is
unaffected by any of this; the **denominator** was 900 µs. Against ~2370 µs it
is ~15%.

Naively that is ~6 instances. **Do not publish that number from division.**
Re-run `tools/bench_instances.cpp`. The slack has other claimants — all four
slots, master FX, mixing — and `Link Main` starving at FIFO 35 is a ceiling that
has nothing to do with arithmetic. The memory is flagged superseded-in-part
rather than rewritten, deliberately.

## Things worth knowing before extending this

- **`spi_tally_record` runs on the SPI callback.** `src/host/spi_tally.c` is
  pure — no I/O, allocation or locks — and must stay that way. `/proc` reading
  and logging live on the worker.
- **The handoff takes deltas of cumulative counters** rather than draining a
  window the producer is still filling. Draining would lose whichever sample
  landed between the read and the reset, and the sample most worth keeping is
  the outlier.
- **The IRQ delta must stay a 32-bit subtraction of 32-bit operands.** Signed
  vs unsigned is irrelevant (two's complement); the **width** is what makes it
  modular. Widening `prev_irqs` / `spi_tally_sample_t::irqs` "for consistency
  with `frames`" turns the counter's 2^32 wrap (~72 days at 344.5 Hz) into a
  4-billion-IRQ window. `tests/host/test_spi_tally.c` fails on exactly that;
  the signedness mutation is *equivalent* and will not fail it, which is worth
  knowing before you go hunting for why.
- **`failed_send_count`** in `/proc/ableton/ablspi0.0/` is always 0. Nothing in
  the driver ever increments it. Do not build anything on it.

## Source

The driver is not reverse-engineered any more — `ablspi` ships in Ableton's GPL
source release for Move, in the patched `linux-raspberrypi-5.15.92-rt57` tree:

```
drivers/spi/ablspi-{core,gpio,proc}.c, ablspi{,-ioctl}.h
arch/arm64/boot/dts/overlays/ablspi-overlay.dts     # IRQ on GPIO 3
.kernel-meta/configs/rpi4-ablspi.cfg                # CONFIG_ABLSPI=m
```

Driver-derived facts are marked *(driver)* in `docs/SPI_PROTOCOL.md`.
