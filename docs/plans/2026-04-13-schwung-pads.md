# Schwung Pads Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the "Extended Pads" feature (bank-selectable 16-pad grid) with "Schwung Pads" — all 32 pads send chromatic notes, Schwung controls the pad entirely, and notes record to Move's sequencer.

**Architecture:** Schwung Pads overwrites cable-0 pad events in MIDI_IN to cable-2 with remapped chromatic notes. Move sees these as external MIDI, plays them on its native synth, and records them to the sequencer. The echo filter suppresses cable-2 echoes from MIDI_OUT while letting sequencer playback through. Direct dispatch to the Schwung slot provides immediate playback on the shadow synth.

**Tech Stack:** C (shim + host), JavaScript/MJS (shadow UI)

**Branch:** `feature/schwung-pads` (based on main + cherry-picked extended-pads commits)

---

### Task 1: Rename `extended_pads` → `schwung_pads` across all files

This is a mechanical rename — no logic changes. Rename the field, param keys, UI labels, state persistence keys, and variable names.

**Files:**
- Modify: `src/host/shadow_chain_types.h:37` — struct field
- Modify: `src/host/shadow_constants.h:161` — UI state field
- Modify: `src/host/shadow_chain_mgmt.c:364,471-478,538,1632-1635,1664-1665` — init, parse, state sync, get/set
- Modify: `src/host/shadow_state.c:215-219,346-363` — state save/load JSON keys
- Modify: `src/schwung_shim.c:396,401,403-407,762,776,798,827,848,5064` — shim logic
- Modify: `src/shadow/shadow_ui_slots.mjs:35,81` — UI setting

**Step 1: Rename struct fields**

In `src/host/shadow_chain_types.h:37`:
```c
// Change:
int extended_pads;      /* 1 = forward right-16 pads ... */
// To:
int schwung_pads;       /* 1 = Schwung controls pads: 32 chromatic notes via cable-2 overwrite */
```

In `src/host/shadow_constants.h:161`:
```c
// Change:
uint8_t slot_extended_pads[SHADOW_UI_SLOTS]; /* 1=extended pads mode active */
// To:
uint8_t slot_schwung_pads[SHADOW_UI_SLOTS]; /* 1=Schwung pads mode active */
```

**Step 2: Rename in shadow_chain_mgmt.c**

All `extended_pads` → `schwung_pads`, all `slot:extended_pads` → `slot:schwung_pads`, all `slot_extended_pads` → `slot_schwung_pads`.

Lines: 364, 471-478, 538, 1632-1635, 1664-1665.

**Step 3: Rename in shadow_state.c**

JSON key `"slot_extended_pads"` → `"slot_schwung_pads"` in save (line 215) and load (line 347).
Log message (line 359): `"Loaded slot extended_pads"` → `"Loaded slot schwung_pads"`.

**Step 4: Rename in schwung_shim.c**

- `extended_pads_bank` → `schwung_pads_bank` (line 396) — will be removed in Task 2 but rename for now
- `ext_pads_echo_refcount` → `schwung_pads_echo_refcount` (lines 401, 946-947, 4811, 4835, 5065)
- `slot_extended_pads()` → `slot_schwung_pads()` (lines 403-407, 4762)
- `skip_extended_pads` → `skip_schwung_pads` (lines 4776, 4848)

**Step 5: Rename in shadow_ui_slots.mjs**

Line 35: `"slot:extended_pads"` → `"slot:schwung_pads"`, label `"Ext Pads"` → `"Schwung Pads"`.
Line 81: `"slot:extended_pads"` → `"slot:schwung_pads"`.

**Step 6: Build**

Run: `./scripts/build.sh`
Expected: Clean build, no errors.

**Step 7: Commit**

```bash
git add -A && git commit -m "refactor: rename extended_pads to schwung_pads across codebase"
```

---

### Task 2: Simplify pad mapping — all 32 pads send chromatic notes

Replace the bank-select (left 16) + note-trigger (right 16) logic with a simple 32-pad chromatic mapping. All pads overwrite in-place from cable-0 to cable-2.

**Files:**
- Modify: `src/schwung_shim.c:396,4759-4848` — pad scanning and overwrite logic

**Step 1: Remove bank state**

Delete `schwung_pads_bank[SHADOW_CHAIN_INSTANCES]` array (line 396). It's no longer needed.

**Step 2: Rewrite pad scan loop (lines 4759-4848)**

Replace the entire extended pads block with simplified chromatic mapping:

