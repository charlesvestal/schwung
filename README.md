# Move Anything

A framework for writing custom modules for the Ableton Move hardware.

## Features

- **Pads**: Note, velocity, polyphonic aftertouch
- **Encoders**: 9 endless knobs with touch sensing
- **Buttons**: All hardware buttons
- **LEDs**: Full color control
- **Display**: 128x64 1-bit framebuffer
- **Audio**: Line-in, mic, speakers, line-out
- **MIDI**: USB-A host (supports hubs with multiple devices)
- **Modular**: Drop-in modules with optional native DSP
- **Hot-swap**: Add/remove modules without recompiling the host (restart or rescan to pick up changes)

## Quick Install

1. Enable SSH on your Move: http://move.local/development/ssh
2. Connect Move to the same network as your computer
3. Run:

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

## Usage

- **Launch**: Hold Shift + touch Volume knob + Knob 8
- **Exit**: Hold Shift + click Jog wheel
- **Back**: Return to the host menu (unless a module sets `raw_ui`)
- **Settings**: Menu → Settings for velocity/aftertouch; "Return to Move" exits to the stock app

## Built-in Modules

| Module | Description |
|--------|-------------|
| **Signal Chain** | Modular signal chain with synths, MIDI FX, and audio FX |
| **MIDI Controller** | MIDI controller with 16 banks |
| **Module Store** | Browse, install, and update external modules |

### Signal Chain

The Signal Chain module lets you combine MIDI sources, MIDI effects, sound generators, and audio effects into patches:

**Sound Generators:**
- Line In (external audio input, built-in)
- SF2, DX7, OB-Xd, JV-880, CLAP (install via Module Store)
- Any module marked chainable as a sound generator

**MIDI Sources:**
- Sequencers or other modules that generate MIDI (via `midi_source` in a patch)

**MIDI Effects:**
- Chord generator (major, minor, power, octave)
- Arpeggiator (up, down, up-down, random)
- JavaScript MIDI FX (per-patch registry)

**Audio Effects:**
- Freeverb (Schroeder-Moorer reverb, built-in)
- CloudSeed, PSX Verb, TAPESCAM, Space Echo (install via Module Store)

Patches are JSON files in `modules/chain/patches/`. Use the jog wheel to highlight, click to load, Up/Down for octave control in the patch view, and Back to return to the list. Modules that provide a chain UI can be entered with Menu. External modules add their own chain presets when installed.

## External Modules (via Module Store)

Install these from the Module Store on your device:

**Sound Generators:**
| Module | Description |
|--------|-------------|
| SF2 Synth | SoundFont (.sf2) synthesizer |
| DX7 Synth | Yamaha DX7 FM synthesizer (loads .syx patches) |
| OB-Xd | Oberheim OB-X synthesizer emulator |
| JV-880 | Roland JV-880 rompler (requires ROM files) |
| CLAP Host | Host for CLAP audio plugins |

**Audio FX:**
| Module | Description |
|--------|-------------|
| CloudSeed | Algorithmic reverb |
| PSX Verb | PlayStation 1 SPU reverb emulation |
| TAPESCAM | Tape saturation and degradation |
| Space Echo | RE-201 style tape delay |

**Utilities:**
| Module | Description |
|--------|-------------|
| M8 LPP | Dirtywave M8 Launchpad Pro emulation |

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh
```

## Building

See [BUILDING.md](BUILDING.md) for build instructions.

## Documentation

- [Hardware API Reference](docs/API.md)
- [Module Development Guide](docs/MODULES.md)
- [Signal Chain Module Notes](src/modules/chain/README.md)

## Project Structure

```
move-anything/
  build/          # Build outputs
  dist/           # Packaged bundle for install
  src/
    host/           # Host runtime
    modules/        # Module source code
    shared/         # Shared JS utilities
  scripts/          # Build and install scripts
  libs/             # Vendored libraries (QuickJS)
  docs/             # Documentation
```

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
