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

## Included Modules

| Module | Description |
|--------|-------------|
| **Signal Chain** | Modular signal chain with synths, MIDI FX, and audio FX |
| DX7 Synth | 6-operator FM synthesizer (Yamaha DX7), loads .syx patches |
| SF2 Synth | SoundFont synthesizer with preset/octave control |
| Step Sequencer | 16-step MIDI sequencer with external output |
| M8 LPP | Dirtywave M8 Launchpad Pro emulation |
| Controller | MIDI controller with 16 banks |

### Signal Chain

The Signal Chain module lets you combine sound generators, MIDI effects, and audio effects into patches:

**Sound Generators:**
- SF2 (SoundFont synth)
- DX7 (FM synth)
- Line In (external audio input)
- External modules (OB-Xd, JV-880) when installed

**MIDI Effects:**
- Chord generator (major, minor, power, octave)
- Arpeggiator (up, down, up-down, random)

**Audio Effects:**
- Freeverb (Schroeder-Moorer reverb)

Patches are JSON files in `modules/chain/patches/`. Use the jog wheel to browse, Up/Down buttons for octave control. External modules add their own chain presets when installed.

## Additional Modules

These modules are maintained in separate repositories:

| Module | Description | Repository |
|--------|-------------|------------|
| **OB-Xd** | Oberheim OB-X synthesizer emulator | [move-anything-obxd](https://github.com/charlesvestal/move-anything-obxd) |
| **JV-880** | Roland JV-880 rompler emulator | [move-anything-jv880](https://github.com/charlesvestal/move-anything-jv880) |

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything/main/scripts/uninstall.sh | sh
```

## Building

See [BUILDING.md](BUILDING.md) for build instructions.

## Documentation

- [Hardware API Reference](docs/API.md)
- [Module Development Guide](docs/MODULES.md)
- [Signal Chain Module Notes](src/modules/chain/README.md)
- [DX7 Module Notes](src/modules/dx7/README.md)
- [SF2 Module Notes](src/modules/sf2/README.md)

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
