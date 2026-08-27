# Realtime Safety and Audio Glitch Prevention

Findings from investigating RNBO/JACK audio glitches (2026-03-29/30). All fixes are in the codebase — no manual steps needed.

## System Architecture

| Component | Scheduling | Core | Notes |
|-----------|-----------|------|-------|
| SPI IRQ (`irq/54-ablspi_r`) | FIFO 91 | Core 3 | Hardware interrupt |
| SPI driver (`spi0`) | FIFO 90 | Core 3 | Pinned via mask 0x8 |
| MoveOriginal Audio/SPI threads | FIFO 70 | Various | Stock, can't change |
| JACK daemon | FIFO 10 | Various | Set by RNBO |
| rnbomovecontrol/rnbooscquery | FIFO 5 | Various | Self-boosted via cap_sys_nice |
| shadow_ui | SCHED_OTHER | Various | Reset from FIFO 70 at fork |

## Rules

### 1. No blocking I/O in SPI callback path

The SPI callback has ~900µs budget. Any file I/O can spike to 78ms when the disk is busy.

**Never call from the SPI path:**
- `unified_log()` (uses fprintf + fflush)
- `fprintf()`, `fopen()`, `fclose()`
- Any file system operation

**Instead:** Use a lock-free snapshot struct and a background thread that drains it on a timer (e.g., every 5 seconds). See `schwung_shim.c` SPI timing implementation.

### 2. Reset scheduling before exec

The shim runs via LD_PRELOAD inside MoveOriginal's threads (FIFO 70). Any forked child inherits this priority.

**Problem:** shadow_ui, host_system_cmd children, jack_midi_connect, and RNBO all inherited FIFO 70, starving lower-priority threads and causing audio glitches.

**Fix:** `shadow_process.c` resets to SCHED_OTHER before exec'ing shadow_ui. `shadow_ui.c` does the same for host_system_cmd children (fork + sched_setscheduler + exec, replacing system()).

### 3. Keep core 3 free for SPI

SPI runs at FIFO 90 on core 3. Other threads landing there cause cache/memory contention.

**Fix:** Pin compute-heavy processes to cores 0-2 with `taskset 0x7`. For RNBO, this is done in `rnbo-runner/ui.js` at launch and re-applied at frame 50.

### 4. Module entry points ARE the SPI callback

This rule is about third-party modules, and it is the one the ecosystem gets
wrong. `set_param`, `get_param`, `on_midi`/`process_midi` and
`render_block`/`process_block`/`tick` all run on the SPI callback. There is no
control thread.

**`create_instance` / `destroy_instance` are the exception, as of 2026-08, and
only for a chain slot's SOUND GENERATOR** — those are built on a normal-priority
loader thread (`src/modules/chain/dsp/chain_loader.c`) and published by the
audio thread. Audio FX, MIDI FX and Master FX still create inline. A module
must therefore assume **either** thread: it may block in create, it may not
touch anything a render can reach, and it may be destroyed without ever having
rendered. The full rule is in `plugin_api_v1.h` — including which host
callbacks are safe from a loader thread (`log` and the scalar queries) and
which are create-forbidden (the MIDI and modulation callbacks).

That change also disarms the inheritance trap below for anything a sound
generator spawns at create. It does **not** disarm it for `set_param` or
`render_block`, and it does not help a module that sets its own priority
rather than inheriting one — minijv asks for FIFO 45 explicitly.

A 2026-08 audit of all 113 catalogued modules found ~150 confirmed violations.
The instructive part is not the count, it is that several modules **document the
opposite**:

| module | comment | what it actually does |
|---|---|---|
| `magneto.c:572,838` | "Control-thread only (blocking dir + file I/O)"; "not the audio thread" | 10.6 MB `fwrite` from `on_midi`, and from a knob detent |
| `war_bells params.c:294` | "run on the control thread, NEVER from process_block — so this malloc is realtime-safe" | 21 MB `calloc` from `set_param`, reachable by MIDI CC |
| `breakbeat.c:431` | "safe because it runs in the MIDI callback (not the RT render thread)" | `open`+`mmap` of a WAV inside `render_block` |
| `dj_plugin.cpp:2501` | "outside the hot render path" | `free()` inside `render_block` |

So the failure mode is not carelessness — authors infer a plausible
architecture and nothing contradicts them. The contract is now stated at the
top of `src/host/plugin_api_v1.h` and in `docs/MODULES.md`; keep all three in
sync.

**Worst shapes seen, worth grepping any new module for:**

- `fork`/`exec` from `render_block` (`webstream`, `radiogarden`, `streamrtsp`)
- a synchronous sample/preset/ROM load inside `set_param` (`sf2`, `nam`,
  `granny`, `mrsample`, `slicer`, `surge`, `helm`, `hush1`, `mrdrums`)
- **`get_param` that rescans a directory** — served once per repaint, so it
  recurs per frame rather than per click (`dexed`, `sf2`, `sfz`, `obxd`,
  `midiverb`, `nam`, `noisemaker`)
