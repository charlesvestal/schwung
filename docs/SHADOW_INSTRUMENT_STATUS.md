# Shadow Instrument POC - Status

**Branch:** `feature/shadow-instrument-poc`
**Date:** 2025-01-16
**Status:** Audio resolved - in-process shadow DSP working

## Goal

Run custom signal chain patches alongside stock Ableton Move, mixing their audio output with Move's audio through the SPI mailbox.

## Architecture

```
Stock Move (ioctl) → Shim (LD_PRELOAD) → SPI Mailbox
                         ↓
                In-process chain DSP
```

### In-Process Shadow Mode (Autostart, Current)

The shim loads and runs the chain DSP **inside MoveOriginal** without the
separate `shadow_poc` process. This mode autostarts when the shim sees the SPI
mailbox `mmap()` and mixes the chain audio directly into the mailbox.

**MIDI routing:** Channel **5–8** are routed to four independent chain instances:
5 → Slot A, 6 → Slot B, 7 → Slot C, 8 → Slot D.

**Config:** `/data/UserData/move-anything/shadow_chain_config.json`

Example:
```json
{
  "patches": [
    { "name": "SF2 + Freeverb (Preset 1)", "channel": 5 },
    { "name": "DX7 + Freeverb", "channel": 6 },
    { "name": "OB-Xd + Freeverb", "channel": 7 },
    { "name": "JV-880 + Freeverb", "channel": 8 }
  ]
}
```

### Components

1. **move_anything_shim.so** - LD_PRELOAD library that:
   - Hooks `mmap()` to capture mailbox address
   - Hooks `ioctl()` to intercept SPI communication
   - Creates shared memory segments for IPC
   - Forwards MIDI from mailbox to shadow process
   - Mixes shadow audio into mailbox audio output

2. **chain dsp.so (in-process)** - Loaded directly by the shim:
   - Scans chain patches in `modules/chain/patches`
   - Loads selected patches for each slot
   - Renders audio blocks inline

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
- ✅ Chain module loading (patches load and play)
- ✅ Audio renders cleanly (in-process)
- ✅ In-process chain autostarts and routes MIDI to channels 5–8
- ✅ Shadow audio mixes with stock Move audio in the mailbox

## Audio Status

Audio is now clean and stable by running the chain DSP **in-process** inside the shim.
This eliminates IPC timing drift entirely.

## Files Modified

- `src/move_anything_shim.c` - In-process chain loader and routing
- `src/modules/chain/patches/*.json` - Shadow test patches

## How to Test

```bash
# On Move, with shim installed:
cd /opt/move && LD_PRELOAD=/usr/lib/move-anything-shim.so ./Move &

# Set track MIDI channel to 5–8 and play pads
```

## Conclusion

In-process shadow DSP is the working path: it autostarts in the shim, renders
clean audio, and mixes with stock Move output. MIDI is gated to channels 5–8 to
avoid UI events.
