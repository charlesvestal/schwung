# Move Everything

[![Move Everything Video](https://img.youtube.com/vi/AQ-5RZlg6gw/0.jpg)](https://www.youtube.com/watch?v=AQ-5RZlg6gw)

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Move Everything adds a Shadow UI that runs alongside stock Move, enabling additional Synths, FX, and other tools to run in parallel to the usual UI. 

## Important Notice

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode (on [Centercode](https://ableton.centercode.com/project/article/item.html?cap=ecd3942a1fe3405eb27a806608401a0b&arttypeid=%7Be70be312-f44a-418b-bb74-ed1030e3a49a%7D&artid=%7BC0A2D9E2-D52F-4DEB-8BEE-356B65C8942E%7D)) in case you need to restore your device. Move still works normally after installation; Move Everything runs alongside it.

This is, in the truest sense of the word, a hack. It is not stable, or generally usable as a daily driver, but it's interesting, and super fun. Be warned, but have fun!

Also: this code is heavily written by coding agents, with human supervision. If that makes you nervous or you disagree with the approach, totally fine! Thanks for checking it out.

## Installation

**Prerequisites:**
- Move connected to WiFi
- A computer on the same network
- **Mac/Linux:** Terminal
- **Windows:** [Git Bash](https://git-scm.com/downloads) (comes with Git for Windows)

**Install:**
```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

**Screen reader only (accessible install):**
```bash
curl -sL https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh -s -- --enable-screen-reader --disable-shadow-ui --disable-standalone
```
_Note: Uses `-sL` (silent) for minimal output, suitable for screen readers._

The installer will:
1. **Guide you through SSH setup** if needed (generates key, shows how to add it to Move)
2. **Download and install** the Move Everything framework
3. **Offer to install modules** (synths, effects) from the Module Store
4. **Copy assets** for modules that need them (ROMs, SoundFonts, etc.)

**Installation options:**
```bash
# Enable screen reader (TTS announcements) by default
./scripts/install.sh local --enable-screen-reader

# Install only screen reader, without UI features
./scripts/install.sh --enable-screen-reader --disable-shadow-ui --disable-standalone

# Skip module installation prompt
./scripts/install.sh --skip-modules
```


For managing files on your Move, you can also use [Cyberduck](https://cyberduck.io) (SFTP to `move.local`, select your SSH private key).

For troubleshooting and manual setup, see [MANUAL.md](MANUAL.md).

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh
```

## Modes (Background)

- **Shadow UI**: Runs custom signal chains alongside stock Move so you can layer additional synths and effects. Use Shift+Vol+Track (and +Menu) to access these signal chain slots.
- **Overtake modules**: Full-screen modules that temporarily take over the Move UI (e.g., MIDI controller apps). Use Shift+Vol+Jog click to access overtake modules.
- **Screen Reader**: Optional TTS announcements for accessibility. Toggle via Shadow UI settings, or Shift+Menu when Shadow UI is disabled.
- **Standalone**: Runs Move Everything without the stock app, including the Module Store. Use Shift+Vol+Knob 8 to access Standalone mode. (To be deprecated, soon)

Usage details, shortcuts, and workflows are documented in [MANUAL.md](MANUAL.md) (primarily standalone-focused).

## Native Sampler Bridge

In **Master FX > Settings**, `Resample Src` controls whether Move Everything audio is fed into native Move sampling workflows:

- `Off`: Disabled (default)
- `Mix`: Adds Move Everything output to the native sampler input
- `Replace`: Replaces native sampler input with Move Everything master output

For the most reliable native sampling behavior with this feature:
- Set `Resample Src` to **Replace**
- In Move's sampler, set sample source to **Line In**
- Set monitoring to **Off**

If monitoring is on (or source/routing is configured differently), audio feedback may occur.

## Documentation

- [MANUAL.md](MANUAL.md) - User guide and shortcuts 
- [BUILDING.md](BUILDING.md) - Build instructions
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - System and Shadow UI architecture
- [docs/MODULES.md](docs/MODULES.md) - Module development, Shadow UI integration, overtake modules
- [docs/API.md](docs/API.md) - JavaScript module API
- [src/modules/chain/README.md](src/modules/chain/README.md) - Signal Chain module notes

## Related Repositories

- [move-anything-braids](https://github.com/charlesvestal/move-anything-braids)
- [move-anything-clap](https://github.com/charlesvestal/move-anything-clap)
- [move-anything-cloudseed](https://github.com/charlesvestal/move-anything-cloudseed)
- [move-anything-dx7](https://github.com/charlesvestal/move-anything-dx7)
- [move-anything-fourtrack](https://github.com/charlesvestal/move-anything-fourtrack)
- [move-anything-hera](https://github.com/charlesvestal/move-anything-hera)
- [move-anything-junologue-chorus](https://github.com/charlesvestal/move-anything-junologue-chorus)
- [move-anything-jv880](https://github.com/charlesvestal/move-anything-jv880)
- [move-anything-m8](https://github.com/charlesvestal/move-anything-m8)
- [move-anything-moog](https://github.com/charlesvestal/move-anything-moog)
- [move-anything-obxd](https://github.com/charlesvestal/move-anything-obxd)
- [move-anything-psxverb](https://github.com/charlesvestal/move-anything-psxverb)
- [move-anything-sf2](https://github.com/charlesvestal/move-anything-sf2)
- [move-anything-sidcontrol](https://github.com/charlesvestal/move-anything-sidcontrol)
- [move-anything-space-delay](https://github.com/charlesvestal/move-anything-space-delay)
- [move-anything-surge](https://github.com/charlesvestal/move-anything-surge)
- [move-anything-tapescam](https://github.com/charlesvestal/move-anything-tapescam)

## Community

- Discord: [https://discord.gg/Zn33eRvTyK](https://discord.gg/Zn33eRvTyK)
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
