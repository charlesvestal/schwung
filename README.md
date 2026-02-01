# Move Everything

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Move Everything adds a Shadow UI that runs alongside stock Move plus a standalone mode for running modules alongside the stock app.

## Important Notice

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode. Move still works normally after installation; Move Everything runs alongside it.

## Installation (Quick)

Prereqs:
- Move connected to WiFi
- A computer on the same network with SSH access enabled

Enable SSH:
1. Generate an SSH key if you do not have one: `ssh-keygen -t ed25519`
2. Add your public key at: `http://move.local/development/ssh`
3. Test: `ssh ableton@move.local`

Install:
```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

For full prerequisites, troubleshooting, and updates, see [MANUAL.md](MANUAL.md).

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh
```

## Modes (Background)

- **Shadow UI**: Runs custom signal chains alongside stock Move so you can layer additional synths and effects. Use Shift+Vol+Track (and +Menu) to access these signal chain slots.
- **Overtake modules**: Full-screen modules that temporarily take over the Move UI (e.g., MIDI controller apps). Use Shift+Vol+Jog click to access overtake modules.

- **Standalone**: Runs Move Everything without the stock app, including the Module Store. Use Shift+Vol+Knob 8 to access Standalone mode. (To be deprecated, soon)

Usage details, shortcuts, and workflows are documented in [MANUAL.md](MANUAL.md) (primarily standalone-focused).

## Documentation

- [MANUAL.md](MANUAL.md) - User guide and shortcuts (standalone-focused)
- [BUILDING.md](BUILDING.md) - Build instructions
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - System and Shadow UI architecture
- [docs/MODULES.md](docs/MODULES.md) - Module development, Shadow UI integration, overtake modules
- [docs/API.md](docs/API.md) - JavaScript module API
- [src/modules/chain/README.md](src/modules/chain/README.md) - Signal Chain module notes

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal, and others

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