- `pthread_create` with no `SCHED_OTHER` demotion — at least 14 modules
- unconditional `fprintf(stderr, …)` or `host->log` on a parameter path
- writing to **`/tmp`** on the device (`streamrtsp`, `airplay`); Move's rootfs is
  ~463 MB and usually full. Use `/data/UserData/`.

### 5. Guard against thread accumulation

Background processes launched from tick (like jack_midi_connect) can accumulate if they hang.

**Fix:** Use a pidfile guard — check if the previous instance is still running before launching a new one.

## Root Causes Found

### Blocking I/O in SPI callback (FIXED)

Sources removed:
- `schwung_shim.c`: heartbeat logging, timing logs, overrun warnings
- `shadow_led_queue.c`: sysex debug every 50th packet
- `schwung_jack_bridge.c`: stash fopen for first 50 MIDI events

### FIFO 70 inheritance (FIXED)

- shadow_ui inherited FIFO 70 from MoveOriginal
- Every host_system_cmd spawned children at FIFO 70
- jack_midi_connect launched with `&` inherited FIFO 70
- Fix: scheduling reset in shadow_process.c and shadow_ui.c

### RNBO threads on SPI core (FIXED)

- RNBO threads landing on core 3 contended with SPI FIFO 90
- Fix: CPU pinning to cores 0-2

### JACK audio bridge read misses (FIXED)

- bridge_read_audio busy-waited for JACK to deliver audio within ~50µs
- 0.34% miss rate (186 misses per 55K frames), each miss = audible click
- Fix: double-buffer in `schwung_jack_bridge.c`. bridge_wake snapshots previous frame into a static buffer; bridge_read_audio returns snapshot immediately (no waiting)
- Cost: +1 frame latency (~2.9ms), total JACK path ~5.8ms
- Result: 0.000% miss rate on device

## Key Measurements

| Metric | Before fixes | After fixes |
|--------|-------------|-------------|
| SPI ioctl baseline | ~2ms | ~2ms (hardware) |
| Pre-callback processing | ~100-150µs | ~100-150µs |
| Max frame time | 18-78ms (spikes) | ~2700µs |
| JACK audio misses | 0.34% | 0.000% |

Frame budget: 2900µs (128 frames @ 44.1kHz).

### The "~2ms transfer" is mostly the shim sitting idle — MEASURED 2026-08-26

The "SPI ioctl baseline ~2ms" row above is the *ioctl call*, not the transfer,
and the ~900µs budget derived from it is wrong. ablspi stamps the real transfer
duration into the mmap'd page every frame; reading it (`spi_tally_on`) against
the existing `[spi_timing]` line, on an idle device with three empty slots:

| | µs |
|---|---|
| Frame period (128 @ 44.1 kHz, **344.5 Hz** block rate) | 2902 |
| `ioctl` call, as the shim measures it | 2569 |
| …of which **actual transfer** (ablspi `spi_tx_time`) | **389** (max 447) |
| …of which **blocked in `wait_event_interruptible`** | **~2180** |
| Our compute (`pre` 97 + `post` 43) | ~140 |
| **Real slack** | **~2370** |

Most of that ioctl is the driver waiting for the *next* XMOS IRQ — it is the
frame pacing, not work and not the wire. So the budget is ~2.4 ms, not ~900 µs.

`total_us` is pinned near the period for the same reason: our work growing
shrinks the wait by the same amount. **It is not a load signal** — use
`mix_buf`/`render` in `[spi_timing] Pre/Post`, or the tally's `backlog`.

**An overrun does not drop a frame — it queues.** ablspi's IRQ is a counting
semaphore (`atomic_inc` in the ISR, `atomic_dec` in the wait), so frames that
fire while we are busy are serviced back-to-back afterwards. Two consequences
worth holding onto when reading the table above and the module-load section
below: "232 consecutive dropped frames" is really 232 frames of *deferral* that
Move then works off in a burst; and a burst arriving at a downstream consumer
(Link Audio, the JACK bridge) is evidence *for* one of our own overruns, not
against it. `backlog` in the tally is that queue, measured.

## What NOT to do

- Never call unified_log from the SPI callback path
- Never let child processes inherit FIFO scheduling from the shim
- Never pin compute threads to core 3
- Don't strip cap_sys_nice from rnbomovecontrol — RNBO needs it for FIFO 5-10
- Don't write to /tmp on the device (rootfs is full, use /data/UserData/)

## RNBO-internal XRuns (not our problem)

JACK reports XRuns from RNBO DSP clients (`fm-synth`, `Limiter-Stereo`, `move-volume`). These are RNBO's own graph exceeding the 128-frame budget. `rnbooscquery` burns ~45% CPU (RNBO's HTTP/WebSocket parameter server). We don't control this — it's internal to RNBO.

## The shim worker (2026-06)

