# Move Everything

[![Move Everything Video](https://img.youtube.com/vi/AQ-5RZlg6gw/0.jpg)](https://www.youtube.com/watch?v=AQ-5RZlg6gw)

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Move Everything adds a Shadow UI that runs alongside stock Move, enabling additional Synths, FX, and other tools to run in parallel to the usual UI. 

## Important Notice

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode (on [Centercode](https://ableton.centercode.com/project/article/item.html?cap=ecd3942a1fe3405eb27a806608401a0b&arttypeid=%7Be70be312-f44a-418b-bb74-ed1030e3a49a%7D&artid=%7BC0A2D9E2-D52F-4DEB-8BEE-356B65C8942E%7D)) in case you need to restore your device. Move still works normally after installation; Move Everything runs alongside it.

Also: this code is heavily written by coding agents, with human supervision. If that makes you nervous or you disagree with the approach, totally fine! Thanks for checking it out.

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

NOTE: Some plugins require additional assets. Check the individual repositories for more information, and copy using scp or soemthing like [Cyberduck](https://cyberduck.io).

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

- [MANUAL.md](MANUAL.md) - User guide and shortcuts 
- [BUILDING.md](BUILDING.md) - Build instructions
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - System and Shadow UI architecture
- [docs/MODULES.md](docs/MODULES.md) - Module development, Shadow UI integration, overtake modules
- [docs/API.md](docs/API.md) - JavaScript module API
- [src/modules/chain/README.md](src/modules/chain/README.md) - Signal Chain module notes

## Related Repositories

- [move-anything-clap](https://github.com/charlesvestal/move-anything-clap)
- [move-anything-cloudseed](https://github.com/charlesvestal/move-anything-cloudseed)
- [move-anything-dx7](https://github.com/charlesvestal/move-anything-dx7)
- [move-anything-fourtrack](https://github.com/charlesvestal/move-anything-fourtrack)
- [move-anything-jv880](https://github.com/charlesvestal/move-anything-jv880)
- [move-anything-m8](https://github.com/charlesvestal/move-anything-m8)
- [move-anything-obxd](https://github.com/charlesvestal/move-anything-obxd)
- [move-anything-psxverb](https://github.com/charlesvestal/move-anything-psxverb)
- [move-anything-sf2](https://github.com/charlesvestal/move-anything-sf2)
- [move-anything-sidcontrol](https://github.com/charlesvestal/move-anything-sidcontrol)
- [move-anything-space-delay](https://github.com/charlesvestal/move-anything-space-delay)
- [move-anything-tapescam](https://github.com/charlesvestal/move-anything-tapescam)

## Community

- Discord: [https://discord.gg/Zn33eRvTyK](https://discord.gg/Zn33eRvTyK)
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