```c
/* === SCHWUNG PADS: overwrite all 32 pad events to chromatic cable-2 notes ===
 * Pads 68-99 → MIDI notes 36-67 (C2 to G4).
 * Overwrites cable-0 events in-place in sh_midi (shadow's copy of MIDI_IN)
 * with cable-2 + remapped note. Move records the cable-2 notes to sequencer.
 * Direct dispatch to Schwung slot for immediate playback.
 * Echo filter in MIDI_OUT suppresses real-time echoes while letting
 * sequencer playback (no refcount) pass through. */
{
    int es = shadow_selected_slot;
    if (es >= 0 && es < SHADOW_CHAIN_INSTANCES &&
        slot_schwung_pads(es) &&
        shadow_plugin_v2 && shadow_plugin_v2->on_midi &&
        shadow_chain_slots[es].instance) {
        /* Lazy activation: same as shadow_chain_dispatch_midi_to_slots */
        if (!shadow_chain_slots[es].active) {
            char buf[64];
            int len = shadow_plugin_v2->get_param(
                shadow_chain_slots[es].instance, "synth_module", buf, sizeof(buf));
            if (len > 0) {
                if (len < (int)sizeof(buf)) buf[len] = '\0';
                else buf[sizeof(buf) - 1] = '\0';
                if (buf[0] != '\0')
                    shadow_chain_slots[es].active = 1;
            }
            if (!shadow_chain_slots[es].active) goto skip_schwung_pads;
        }
        /* Scan at 8-byte stride matching MIDI_IN event size (31 events max) */
        for (int j = 0; j < 248; j += 8) {
            if ((hw_midi[j] & 0xFF) == 0) break;  /* Empty = end of events */
            uint8_t cable = (hw_midi[j] >> 4) & 0x0F;
            if (cable != 0) continue;
            uint8_t cin = hw_midi[j] & 0x0F;
            if (cin != 0x09 && cin != 0x08) continue;
            uint8_t st = hw_midi[j + 1];
            uint8_t type = st & 0xF0;
            if (type != 0x90 && type != 0x80) continue;
            uint8_t d1 = hw_midi[j + 2];
            uint8_t vel = hw_midi[j + 3];

            /* Only intercept pad notes (68-99) */
            if (d1 < 68 || d1 > 99) continue;

            /* Map pad 68-99 → MIDI note 36-67 (C2 to G4) */
            uint8_t remapped = (d1 - 68) + 36;

            /* Overwrite in sh_midi: cable 2, remapped note.
             * Move records this for sequencer. Echo filtered in MIDI_OUT. */
            sh_midi[j]   = cin | 0x20;  /* cable 2 */
            sh_midi[j+1] = st;
            sh_midi[j+2] = remapped;
            sh_midi[j+3] = vel;
            schwung_pads_echo_refcount[remapped]++;

            /* Dispatch directly to slot for immediate playback.
             * Apply forward channel remapping to match normal dispatch path. */
            uint8_t remapped_st = shadow_chain_remap_channel(es, st);
            uint8_t msg[3] = { remapped_st, remapped, vel };
            shadow_plugin_v2->on_midi(
                shadow_chain_slots[es].instance, msg, 3,
                MOVE_MIDI_SOURCE_EXTERNAL);
        }
    }
}
skip_schwung_pads: ;
```

**Step 3: Build**

Run: `./scripts/build.sh`
Expected: Clean build.

**Step 4: Commit**

```bash
git add src/schwung_shim.c && git commit -m "feat: schwung pads — 32 chromatic notes replacing bank grid"
```

---

### Task 3: Set pad LEDs to white when Schwung Pads is active

When the selected slot has schwung_pads enabled, set all 32 pad LEDs (notes 68-99) to white (120). This should happen:
- When switching to a track that has schwung_pads enabled
- Move handles clearing LEDs when switching away to a non-schwung-pads track

**Files:**
- Modify: `src/schwung_shim.c` — add LED setting in the track-switch handler and schwung_pads activation

**Step 1: Add LED init when switching to a schwung_pads slot**

In the track-switch handler (around line 5060, where `shadow_selected_slot` is updated), after the echo refcount reset, add:

```c
/* Set Schwung Pads LEDs when switching to a slot with schwung_pads enabled */
if (slot_schwung_pads(new_slot)) {
    for (int p = 68; p <= 99; p++) {
        shadow_queue_led(0x09, 0x90, p, 120);  /* White */
    }
}
```

**Step 2: Build and commit**

Run: `./scripts/build.sh`

```bash
git add src/schwung_shim.c && git commit -m "feat: set pad LEDs white when switching to schwung_pads slot"
```

---

### Task 4: Update echo filter range comment

The echo filter at shim line ~942 already works generically (any note with a refcount). Just update the comment to reflect that it now covers notes 36-67 instead of the old bank range.

**Files:**
- Modify: `src/schwung_shim.c:942-944` — comment only

**Step 1: Update comment**

```c
/* Schwung pads echo filter: suppress cable 2 echoes of notes
 * we overwrote into MIDI_IN (pads 68-99 → notes 36-67).
 * Sequencer playback has no refcount and passes through normally. */
```

**Step 2: Commit**

```bash
git add src/schwung_shim.c && git commit -m "docs: update schwung pads echo filter comment"
```

---

### Task 4: Build and deploy for hardware testing

**Step 1: Full build**

Run: `./scripts/build.sh`
Expected: Clean build.

**Step 2: Deploy**

Run: `./scripts/install.sh local --skip-modules --skip-confirmation`

**Step 3: Test on hardware**

1. Enter shadow mode (Shift+Vol+Track)
2. Load a patch on a slot
3. Go to slot settings, enable "Schwung Pads"
4. Press pads — should hear Schwung synth AND Move native synth (if track has a synth)
5. Record a clip — sequencer should record the remapped notes
6. Play back the clip — sequencer playback should trigger the Schwung slot
7. Switch tracks — echo refcount should reset
8. Disable Schwung Pads — pads should return to normal behavior

---

## Future Work (not in this plan)

- **Pad LED feedback**: Show note names or custom colors on pads when Schwung Pads is active
- **Configurable base note**: Let users set the starting note (currently hardcoded to C2/36)
- **Custom layouts**: Drum grids, isomorphic, scales
- **MIDI FX integration**: Route through chain MIDI FX (chord, arp) before injection — this is the midi-fx-to-move echo problem, deferred
- **Block Move native synth**: Option to suppress cable-2 reaching Move's synth (currently both play)
