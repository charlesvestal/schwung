# MIDI FX Enhancements Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add three enhancements: Mini-JV swap module placement, chord strum control, arp MIDI clock sync.

**Architecture:** Modifications to Shadow UI hierarchy handling and two MIDI FX modules (chord, arp).

**Tech Stack:** C (MIDI FX DSP), JavaScript (Shadow UI)

---

## Feature 1: Mini-JV "Swap Module" at Top Level

### Problem
Currently "Swap Module" appears at the bottom of every parameter list in the hierarchy editor. For modules with modes (like Mini-JV), it should appear at the mode selection level alongside "Patch" and "Performance".

### Solution
Modify Shadow UI's `loadHierarchyLevel()` to add "Swap Module" at the mode selection level instead of appending to every parameter list.

### UI Flow
```
Mini-JV Hierarchy Editor:
├── Patch          (select to enter patch mode)
├── Performance    (select to enter performance mode)
└── Swap module... (select to change sound generator)
```

### Files
- `src/shadow/shadow_ui.js` - modify `loadHierarchyLevel()` around lines 2466-2471

---

## Feature 2: Chord Strum Control

### Problem
Chord notes currently fire simultaneously. Musicians often want a "strum" effect where notes are slightly delayed from each other.

### Solution
Add `strum` (0-100ms) and `strum_dir` (up/down) parameters. Queue chord notes with increasing delays and emit them via the `tick()` function.

### Parameters
| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| strum | int | 0-100 | 0 | Milliseconds between successive notes |
| strum_dir | enum | up, down | up | Direction of strum |

### Data Structures
```c
typedef struct {
    uint8_t note;
    uint8_t velocity;
    int delay_samples;      // samples remaining until trigger
} pending_note_t;

// Add to chord_instance_t:
pending_note_t pending[8];  // max chord notes with headroom
int pending_count;
int strum_ms;               // 0-100
int strum_dir;              // 0=up, 1=down
```

### Logic
1. On note-on with chord type != none:
   - If strum_ms == 0: emit all notes immediately (current behavior)
   - If strum_ms > 0:
     - Emit first note immediately
     - Queue remaining notes with delays: strum_ms, strum_ms*2, etc.
     - If strum_dir == down, reverse the order
2. In tick():
   - Decrement delay_samples for each pending note
   - When delay_samples <= 0, emit the note and remove from queue

### Files
- `src/modules/midi_fx/chord/dsp/chord.c` - add strum logic
- `src/modules/midi_fx/chord/module.json` - add parameters to chain_params

---

## Feature 3: Arp MIDI Clock Sync

### Problem
The arpeggiator uses internal BPM timing. Users want to sync to Move's MIDI clock for tighter integration with the host tempo.

### Solution
Add `sync` parameter (internal/clock). When set to "clock", derive timing from incoming MIDI clock messages (0xF8) instead of internal BPM.

### Parameters
| Parameter | Type | Range | Default | Description |
|-----------|------|-------|---------|-------------|
| sync | enum | internal, clock | internal | Timing source |

### MIDI Clock Math
- MIDI clock sends 24 pulses per quarter note (24 PPQN)
- Division mapping:
  - division=1 (quarter): advance every 24 clocks
  - division=2 (eighth): advance every 12 clocks
  - division=4 (sixteenth): advance every 6 clocks

### Data Structures
```c
// Add to arp_instance_t:
int sync_mode;              // 0=internal, 1=clock
int clock_counter;          // counts 0xF8 messages
int clocks_per_step;        // 24/division
```

### Logic
1. In process_midi():
   - If msg[0] == 0xF8 (timing clock) and sync_mode == 1:
     - Increment clock_counter
     - If clock_counter >= clocks_per_step: trigger step, reset counter
   - If msg[0] == 0xFA (start): reset clock_counter and step
2. In tick():
   - If sync_mode == 1: skip internal timing accumulation
   - Still handle note-off timing for previously triggered notes

### Files
- `src/modules/midi_fx/arp/dsp/arp.c` - add clock sync logic
- `src/modules/midi_fx/arp/module.json` - add sync parameter

---

## Implementation Order

1. **Feature 1** (Shadow UI) - standalone, no dependencies
2. **Feature 2** (Chord strum) - standalone MIDI FX change
3. **Feature 3** (Arp clock) - standalone MIDI FX change

All three can be developed in parallel if desired.

## Testing

- **Feature 1:** Load Mini-JV in chain, verify swap module appears at mode level
- **Feature 2:** Set chord type, increase strum, verify notes are delayed
- **Feature 3:** Set sync=clock, change host tempo, verify arp follows
