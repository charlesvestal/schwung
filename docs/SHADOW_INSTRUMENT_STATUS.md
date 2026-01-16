# Shadow Instrument POC - Status

**Branch:** `feature/shadow-instrument-poc`
**Date:** 2025-01-16
**Status:** Audio resolved - in-process shadow DSP working

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

### In-Process Shadow Mode (Autostart, Current)

The shim can load and run the DX7 DSP **inside MoveOriginal** without the
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
- ✅ Synth module loading (DX7 loads and plays)
- ✅ Audio renders cleanly (in-process)
- ✅ In-process shadow DSP autostarts and filters MIDI to channels 5–8
- ✅ Shadow audio mixes with stock Move audio in the mailbox

## Audio Status

Audio is now clean and stable by running the DX7 **in-process** inside the shim.
This eliminates IPC timing drift entirely.

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

In-process shadow DSP is the working path: it autostarts in the shim, renders
clean audio, and mixes with stock Move output. MIDI is gated to channels 5–8 to
avoid UI events.
