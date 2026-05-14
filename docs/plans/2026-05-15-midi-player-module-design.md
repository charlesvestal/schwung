# MIDI Player Module — Design

**Date:** 2026-05-15
**Status:** Approved, ready to implement
**Module ID:** `midi-player`
**Component type:** `midi_fx`

## Purpose

Play `.mid` files into any chain slot's sound generator, synced to Move's MIDI
clock. Users drop files into the module's own `MIDI/` directory and pick one
from the chain slot's Shadow UI; all selected events flow downstream to
whatever synth is loaded in the slot.

## Architecture

The module sits in the MIDI FX slot of a Signal Chain:

```
Move clock (0xF8) + transport (0xFA/0xFB/0xFC)
       ↓
Chain MIDI FX slot (midi-player/dsp.so)
       ↓ emits note on/off via host MIDI callback
Sound generator (whatever the slot's synth is)
       ↓
Audio FX → output
```

External native module, separate repo (`schwung-midi-player`), released
through GitHub Actions and installed via the Module Store, same flow as
`schwung-midiverb` and other recent modules.

### Repo layout

```
schwung-midi-player/
  src/
    module.json
    ui.js              # standalone UI (file browser, track select, loop toggle)
    dsp/
      midi_player.c    # SMF parser + clock-synced scheduler
  scripts/
    build.sh           # Docker cross-compile + tarball
    install.sh
    Dockerfile
  .github/workflows/release.yml
  release.json
```

## Behavior

### Timing & transport

Locked to Move's transport. Move sends MIDI clock (`0xF8`, 24 PPQN) and
transport (`0xFA`/`0xFB`/`0xFC`) into the chain; the chain forwards them to
the MIDI FX plugin, same path the arpeggiator uses.

| Message | Action |
|---|---|
| `0xF8` Clock  | Advance playhead by `division / 24` file ticks; emit due events |
| `0xFA` Start  | Reset playhead to 0, arm playback |
| `0xFB` Cont.  | Resume from current playhead |
| `0xFC` Stop   | Pause, emit all-notes-off on ch 1 |
| `0xF2` SPP    | Ignored (v0.1 YAGNI) |

Quantization: events land on the 24 PPQN grid (sixteenth-note triplets at
worst). Sub-pulse interpolation can come later if anyone notices.

### Tempo

External clock *is* the tempo. The `.mid` file's tempo meta events
(`FF 51 03`) are discarded during parsing — Move's clock decides playback
speed, not the file.

### Track handling

Default: **All** — all tracks merged into one tick-sorted stream. User can
narrow to a single track via the chain slot menu. Track names come from
`FF 03` meta events; unnamed tracks display as `Track N`.

### Channels

All events rewritten to channel 1 before emission. Downstream chain synths
conventionally listen on ch 1; this guarantees the synth hears the playback
regardless of the file's original channel layout (e.g. drums on ch 10 still
play, as pitched notes — user picks appropriate files).

### Loop

Toggle parameter, default **On**. At end of timeline:
- `loop == On`: wrap playhead to 0 and continue draining events same frame.
- `loop == Off`: stop emitting, send all-notes-off on ch 1.

## DSP

`midi-player.so` is a `plugin_api_v2` module (multi-instance, chain-safe).

### Parameters

| Key | Type | Read/write | Description |
|---|---|---|---|
| `file`       | string | RW | Path to `.mid` file |
| `track`      | int    | RW | Track index, `-1` = All (default) |
| `loop`       | enum   | RW | `On` / `Off` (default `On`) |
| `track_list` | string | RO | JSON `[{index,name},…]` for current file |
| `position`   | string | RO | `"bar.beat"` for UI display |

Plus the standard `chain_params` and `ui_hierarchy` strings for chain
metadata.

### `.mid` parsing (on `set_param("file", …)`)

1. Read SMF header (`MThd`): type, ntrks, division (PPQ).
2. For each track chunk (`MTrk`): walk events building
   `{tick_abs, status, data1, data2, track_index}` records.
3. Capture each track's name from `FF 03` meta.
4. Discard tempo meta.
5. Build the playback timeline:
   - If `track == -1`: merge all records, sort by `tick_abs`.
   - Else: filter to that track only.
6. Rewrite all channel-voice status bytes to channel 1.
7. Reset playhead to 0.

Parsing happens off the realtime path — only on `set_param`. The clock-pulse
hot path just does index math plus emit calls. No allocations, no I/O.

### Scheduler

State: `playhead_ticks`, `event_index`, `running`, `end_ticks`,
`ticks_per_clock = division / 24`.

On `0xF8`:
```
if (!running) return;
playhead_next = playhead + ticks_per_clock;
while (event_index < n && events[event_index].tick < playhead_next) {
    emit(events[event_index]);
    event_index++;
}
playhead = playhead_next;
if (playhead >= end_ticks) {
    if (loop) { playhead = 0; event_index = 0; }
    else { running = false; send_all_notes_off(); }
}
```

## UI

### Shadow UI hierarchy (chain slot menu)

```
root
  ├─ Choose File…  → file_browser level
  ├─ Track          → track_select level
  └─ Loop           → On / Off (editable enum)

file_browser
  └─ uses filepath_browser.mjs, scoped to modules/midi_fx/midi-player/MIDI/

track_select
  └─ list of "All", "Track 1: <name>", "Track 2: <name>"…
```

No knobs mapped by default. Track / file / loop are all list-pickers in v0.1.

### `ui.js`

Standalone host-process UI mirrors the same controls for users loading the
module outside a chain context (rare for MIDI FX, but required for the
module loader to be happy). Uses `host_module_set_param` to push changes
into the DSP.

## Files

### `.mid` file location

`/data/UserData/schwung/modules/midi_fx/midi-player/MIDI/`

Module-scoped, not under `UserLibrary/`. Users drop files in via SSH or SMB
share, then browse from the module's Choose File menu.

### Empty `MIDI/` dir ships in the tarball

The Module Store extracts the tarball directly on first install, bypassing
`install.sh`. So `build.sh` creates `dist/midi-player/MIDI/.gitkeep` before
tar to guarantee the directory exists no matter how the module is installed.

### User files survive upgrades

`tar -xzf` overwrites archived files but does not delete files not in the
archive. User `.mid` files in `MIDI/` survive module upgrades; only the
module's own files (ui.js, dsp.so, module.json) are replaced.

## State persistence

The chain stores all `chain_params` per slot. Picked file, track, and loop
setting survive patch save/load automatically — no extra code in this
module.

## Catalog entry

Added to `module-catalog.json` in the schwung repo:

```json
{
  "id": "midi-player",
  "name": "MIDI Player",
  "description": "Play .mid files into any chain slot synth, synced to Move's clock",
  "author": "Charles Vestal",
  "component_type": "midi_fx",
  "github_repo": "charlesvestal/schwung-midi-player",
  "default_branch": "main",
  "asset_name": "midi-player-module.tar.gz",
  "min_host_version": "0.9.13",
  "requires": "Drop .mid files into modules/midi_fx/midi-player/MIDI/ via SSH or SMB"
}
```

## Out of scope (v0.1)

- Song Position Pointer (`0xF2`)
- Sub-pulse timing interpolation
- Per-track channel preservation (everything goes to ch 1)
- Tempo meta event honoring (external clock is the tempo)
- In-module recording or editing
- Multi-file playlists
