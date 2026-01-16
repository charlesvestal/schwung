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
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything-seqomd/main/scripts/install.sh | sh
```

## Usage

- **Launch**: Hold Shift + touch Volume knob + Knob 8
- **Exit**: Hold Shift + click Jog wheel
- **Back**: Return to the host menu (unless a module sets `raw_ui`)
- **Settings**: Menu → Settings for velocity/aftertouch; "Return to Move" exits to the stock app

## Included Module: SEQOMD

This fork includes the **SEQOMD** 16-track MIDI step sequencer. See [src/modules/seqomd/README.md](src/modules/seqomd/README.md) for full documentation.

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/ahlstrominfo/move-anything-seqomd/main/scripts/uninstall.sh | sh
```

## Building

See [BUILDING.md](BUILDING.md) for build instructions.

## Documentation

- [How It Works](docs/ARCHITECTURE.md) - System architecture and module loading
- [Hardware API Reference](docs/API.md) - JavaScript API for modules
- [Module Development Guide](docs/MODULES.md) - Creating your own modules

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

## Credits

This project is based on the excellent work by:
- **[bobbydigitales/move-anything](https://github.com/bobbydigitales/move-anything)** - Original Move Anything framework
- **[charlesvestal/move-anything](https://github.com/charlesvestal/move-anything)** - Extended version with additional modules

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
