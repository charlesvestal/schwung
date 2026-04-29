# Schwung (Formerly Move Everything)

[![Schwung Video](https://img.youtube.com/vi/AQ-5RZlg6gw/0.jpg)](https://www.youtube.com/watch?v=AQ-5RZlg6gw)

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Schwung adds a Shadow UI that runs alongside stock Move, enabling additional Synths, FX, and other tools to run in parallel to the usual UI.

- **Website:** [schwung.dev](https://schwung.dev)
- **Manual:** [schwung.dev/manual.html](https://schwung.dev/manual.html)
- **Module catalog:** [schwung.dev/catalog.html](https://schwung.dev/catalog.html)

## License

MIT - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)

Schwung would not be possible without the work of the [Move Anything](https://github.com/bobbydigitales/move-anything) project which provided the base techniques for accessing Move's hardware and system functions. Thanks to @talktogreg, @impbox, @deets, and especially @bobbyd for those contributions.

## Important Notice

This project is in no way approved, endorsed or supported by Ableton.

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode (on [Centercode](https://ableton.centercode.com/project/article/item.html?cap=ecd3942a1fe3405eb27a806608401a0b&arttypeid=%7Be70be312-f44a-418b-bb74-ed1030e3a49a%7D&artid=%7BC0A2D9E2-D52F-4DEB-8BEE-356B65C8942E%7D)) in case you need to restore your device. Move still works normally after installation; Schwung runs alongside it.

This is, in the truest sense of the word, a hack. It is not stable, or generally usable as a daily driver, but it's interesting, and super fun. Be warned, but have fun!

Also: this code is heavily written by coding agents, with human supervision. If that makes you nervous or you disagree with the approach, totally fine! Thanks for checking it out.

## Installation

### Desktop Installer (Recommended)

Download the [Schwung Installer](https://github.com/charlesvestal/schwung-installer/releases/latest) for your platform (macOS, Windows, Linux). It handles SSH setup, module selection, and upgrades via a graphical interface. The desktop installer is also accessible via screen reader.

### Command Line

**Prerequisites:**
- Move connected to WiFi
- A computer on the same network
- **Mac/Linux:** Terminal
- **Windows:** [Git Bash](https://git-scm.com/downloads) (comes with Git for Windows)

**Install:**
```bash
curl -L https://raw.githubusercontent.com/charlesvestal/schwung/main/scripts/install.sh | sh
```

**Screen reader only (accessible install):**
```bash
curl -sL https://raw.githubusercontent.com/charlesvestal/schwung/main/scripts/install.sh | sh -s -- --enable-screen-reader --disable-shadow-ui
```
_Note: Uses `-sL` (silent) for minimal output, suitable for screen readers._

The installer will:
1. **Guide you through SSH setup** if needed (generates key, shows how to add it to Move)
2. **Download and install** the Schwung framework
3. **Offer to install modules** (synths, effects) from the Module Store
4. **Copy assets** for modules that need them (ROMs, SoundFonts, etc.)

**Installation options:**
```bash
# Enable screen reader (TTS announcements) by default
./scripts/install.sh local --enable-screen-reader

# Install only screen reader, without UI features
./scripts/install.sh --enable-screen-reader --disable-shadow-ui

# Skip module installation prompt
./scripts/install.sh --skip-modules
```

For managing files on your Move, you can also use [Cyberduck](https://cyberduck.io) (SFTP to `move.local`, select your SSH private key).

For troubleshooting and manual setup, see the [Schwung Manual](https://schwung.dev/manual.html).

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/schwung/main/scripts/uninstall.sh | sh
```

By default, uninstall exports inactive Set Pages backups to `/data/UserData/UserLibrary/Schwung Backups/Set Pages/` before removing Schwung.

To permanently delete Schwung data instead of exporting a backup:

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/schwung/main/scripts/uninstall.sh | sh -s -- --purge-data
```

## Documentation

- [Schwung Manual](https://schwung.dev/manual.html) - User guide and shortcuts
- [BUILDING.md](BUILDING.md) - Build instructions
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - System and Shadow UI architecture
- [docs/MODULES.md](docs/MODULES.md) - Module development, Shadow UI integration, overtake modules
- [docs/API.md](docs/API.md) - JavaScript module API
- [src/modules/chain/README.md](src/modules/chain/README.md) - Signal Chain module notes

## Available Modules

Browse the full module catalog at [schwung.dev/catalog.html](https://schwung.dev/catalog.html). Modules are installable via the Module Store or the desktop installer. The catalog source lives in [module-catalog.json](module-catalog.json).

## Related Repositories

**Installer:**
- [move-everything-installer](https://github.com/charlesvestal/schwung-installer) - Cross-platform desktop installer (macOS, Windows, Linux)

## Community

- Discord: [https://discord.gg/GHWaZCC9bQ](https://discord.gg/GHWaZCC9bQ)

## AI Assistance Disclaimer

This module is part of Schwung and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
