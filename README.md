# Move Anything

A framework for writing custom modules for the Ableton Move hardware.

> **Fork Note**: This is a fork of [charlesvestal/move-anything](https://github.com/charlesvestal/move-anything) with additional modules and build improvements. See [Changes from Upstream](#changes-from-upstream) for details.

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
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything/main/scripts/install.sh | sh
```

## Usage

- **Launch**: Hold Shift + touch Volume knob + Knob 8
- **Exit**: Hold Shift + click Jog wheel

## Included Modules

| Module | Description |
|--------|-------------|
| SF2 Synth | SoundFont synthesizer with preset/octave control |
| DX7 | Yamaha DX7 FM synthesizer emulation |
| Step Sequencer | 16-step MIDI sequencer with external output |
| M8 LPP | Dirtywave M8 Launchpad Pro emulation |
| Controller | MIDI controller with 16 banks |

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything/main/scripts/uninstall.sh | sh
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

## Changes from Upstream

This fork differs from [charlesvestal/move-anything](https://github.com/charlesvestal/move-anything):

**Added modules:**
- **DX7** - Yamaha DX7 FM synthesizer emulation
- **Step Sequencer** - 16-step MIDI sequencer that outputs to external devices

**Build improvements:**
- Docker build uses `docker cp` to avoid OrbStack/Docker volume mount corruption on macOS
- Binaries are stripped for smaller package size (~588KB vs ~601KB)

**Not included from upstream:**
- Chain module (instrument + effects chaining)
- JV-880 module (Roland JV-880 emulation)

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
