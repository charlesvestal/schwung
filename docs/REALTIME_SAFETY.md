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

### 4. Guard against thread accumulation

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

## What NOT to do

- Never call unified_log from the SPI callback path
- Never let child processes inherit FIFO scheduling from the shim
- Never pin compute threads to core 3
- Don't strip cap_sys_nice from rnbomovecontrol — RNBO needs it for FIFO 5-10
- Don't write to /tmp on the device (rootfs is full, use /data/UserData/)

## RNBO-internal XRuns (not our problem)

JACK reports XRuns from RNBO DSP clients (`fm-synth`, `Limiter-Stereo`, `move-volume`). These are RNBO's own graph exceeding the 128-frame budget. `rnbooscquery` burns ~45% CPU (RNBO's HTTP/WebSocket parameter server). We don't control this — it's internal to RNBO.
