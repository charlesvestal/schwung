# Move Anything

A framework for writing custom modules for the Ableton Move hardware.

## Credits

This project is based on the excellent work by:
- **[bobbydigitales/move-anything](https://github.com/bobbydigitales/move-anything)** - Original Move Anything framework
- **[charlesvestal/move-anything](https://github.com/charlesvestal/move-anything)** - Extended version with additional modules

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

## Included Module

| Module | Description |
|--------|-------------|
| Sequencer | OP-Z inspired 8-track MIDI step sequencer with per-step parameters |

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

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