Work the SPI callbacks must not do lives on the shim worker thread
(`src/host/shim_worker.c`, SCHED_OTHER, cores 0-2): debug-flag file polling
(published as a bitmask the RT path reads), trigger-file consumption,
deferred events posted from RT via an SPSC ring (overtake exit hooks, Move
restart, sampler prepare/finalize/cancel, skipback save/resize, preview
load), and the current-set filesystem scan (results delivered through a
seqlock snapshot consumed on the SPI thread). When adding RT-path work that
needs file I/O or process spawning: post an event, add a hook.

## Module load blocks the SPI callback — MEASURED at 673 ms

`set_param("<id>:module")`, `load_patch`, `load_file` and Master-FX slot loads
reach `dlopen` / `fopen` / `calloc` and the plugin's own `create_instance` from
the SPI thread. This was recorded here as an "accepted tradeoff" with an
"audible hiccup". It is not a hiccup.

**Measured on hardware 2026-08-21, loading minijv from the chain editor:**

```
param  = 678 / 672889 us     one param request took 672.9 ms
total  max = 673487 us       the whole frame
pre    max = 673004 us       essentially all of it before the transfer
ioctl  max =   2932 us       the transfer itself was normal
```

Against a **2900 us** frame budget that is **~232 consecutive dropped frames**.
Idle `param` max is 35-80 us, so a load is roughly four orders of magnitude
over. Corroborated independently one line earlier in the same log by a
different subsystem: `[link_subscriber] cbgap slot=3 674.3 ms`. The whole
system stops, not just the slot being loaded.

**Where the time goes.** Not `dlopen`, and not file I/O. minijv loads ROMs
inside `create_instance`, and **the plugin API has no realtime contract** —
`create_instance` may do anything, most modules ship outside this tree, and
the cost is therefore unbounded and unknowable from here. Any budget set
against modules in this repo is a lower bound.

**The previous entry cited a design that does not exist.** It pointed at
`docs/plans/2026-06-11-codebase-cleanup-review.md`; that file's only mention
(line 103) is a one-line TODO to *write this section*. The citation was
circular, and "don't re-investigate" then discouraged anyone from finding out
the number was 673 ms.

**The design does exist, in code.** `SHIM_EVT_OVERTAKE_DSP_LOAD`
(`src/schwung_shim.c:1570-1805`) already does loader-thread + staged instance +
detach-then-defer free for overtake modules, including a deliberate
leak-on-timeout because "a leak is recoverable; a use-after-free in the audio
path is not". The chain path never adopted it. See residual 2.6.

### FIXED for the synth position (2026-08-27)

`src/modules/chain/dsp/chain_loader.c`. A `synth:module` write now *asks*; a
SCHED_OTHER loader thread builds the instance into a staging record no render
path can reach; `v2_render_block` publishes it by swapping pointers. The
outgoing module keeps rendering for the whole load, so a swap costs no silence.

Two properties are load-bearing and worth not undoing:

- **Neither side takes a lock.** An RT thread blocking on a mutex held by a
  SCHED_OTHER thread is unbounded priority inversion — the defect the
  2026-08-22 audit flagged in minijv's `ring_mutex`. The SPI-side entry points
  are atomic stores and nothing else: no mutex, no condvar, no semaphore, no
  syscall. The loader is woken by polling, which costs a wakeup every 20 ms
  against a load measured in hundreds.
- **No field has two writers.** A first version kept one `state` word both
  threads wrote; it lost requests and leaked handles depending on which store
  landed first. Work is derived from generation counters, each advanced by one
  side only. `tests/host/test_chain_deferred_load.sh` pins both, and pins that
  a `synth:module` write actually reaches the loader.

This also gives `synth:is_loading` its first real implementation — the shadow
UI has consumed that key for months against a host that never served it.

**Still on the synchronous path:** audio FX, MIDI FX (both chain positions) and
Master FX slots. `load_patch` and boot restore also still load inline, which is
deliberate — they are not interactive, and the patch path is already
fade-masked to silence.

**What makes the port non-trivial** (all verified, not assumed):
- The module triple (`handle` / `plugin_v2` / `instance`) is a check-then-call
  over three non-atomic words read from `chain_midi.c` in five places. Off
  thread, a `dlclose` under a reader is a jump into an unmapped segment.
- Chain instances are touched on **both** sides of the ioctl — `mix_audio` and
  `forward_midi` before, `render_block` / `shadow_chain_process_fx` after — so a
  worker must be excluded from two windows separated by a ~2 ms blocking call,
  not one.
- Four **synchronous read-backs** live inside the param handler in C
  (`shadow_chain_mgmt.c:3341`, `:3362`, `:3388`, `:3406`). The
  `default_forward_channel` one fails *silently* if the load is deferred: wrong
  MIDI routing, no error, no crash.
- The patch path is already fade-masked to silence before loading; the
  `<id>:module` write path is not, and that is the one users hit.
