# Shadow Instrument POC - Status

**Branch:** `feature/shadow-instrument-poc`
**Date:** 2025-01-15
**Status:** Incomplete - audio synchronization issues unresolved

## Goal

Run custom synth modules (SF2, DX7) alongside stock Ableton Move, mixing their audio output with Move's audio through the SPI mailbox.

## Architecture

```
Stock Move (ioctl) → Shim (LD_PRELOAD) → SPI Mailbox
                         ↓
                    Shared Memory IPC
                         ↓
                    shadow_poc process
                         ↓
                    DX7/SF2 synth module
```

### In-Process Shadow Mode (Autostart)

The shim can also load and run the DX7 DSP **inside MoveOriginal** without the
separate `shadow_poc` process. This mode autostarts when the shim sees the SPI
mailbox `mmap()` and mixes the DX7 audio directly into the mailbox.

**MIDI filter:** Only channel **5–8** messages are forwarded to the in-process
DX7. Set the Move track MIDI channel to 5, 6, 7, or 8 to drive the shadow synth.

### Components

1. **move_anything_shim.so** - LD_PRELOAD library that:
   - Hooks `mmap()` to capture mailbox address
   - Hooks `ioctl()` to intercept SPI communication
   - Creates shared memory segments for IPC
   - Forwards MIDI from mailbox to shadow process
   - Mixes shadow audio into mailbox audio output

2. **shadow_poc** - Standalone process that:
   - Loads synth module (DX7 or SF2) via dlopen
   - Receives MIDI from shared memory
   - Renders audio blocks
   - Writes audio to shared memory for shim to mix

### Shared Memory Segments

| Segment | Size | Purpose |
|---------|------|---------|
| `/move-shadow-audio` | 1536 bytes | Triple-buffered audio output (3 × 512) |
| `/move-shadow-movein` | 512 bytes | Move's audio for shadow to read |
| `/move-shadow-midi` | 256 bytes | MIDI from shim to shadow |
| `/move-shadow-display` | 1024 bytes | Display buffer (not implemented) |
| `/move-shadow-control` | 64 bytes | Control flags and sync counters |

## What Works

- ✅ Shim hooks mmap/ioctl successfully
- ✅ MIDI forwarding from Move to shadow_poc
- ✅ Synth module loading (DX7 loads and plays)
- ✅ Audio renders and can be heard
- ✅ Triple buffering implemented
- ✅ Drift correction via shim_counter
- ✅ In-process shadow DSP autostarts and filters MIDI to channels 5–8

## The Problem

**Audio sounds "warbly"** - like samples playing at wrong rate or buffers being skipped/repeated.

### Key Finding
- Same warbly issue occurs in both REPLACE mode (shadow only) and MIX mode
- DX7 (algorithmic synthesis) has same issue as SF2 (sample-based)
- DX7 sounds perfect when loaded directly through move-anything
- **Conclusion:** Issue is IPC timing, not the synth

### Attempted Solutions

1. **Simple handshake** (`audio_ready` flag) - Still warbly
2. **Triple buffering with drift correction** - Still warbly
3. **REPLACE mode** (eliminate mixing as variable) - Still warbly

### Root Cause Analysis

The fundamental problem: **two independent clocks cannot be synchronized for real-time audio without kernel-level support**.

- Move's audio runs at exactly 44100 Hz, triggered by XMOS hardware via ioctl
- shadow_poc runs its own loop with `usleep(2900)` - cannot match exactly
- Even with drift correction, context switches and scheduling jitter cause buffer mismatches
- Triple buffering smooths short-term jitter but doesn't fix the fundamental timing problem

## Potential Solutions (Not Implemented)

1. **Run synth in shim directly** - dlopen the module in the shim, call render_block() during ioctl. Eliminates IPC timing entirely. This is essentially what move-anything does.

2. **Use kernel audio infrastructure** - JACK/PipeWire for proper audio clock synchronization. Not available on Move.

3. **POSIX real-time scheduling** - `sched_setscheduler(SCHED_FIFO)`, `mlockall()`, semaphores. Complex and still may have issues.

4. **Larger ring buffer** - Accept 20-50ms latency to smooth out all timing variations.

## Files Modified

- `src/move_anything_shim.c` - Shadow IPC, triple buffering, drift correction
- `src/shadow/shadow_poc.c` - Standalone shadow process

## How to Test

```bash
# On Move, with shim installed:
cd /opt/move && LD_PRELOAD=/usr/lib/move-anything-shim.so ./Move &
cd /data/UserData/move-anything && ./shadow_poc &

# Press pads - you should hear DX7 (warbly)
```

## Conclusion

The shadow instrument architecture as designed cannot achieve clean audio due to the fundamental timing mismatch between independent processes. The recommended path forward is either:

1. Integrate synth rendering directly into the shim (no IPC)
2. Accept this as a proof-of-concept and focus on the main move-anything approach

The main move-anything project works correctly because it runs the synth in the same process as the ioctl handler, eliminating all timing coordination issues.
