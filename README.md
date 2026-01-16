# Move Anything

A framework for writing custom modules for the Ableton Move hardware.

## Credits

This project is based on the excellent work by:
- **[bobbydigitales/move-anything](https://github.com/bobbydigitales/move-anything)** - Original Move Anything framework
- **[charlesvestal/move-anything](https://github.com/charlesvestal/move-anything)** - Extended version with additional modules

## Quick Install

1. Enable SSH on your Move: http://move.local/development/ssh
2. Connect Move to the same network as your computer
3. Run:

```bash
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything-seqomd/main/scripts/install.sh | sh
```

## Usage

- **Launch**: Hold Shift + touch Volume knob + Knob 8
- **Exit**: Hold Shift + click Jog wheel

## SEQOMD - 16-Track MIDI Step Sequencer

SEQOMD turns your Ableton Move into a MIDI step sequencer. You get 16 tracks, each with 16 steps and 16 patterns. Steps can hold up to 7 notes with individual velocities, two CC values, probability, loop-based conditions, ratchets, and micro-timing offsets. There's a built-in arpeggiator with 15 modes that can be set per-track or overridden per-step. Tracks run at independent speeds and swing amounts. A transpose sequence lets you automate key changes across the pattern. Everything saves to 32 set slots on disk. It outputs MIDI over USB to control synths, drum machines, or your DAW.

### Core Specs

| Feature | Capacity |
|---------|----------|
| Tracks | 16 (independent MIDI channels, speeds, swing) |
| Steps per Pattern | 16 (adjustable loop points) |
| Patterns per Track | 16 (with snapshot recall) |
| Sets | 32 (complete states saved to disk) |
| Notes per Step | 7 (polyphonic with individual velocities) |
| Scheduled Notes | 512 simultaneous |

### Step Parameters

Each step supports:
- **Notes & Velocities**: Up to 7 polyphonic notes with per-note velocity
- **Note Length**: 1-16 steps
- **CC1 & CC2**: Two CC values per step with track defaults
- **Micro-timing**: ±24 ticks offset (half-step resolution)
- **Probability**: 5-100% trigger chance
- **Conditions**: Loop-based triggers (1:2, 2:3, 1:4, etc. - 72 total)
- **Ratchets**: 1x-8x with velocity ramp up/down modes (22 options)
- **Jump**: Non-linear sequencing to any step

### Arpeggiator

Full per-track arpeggiator with per-step overrides:
- **15 Modes**: Up, Down, Up-Down, Random, Chord, Converge, Diverge, Thumb, Pinky, Outside-In, Inside-Out, etc.
- **10 Speeds**: 1/32 to whole notes
- **Octave Extension**: None, ±1, ±2 octaves
- **Play Steps**: Binary skip patterns for complex rhythms
- **Continuous Mode**: Arp continues across triggers
- **Layer Modes**: Layer (overlap), Cut (monophonic), Legato

### Spark System (Conditional Triggering)

Three independent condition layers per step:
1. **Trigger Spark**: When does the step play?
2. **Parameter Spark**: When do CC locks apply?
3. **Component Spark**: When do ratchets/jumps apply?

Create evolving patterns like "play every loop, but filter sweep only every 4th."

### Transpose & Scale

- **16-step transpose sequence** with duration and conditions per step
- **Live transpose**: Piano pads for instant ±24 semitone shifts
- **Chord follow**: Per-track toggle for transpose response
- **Scale detection**: Auto-detects scale from played notes

### Track Features

- **Speed**: 1/4x to 4x multipliers (polyrhythms)
- **Swing**: 0-100% per track
- **MIDI Channel**: 1-16 per track
- **Mute**: Per-track control

### Navigation

| Action | Result |
|--------|--------|
| **Pad tap** (set view) | Load set, enter track view |
| **Menu** | Toggle pattern view |
| **Shift + Menu** | Toggle master view (transpose) |
| **Loop** (hold) | Edit loop points |
| **Capture + Step** | Edit spark conditions |
| **Shift + Step 1** | Return to set view |
| **Shift + Step 2** | Channel mode |
| **Shift + Step 5** | Speed mode |
| **Shift + Step 7** | Swing mode |
| **Shift + Step 11** | Arp mode |
| **Track buttons** | Select tracks 1-4 |
| **Shift + Track** | Select tracks 5-8 |
| **Jog wheel** | Scroll tracks (in 4-track groups) |

### Step Editing (Hold Step)

| Control | Function |
|---------|----------|
| **Pads** | Toggle notes (up to 7) |
| **Knob 1** | CC1 value (tap to clear) |
| **Knob 2** | CC2 value (tap to clear) |
| **Knob 3** | Arp override (tap to cycle modes) |
| **Knob 6** | Ratchet 1x-8x (tap to reset) |
| **Knob 7** | Probability/Condition (tap to clear) |
| **Knob 8** | Velocity (tap to reset to 100) |
| **Jog wheel** | Micro-timing offset |
| **Tap another step** | Set note length |

### Hardware Integration

Uses all Move hardware:
- **32 Pads**: Piano keyboard with velocity, notes light when playing
- **16 Step buttons**: Step editing and playhead display
- **4 Track buttons**: Track selection (scrollable across 16)
- **8 Knobs**: Context-sensitive parameters with touch sensing
- **Jog wheel**: Fine control and scrolling
- **Transport**: Play, Record, Loop, Menu, Back, Shift
- **Display**: 4-line LCD with context-sensitive info

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything-seqomd/main/scripts/uninstall.sh | sh
```

## Building

See [BUILDING.md](BUILDING.md) for build instructions.

## Documentation

- [Hardware API Reference](docs/API.md)
- [Module Development Guide](docs/MODULES.md)

## Project Structure

```
move-anything/
  src/
    host/           # Host runtime
    modules/        # Module source code
    shared/         # Shared JS utilities
  scripts/          # Build and install scripts
  assets/           # Font and images
  libs/             # Vendored libraries (QuickJS)
  docs/             # Documentation
```

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
