# Move Everything

[![Move Everything Video](https://img.youtube.com/vi/AQ-5RZlg6gw/0.jpg)](https://www.youtube.com/watch?v=AQ-5RZlg6gw)

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Move Everything adds a Shadow UI that runs alongside stock Move, enabling additional Synths, FX, and other tools to run in parallel to the usual UI. 

Move Everything is based on the original Move Anything project by Bobby Digitales.

## Important Notice

This project is in no way approved, endorsed or supported by Ableton.

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode (on [Centercode](https://ableton.centercode.com/project/article/item.html?cap=ecd3942a1fe3405eb27a806608401a0b&arttypeid=%7Be70be312-f44a-418b-bb74-ed1030e3a49a%7D&artid=%7BC0A2D9E2-D52F-4DEB-8BEE-356B65C8942E%7D)) in case you need to restore your device. Move still works normally after installation; Move Everything runs alongside it.

This is, in the truest sense of the word, a hack. It is not stable, or generally usable as a daily driver, but it's interesting, and super fun. Be warned, but have fun!

Also: this code is heavily written by coding agents, with human supervision. If that makes you nervous or you disagree with the approach, totally fine! Thanks for checking it out.

## Installation

### Desktop Installer (Recommended)

Download the [Move Everything Installer](https://github.com/charlesvestal/move-everything-installer/releases/latest) for your platform (macOS, Windows, Linux). It handles SSH setup, module selection, and upgrades via a graphical interface.

### Command Line

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

By default, uninstall exports inactive Set Pages backups to `/data/UserData/UserLibrary/Move Everything Backups/Set Pages/` before removing Move Everything.

To permanently delete Move Anything data instead of exporting a backup:

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh -s -- --purge-data
```

## Modes

- **Shadow UI**: Runs custom signal chains alongside stock Move so you can layer additional synths and effects. Use Shift+Vol+Track (and +Menu) to access these signal chain slots.
- **Overtake modules**: Full-screen modules that temporarily take over the Move UI (e.g., MIDI controller apps). Use Shift+Vol+Jog click to access overtake modules.
- **Quantized Sampler**: Shift+Sample opens a sampler that records to `Samples/Move Everything/Resampler/YYYY-MM-DD/`. Choose resample (including Move Everything synths) or Move Input, set duration in bars, and recording starts on a note event or pressing play.
- **Skipback**: Shift+Capture writes the last 30 seconds of audio to `Samples/Move Everything/Skipback/YYYY-MM-DD/`.
- **Screen Reader**: Optional TTS announcements for accessibility. Toggle via Shadow UI settings, or Shift+Menu when Shadow UI is disabled.
- **Standalone**: Runs Move Everything without the stock app, including the Module Store. Use Shift+Vol+Knob 8 to access Standalone mode.

In **Global Settings > Display > Overlay Knobs**, you can change the knob overlay trigger between `+Shift` (default), `+Jog Touch`, or `Off`. If `Shift+Knob` interferes with native Move actions (like fine control), use `+Jog Touch` or `Off`.

Usage details, shortcuts, and workflows are documented in [MANUAL.md](MANUAL.md).

## Native Sampler Bridge

In **Master FX > Settings**, `Resample Src` controls whether Move Everything audio is fed into native Move sampling workflows:

- `Off`: Disabled (default)
- `Replace`: Replaces native sampler input with Move Everything master output

`Mix` is retained only as a legacy config value and is treated as `Replace`.

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

**Installer:**
- [move-everything-installer](https://github.com/charlesvestal/move-everything-installer) - Cross-platform desktop installer (macOS, Windows, Linux)

**Sound Generators:**
- [move-anything-airplay](https://github.com/charlesvestal/move-anything-airplay) - AirPlay audio receiver
- [move-anything-braids](https://github.com/charlesvestal/move-anything-braids) - Mutable Instruments macro oscillator
- [move-anything-chiptune](https://github.com/charlesvestal/move-anything-chiptune) - NES and Game Boy chiptune synthesizer
- [move-anything-clap](https://github.com/charlesvestal/move-anything-clap) - CLAP plugin host
- [move-anything-dx7](https://github.com/charlesvestal/move-anything-dx7) - Yamaha DX7 FM synthesizer
- [move-anything-hera](https://github.com/charlesvestal/move-anything-hera) - Juno-60 emulation
- [move-anything-hush1](https://github.com/charlesvestal/move-anything-hush1) - HUSH ONE monophonic SH-101-style synthesizer
- [move-anything-jv880](https://github.com/charlesvestal/move-anything-jv880) - Roland JV-880 emulator
- [move-anything-moog](https://github.com/charlesvestal/move-anything-moog) - RaffoSynth (Moog ladder filter)
- [move-anything-nusaw](https://github.com/charlesvestal/move-anything-nusaw) - NuSaw polyphonic detuned supersaw synthesizer
- [move-anything-obxd](https://github.com/charlesvestal/move-anything-obxd) - Oberheim OB-X emulator
- [move-anything-radiogarden](https://github.com/charlesvestal/move-anything-radiogarden) - Live radio streaming synthesizer
- [move-anything-rex](https://github.com/charlesvestal/move-anything-rex) - ReCycle loop slice player (.rx2/.rex)
- [move-anything-sf2](https://github.com/charlesvestal/move-anything-sf2) - SoundFont synthesizer
- [move-anything-surge](https://github.com/charlesvestal/move-anything-surge) - Surge XT hybrid synthesizer
- [move-anything-virus](https://github.com/charlesvestal/move-anything-virus) - Access Virus emulator (Osirus/Gearmulator)
- [move-anything-webstream](https://github.com/charlesvestal/move-anything-webstream) - Web audio search and streaming generator

**Audio FX:**
- [move-anything-cloudseed](https://github.com/charlesvestal/move-anything-cloudseed) - Algorithmic reverb
- [move-anything-ducker](https://github.com/charlesvestal/move-anything-ducker) - MIDI-triggered sidechain ducker
- [move-anything-gate](https://github.com/charlesvestal/move-anything-gate) - Noise gate and downward expander
- [move-anything-junologue-chorus](https://github.com/charlesvestal/move-anything-junologue-chorus) - Juno-60 chorus
- [move-anything-keydetect](https://github.com/charlesvestal/move-anything-keydetect) - Musical key detection effect
- [move-anything-nam](https://github.com/charlesvestal/move-anything-nam) - Neural Amp Modeler
- [move-anything-psxverb](https://github.com/charlesvestal/move-anything-psxverb) - PlayStation SPU reverb
- [move-anything-space-delay](https://github.com/charlesvestal/move-anything-space-delay) - RE-201 tape delay
- [move-anything-tapescam](https://github.com/charlesvestal/move-anything-tapescam) - Tape saturation
- [move-anything-vocoder](https://github.com/charlesvestal/move-anything-vocoder) - Channel vocoder (line-in/mic modulator)

**MIDI FX:**
- [move-anything-superarp](https://github.com/handcraftedcc/move-anything-superarp) - Super Arp advanced arpeggiator (community)

**Overtake/Utilities:**
- [move-anything-control](https://github.com/chaolue/move-anything-control) - Custom MIDI controller (community)
- [move-anything-m8](https://github.com/charlesvestal/move-anything-m8) - Dirtywave M8 Launchpad Pro emulator
- [move-anything-seqomd](https://github.com/charlesvestal/move-anything-seqomd) - 16-track MIDI step sequencer
- [move-anything-sidcontrol](https://github.com/charlesvestal/move-anything-sidcontrol) - SIDaster III controller

## Community

- Discord: [https://discord.gg/Zn33eRvTyK](https://discord.gg/Zn33eRvTyK)
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)

## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
