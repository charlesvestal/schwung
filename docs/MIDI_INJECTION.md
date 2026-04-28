# MIDI Injection and Echo Filter Problem

> **Status: historical.** This document captures the design journey
> behind injecting transformed MIDI back into Move's native synth
> engine, including the multi-pad echo race that we never fully solved
> via refcounting. The shipped solution is documented in
> [ADDRESSING_MOVE_SYNTHS.md](ADDRESSING_MOVE_SYNTHS.md); read this
> file only if you need the historical context for why that API looks
> the way it does.
>
> **For external-MIDI channel remapping specifically:** the cable-2
> echo cascade described in this doc is the *reason* there's now a
> shim-side `/schwung-ext-midi-remap` SHM that overtake modules use to
> rewrite cable-2 channels before Move sees them. See CLAUDE.md
> "Cable-2 Channel Remap" and `host_ext_midi_remap_*` in `docs/API.md`.
> Do not re-inject from `onMidiMessageExternal` — it cascades.

Documents attempts to inject MIDI events into Move's SPI mailbox to make shadow chain MIDI FX (chord, arp, strum) play on Move's native synth engine.

## Goal

Press a pad on Move → shadow chain MIDI FX transforms the note → injected notes play on Move's native instruments.

## Architecture

```
Pad press (cable-0) → Move plays note → MIDI_OUT cable-2 (pitched note)
  ↓
Shim reads MIDI_OUT → echo filter → dispatch to chain → MIDI FX process
  ↓
MIDI FX output → midi_fx_out_buf (shared buffer) → shim reads
  ↓
Shim injects into MIDI_IN cable-2 → Move plays injected notes
  ↓
Move echoes injected notes on MIDI_OUT cable-2 → LOOP
```

## What Works

- **Single pad chord**: Press one pad, Move plays a chord via native synth. Solid.
- **8-byte MIDI_IN stride**: Correctly uses 8-byte events (4-byte USB-MIDI + 4-byte timestamp).
- **Monotonic timestamps**: Scan MIDI_IN for max timestamp, inject at max+1.
- **Cable-2 injection**: Move's synth plays cable-2 notes from SPI mailbox even without a physical USB device.
- **Additive/replacing mode**: MIDI FX modules declare mode via `get_param("midi_fx_mode")`.

## What Doesn't Work

- **Multi-pad chords**: 3 simultaneous pads → echo filter fails → cascade → crash or stuck notes.
- **Arp to Move**: Deferred (same echo loop problem, worse in replacing mode).

## The Echo Filter Problem

### The Loop

Every note injected into MIDI_IN cable-2 gets echoed back on MIDI_OUT cable-2 by Move. The echo gets dispatched to the chain, re-processed by the MIDI FX, and generates MORE injected notes — exponential cascade.

### The Fundamental Tension

| Approach | Single pad | Multi-pad | Why it fails |
|----------|-----------|-----------|--------------|
| Refcount without timing | Works | Broken | Note-offs set refcounts that consume real pad presses for same pitch |
| Refcount with timing window | Works | Broken | MIDI_OUT buffer overflow (20 slots shared with cable-0 LEDs) drops/delays echoes → stale refcounts |
| Overwrite buffer | Works | Broken | Only last chord survives, others lost |
| Accumulating buffer | Works | Crashes | Any single echo leak → exponential cascade → crash |

### All Approaches Tried

**1. Refcount in chain_host with timing window (age 1-3 frames)**
Works for single pad. Multi-pad echoes delayed by MIDI_OUT buffer contention arrive outside window.

**2. Wider timing window (age 1-10 frames)**
Better, but MIDI_OUT can have 11+ cable-0 events per frame, starving cable-2 echo slots. Dropped echoes → stale refcounts.

**3. Buffer overwrite (16 slots) with per-batch refcounts**
Single pad works. Multi-pad: only last chord's buffer survives. Orphaned refcounts block note-offs → stuck notes.

**4. memset(injected, 0) before each batch write**
Fixes orphans BUT destroys active chord's pending refcounts when a note-off chord overwrites the buffer mid-frame.

**5. Refcount tracking at read time (chain_read_midi_fx_output)**
Fixes orphan problem (only notes read get refcounts). But overwrite buffer still loses multi-pad notes.

**6. Accumulating buffer (32 slots) + read-time refcounts + age=0 preservation**
All chords injected. But stale refcounts from dropped MIDI_OUT echoes → stale events pass through → re-chorded → exponential cascade → crash.

**7. Shim-side echo filter (mfx_echo_refcount[128])**
Refcount at injection point, decremented at MIDI_OUT dispatch. No timing window. Problem: note-off injection sets refcount, then next REAL pad press for same pitch consumed as "echo." Result: first chord works, then silence.

### Key Discovery: MIDI_OUT Buffer Contention

MIDI_OUT has only 20 slots per frame. Cable-0 events (LED updates, button state) routinely consume 11+ of these. Cable-2 echoes compete for remaining slots and often get dropped entirely. This makes any refcount-based echo filter unreliable — you can't decrement a refcount for an echo that was never delivered.

## Approaches Not Yet Tried

1. **Separate ON/OFF refcounts**: `echo_on[128]` and `echo_off[128]` in shim. Only a note-ON echo consumes a note-ON refcount. Prevents note-off injections from blocking future note-on pad presses.

2. **Zero echoes in raw MIDI_OUT buffer**: Instead of filtering at dispatch, zero out cable-2 events in the raw hardware MIDI_OUT buffer that match recently injected pitches. Prevents echoes from reaching any code path.

3. **Cable-0 injection**: Inject as cable-0 with pad note numbers (68-99). Cable-0 is hardware input — Move wouldn't echo it. Avoids echo entirely. But needs pad-to-pitch mapping that respects Move's scale/key/octave settings.

4. **Frame-level gating**: Only dispatch cable-2 notes to chain in frames where a NEW cable-0 pad press was detected. Block all cable-2 in echo-only frames. Simple, no per-note tracking. Risk: blocks legitimate cable-2 if timing differs.

5. **Hybrid ON/OFF separation + stale timeout**: Approach #1 plus a safety valve: clear any refcount persisting >20 frames (~58ms).

## USB-MIDI Packet Format

```
Byte 0: (cable << 4) | CIN
Byte 1: status
Byte 2: data1
Byte 3: data2

Cable-2 note-on:  0x29, 0x90|ch, note, vel
Cable-2 note-off: 0x28, 0x80|ch, note, 0x40

CIN: 0x09 = note-on, 0x08 = note-off, 0x0B = CC
```

## Key Files

- `src/modules/chain/dsp/chain_host.c` — MFX buffer write, tick redirect, inject_note_off
- `src/schwung_shim.c` — echo refcount, pending buffer, drain, MIDI_OUT dispatch filtering
- `src/host/shadow_chain_mgmt.c/.h` — dlsym for chain exports
- `src/host/shadow_midi.c` — General MIDI injection drain

## Flag File

Enable MIDI FX rewrite: `touch /data/UserData/schwung/midi_fx_rewrite_on`
