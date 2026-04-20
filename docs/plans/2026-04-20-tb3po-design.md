# TB-3PO Port — Design

Port of the Phazerville Hemisphere Suite TB-3PO generative acid-bassline sequencer to Schwung.

## Goal

Ship a generative 303-style sequencer as an overtake module. Full Phazerville feature set: pattern generation, density/accent/slide probabilities, pattern banks, mutate, direction, manual step edit, scale/root/length/gate — all on Move's 32 pads + 8 knobs + 128×64 display. MIDI output routes to any shadow slot via channel-based routing. Sequencer keeps running when its UI is suspended.

## Non-goals

- CV/gate outputs (the original Ordering Chaos has these; we're MIDI-only).
- 303-specific filter emulation. Pair with hush1 (SH-101), bristol, or obxd for the acid sound; TB-3PO only generates notes.

## Architecture

```
┌──────────────────────────┐
│  tb3po tool module       │   JS-only. QuickJS ticks every audio block.
│   - sequencer algorithm  │   Emits MIDI via shadow_send_midi_to_dsp.
│   - pad/display UI       │
└──────────┬───────────────┘
           │ shadow_send_midi_to_dsp([status|ch, d1, d2])
           ▼
    shadow_midi_dsp SHM
           │
           ▼
┌──────────────────────────┐
│  Shadow chain DSP        │   Existing mechanism. Slots with matching
│   Slot 1..4              │   receive_channel pick up the MIDI and
│   (hush1/bristol/…)      │   drive their synth.
└──────────────────────────┘
```

Shipped as a **tool** (`component_type: tool` + `tool_config.skip_file_browser`) so it's reachable via **Shift+Vol+Step13 → Tools menu → TB-3PO** — same shortcut as File Browser, Song Mode, etc.

No daemon, no split modules, no new IPC. Routing uses an existing JS binding (`js_shadow_send_midi_to_dsp` in `src/shadow/shadow_ui.c:764`).

### Suspend behavior — host change (Phase 1, landed)

A new capability `suspend_keeps_js` makes the host's suspend path retain the module's JS closure. Concretely:

- **Back button** → suspend: module parks into `suspendedOvertakes[moduleId]`. Its `tick()` keeps firing every frame. Display/LED bindings are swapped for no-ops during background tick so the module can't stomp on whatever view is on screen. Pad/knob/button input stops reaching the module.
- **Shift+Back** → full exit (unload module) — Wave Editor convention.
- **Shift+Vol+Back** → also suspend (host gesture, works from any view).
- **Shift+Vol+Jog Click** → full exit via host gesture (always works, can't be trapped by a broken module).
- **Re-open via Tools menu** → if the picked module is in `suspendedOvertakes`, `loadOvertakeModule` hoists it back to active instead of fresh-loading. State preserved.

Multiple modules can be suspended simultaneously — each stays parked in the map and ticks independently until exited or resumed.

Declared via `module.json`:

```json
"capabilities": { "suspend_keeps_js": true }
```

This is a general primitive — any future generative tool (step seqs, arps, drum machines) can use it.

## Module manifest

`src/module.json`:

```json
{
  "id": "tb3po",
  "name": "TB-3PO",
  "version": "0.1.0",
  "author": "Charles Vestal",
  "description": "Generative 303-style acid-bassline sequencer (after Phazerville Hemisphere Suite)",
  "component_type": "tool",
  "tool_config": {
    "interactive": true,
    "skip_file_browser": true,
    "overtake": true
  },
  "capabilities": {
    "midi_out": true,
    "suspend_keeps_js": true
  }
}
```

No DSP plugin. No native code. JS-only.

`skip_file_browser` + `interactive` means tool launches straight into `startInteractiveTool(tool, "")` without a file-picker step. `overtake: true` makes it take the hardware UI (pads/knobs/display) like a normal overtake module.

## UI layout

### Knobs (1–8)

| # | Param         | Range                        |
|---|---------------|------------------------------|
| 1 | Density       | 0–100%                       |
| 2 | Accent        | 0–100%                       |
| 3 | Slide         | 0–100%                       |
| 4 | Octave range  | 1–3                          |
| 5 | Root          | C, C#, D, …, B               |
| 6 | Scale         | Minor, Phrygian, HarmMinor, Pent, Dorian, Major |
| 7 | Length        | 8, 16, 24, 32                |
| 8 | Gate length   | 10–100%                      |

### Pads (32, 4×8)

| Row | Pads   | Function                                                |
|-----|--------|---------------------------------------------------------|
| 1   | 1–8    | Steps 1–8 visualization + tap-to-cycle state (rest → note → accent → slide) |
| 2   | 9–16   | Steps 9–16 (for length ≥16)                             |
| 3   | 17–24  | Pattern banks 1–8. Tap: recall. Hold: store current.    |
| 4   | 25–32  | Actions: New / Mutate / Reverse / Shift− / Shift+ / Direction / Clear / Regen-seed |

Step LEDs:
- Dim grey = rest
- White = note
- Red = accent
- Blue = slide
- Bright accent on the *current playing step* (cycles with sequencer position)

### Display (128×64, 1-bit)

```
┌────────────────────────────────────────┐
│ TB-3PO     Amin    BPM120  CH8  [>]    │   header
│ ■·■·■·■·■·■·■·■·                       │   step grid row 1 (16 cells scaled)
│ ·■·■·■·■·■·■·■·■                       │
│ Dens 72%  Acc 40%  Slide 25%           │   params
│ Bank 3/8  Dir ↑    Len 16  Gate 50%    │
└────────────────────────────────────────┘
```

Current step marked with a caret or inverted cell.

### Buttons

- **Back** → suspend (MIDI keeps flowing, UI returns to Move). Host-intercepted for suspend_keeps_js modules.
- **Shift+Back** → full exit (unload). Wave Editor convention.
- **Shift+Vol+Back** / **Shift+Vol+Jog Click** → suspend / full exit via host gestures (escape hatches).
- **Re-open from Tools menu** → auto-resume with preserved state.
- **Play** → start/stop the internal clock (if sync is "internal").
- **Capture / Sample** → left to the host (don't override).

### Clock

Two modes, param-selected:

- **Internal**: own BPM knob (or shift+knob to adjust in Hz), emits on its own tempo.
- **External (MIDI clock)**: consume 0xF8 pulses; advance on every 6th pulse for 16ths. Auto-switches to external within ~1s of seeing clock pulses, falls back to internal after ~2s of silence.

Mirrors the arp module's `sync: internal/clock` convention.

## Sequencer algorithm

Ported from `TB_3PO.h` in the Phazerville repo. See **Licensing** below for the port approach.

Core state:
- `steps[32]` — each step is `{state: rest|note|accent|slide, degree: 0-7, octave: 0-2}`
- `position` — current step (0 to length-1)
- `direction` — 0=fwd, 1=rev, 2=pingpong, 3=random
- `seed` — 16-bit PRNG seed used to regenerate
- `pattern_banks[8]` — snapshots

Generator (`Generate(seed)`):
1. Seed a small LCG / xorshift.
2. For each step 0..length-1:
   - Roll `density` — if miss, rest; else note.
   - If note, roll `accent` density → mark accent.
   - If note, roll `slide` density → mark slide.
   - Pick scale degree (0..7) weighted toward roots on strong beats.
   - Pick octave from `octave_range`.
3. Post-pass: clean up illegal slide→rest sequences, ensure at least one note.

Emission (per clock step):
- Compute MIDI note from (root, scale, degree, octave).
- Note-on on current step with velocity 60–85 (100–127 if accent).
- Note-off at end of gate window.
- If next step is a slide, *overlap* — emit next note-on before releasing current (legato), and send `CC 65 = 127` (portamento on) for that step, `CC 65 = 0` afterwards.

Tempo:
- Internal: step interval = 60/BPM/4 seconds (16ths). `tick()` runs every 128 frames = ~2.9ms, so we accumulate fractional step progress.
- External: count MIDI clock pulses (24 PPQN); advance on every 6th pulse.

## State persistence

On every param change + on exit, write JSON snapshot to `/data/UserData/schwung/modules/tb3po/state.json`:

```json
{
  "seed": 42,
  "density": 0.72,
  "accent": 0.40,
  "slide": 0.25,
  "octaves": 2,
  "root": 9,
  "scale": 0,
  "length": 16,
  "gate": 0.50,
  "direction": 0,
  "channel": 8,
  "bpm": 120,
  "sync": 1,
  "current_bank": 2,
  "banks": [[…16 steps…], …]
}
```

On init, load and restore. Clean resume.

## Licensing

Phazerville's O_C firmware is **GPL-3**. Schwung is **MIT**. Two options:

- **A. Clean-room reimplementation**: read the TB_3PO algorithm description, write fresh JS. Music-theory scale tables and 16-step RNG generators aren't themselves copyrightable; the implementation is. The result is MIT-clean.
- **B. Direct port**: translate TB_3PO.h line-by-line to JS. `schwung-tb3po` repo is licensed GPL-3 (separate from parent Schwung's MIT). Attribution in README. Parent Schwung repo unaffected.

Recommendation: **A**. The algorithm is small enough that clean-room is faster than arguing with licensing. TB_3PO.h can be referenced as a spec, not as code.

## Repo layout

```
schwung-tb3po/
  src/
    module.json
    ui.js                 # everything: algorithm + UI + IO
  scripts/
    build.sh              # packages dist/tb3po/ + tarball. No cross-compile needed.
    install.sh
    clean.sh
  .github/workflows/
    release.yml           # tag-triggered build, tarball upload
  release.json            # version + download URL
  LICENSE
  README.md
```

Since there's no native code, `build.sh` is trivial (just copy `src/` to `dist/tb3po/` and tar). No Dockerfile needed.

## Catalog entry

In `module-catalog.json` under `modules[]`:

```json
{
  "id": "tb3po",
  "name": "TB-3PO",
  "description": "Generative 303-style acid-bassline sequencer",
  "author": "Charles Vestal",
  "component_type": "overtake",
  "github_repo": "charlesvestal/schwung-tb3po",
  "default_branch": "main",
  "asset_name": "tb3po-module.tar.gz",
  "min_host_version": "0.9.8"
}
```

`min_host_version` bumps to whichever release ships the `suspend_keeps_js` capability.

## Build-out phases

1. **Host change** [done]: `suspend_keeps_js` capability + parked-modules map + display/LED no-op during background tick + Back/Shift+Back host intercepts + Tools-menu auto-resume via `loadOvertakeModule`. Tested with a `suspend-test` stub. Removed once TB-3PO ships.
2. **Algorithm**: port TB_3PO logic to JS in isolation. Verify density/accent/slide distributions match expectations.
3. **Module skeleton**: module.json + ui.js with knob handling, pad handling, display layout, MIDI output, MIDI clock sync.
4. **Integration**: deploy, test with hush1 in a shadow slot, verify suspend/resume.
5. **Polish**: state persistence, pattern banks, mutate, direction, manual step edit.

Phase 1 shipped in the main Schwung repo. Phases 2–5 in `schwung-tb3po`.

## YAGNI cuts

- No MIDI learn for external CC control of params (v0.2 if wanted).
- No CV-in for pattern reset / transpose (Move doesn't have CV in).
- No per-step probability per-slot (Phazerville has this; we ship the global probabilities only).
- No "manual play-while-sequencing" mode — pads are UI when overtake focused.

## Open questions

- Does suspended-but-ticking JS cause audio glitches? `tick()` runs in the shadow UI thread, not the SPI callback, so probably fine, but verify on hardware.
- Pattern bank store/recall: 8 banks = 8 pads. What happens if user holds a bank while a step pad is held? Resolve: store wins.
- When resuming, do we show a brief "Resuming…" splash or instant? Probably instant — there's no heavy init.
