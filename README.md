# Move Everything

[![Move Everything Video](https://img.youtube.com/vi/AQ-5RZlg6gw/0.jpg)](https://www.youtube.com/watch?v=AQ-5RZlg6gw)

An unofficial framework for running custom instruments, effects, and controllers on Ableton Move.

Move Everything adds a Shadow UI that runs alongside stock Move, enabling additional Synths, FX, and other tools to run in parallel to the usual UI. 

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)

Move Everything would not be possible without the work of the [Move Anything](https://github.com/bobbydigitales/move-anything) project which provided the base techniques for accessing Move's hardware and system functions. Thanks to @talktogreg, @impbox, @deets, and especially @bobbyd for those contributions.

## Important Notice

This project is in no way approved, endorsed or supported by Ableton.

This project modifies software on your Ableton Move. Back up important sets and samples before installing and familiarize yourself with DFU restore mode (on [Centercode](https://ableton.centercode.com/project/article/item.html?cap=ecd3942a1fe3405eb27a806608401a0b&arttypeid=%7Be70be312-f44a-418b-bb74-ed1030e3a49a%7D&artid=%7BC0A2D9E2-D52F-4DEB-8BEE-356B65C8942E%7D)) in case you need to restore your device. Move still works normally after installation; Move Everything runs alongside it.

This is, in the truest sense of the word, a hack. It is not stable, or generally usable as a daily driver, but it's interesting, and super fun. Be warned, but have fun!

Also: this code is heavily written by coding agents, with human supervision. If that makes you nervous or you disagree with the approach, totally fine! Thanks for checking it out.

## Installation

### Desktop Installer (Recommended)

Download the [Move Everything Installer](https://github.com/charlesvestal/move-everything-installer/releases/latest) for your platform (macOS, Windows, Linux). It handles SSH setup, module selection, and upgrades via a graphical interface. The desktop installer is also accessible via screen reader.

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
curl -sL https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh -s -- --enable-screen-reader --disable-shadow-ui
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
./scripts/install.sh --enable-screen-reader --disable-shadow-ui

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

## Available Modules

Modules are installable via the Module Store or the desktop installer. See [module-catalog.json](module-catalog.json) for full details.

<!-- MODULE_TABLE_START -->
### Sound Generators

| Module | Description | Author |
|--------|-------------|--------|
| [Dexed](https://github.com/charlesvestal/move-everything-dx7) | 6-operator FM synthesizer (Dexed/MSFA) with .syx patch support | Dexed/MSFA by google/asb2m10 (port: charlesvestal) |
| [SF2 Synth](https://github.com/charlesvestal/move-everything-sf2) | SoundFont (.sf2) synthesizer using FluidLite | FluidLite by Robin Lobel (port: charlesvestal) |
| [SFZ Player](https://github.com/charlesvestal/move-everything-sfz) | SFZ and DecentSampler (.dspreset) sample player using sfizz engine | sfizz by sfztools (port: charlesvestal) |
| [Mini-JV](https://github.com/charlesvestal/move-everything-jv880) | ROM-based PCM rompler emulator | nukeykt/giulioz (port: charlesvestal) |
| [OB-Xd](https://github.com/charlesvestal/move-everything-obxd) | Virtual analog synthesizer based on Oberheim OB-X | reales (port: charlesvestal) |
| [Braids](https://github.com/charlesvestal/move-everything-braids) | Macro oscillator with 47 synthesis algorithms (Mutable Instruments) | Emilie Gillet (port: charlesvestal) |
| [Hera](https://github.com/charlesvestal/move-everything-hera) | Juno-60 emulation synthesizer with BBD chorus | jpcima (port: charlesvestal) |
| [Surge XT](https://github.com/charlesvestal/move-everything-surge) | Hybrid synthesizer - wavetable, FM, subtractive, and physical modeling with 600+ presets | Surge Synth Team (port: charlesvestal) |
| [RaffoSynth](https://github.com/charlesvestal/move-everything-moog) | Monophonic synthesizer with 4 oscillators and Moog ladder filter | Nicolas Roulet, Julian Palladino (port: charlesvestal) |
| [Webstream](https://github.com/charlesvestal/move-everything-webstream) | Web audio search and streaming generator with bundled yt-dlp + ffmpeg runtime | charlesvestal |
| [Radio Garden](https://github.com/charlesvestal/move-everything-radiogarden) | Browse and stream live radio from 200 cities worldwide via Radio Garden | charlesvestal |
| [AirPlay](https://github.com/charlesvestal/move-everything-airplay) | AirPlay audio receiver - stream from iPhone, iPad, or Mac to Move's signal chain | charlesvestal |
| [Chiptune](https://github.com/charlesvestal/move-everything-chiptune) | NES 2A03 & Game Boy DMG chiptune synthesizer with 32 presets | blargg (port: charlesvestal) |
| [Osirus](https://github.com/charlesvestal/move-everything-virus) | Access Virus DSP56300 emulator via Gearmulator JIT engine | dsp56300/gearmulator (port: charlesvestal) |
| [Granny](https://github.com/handcraftedcc/move-everything-granny) | Granular sample instrument with scan controls and file browser | handcraftedcc |
| [MrHyde](https://github.com/handcraftedcc/move-everything-mrhyde) | MicroFreak-inspired macro oscillator based on Mutable Instruments Plaits | handcraftedcc |
| [MrDrums](https://github.com/handcraftedcc/move-everything-mrdrums) | 16-pad sample drum module with per-pad controls and dynamic pad editing | handcraftedcc |
| [REX Player](https://github.com/charlesvestal/move-everything-rex) | Propellerhead ReCycle (.rx2/.rex) slice player with DWOP lossless decoder | charlesvestal |
| [HUSH ONE](https://github.com/charlesvestal/move-everything-hush1) | Monophonic subtractive synthesizer emulating the Roland SH-101 | charlesvestal |
| [NuSaw](https://github.com/charlesvestal/move-everything-nusaw) | Polyphonic detuned multi-saw synthesizer with resonant filter, dual envelopes, chorus, and delay | charlesvestal |
| [Plaits](https://github.com/j3threejay/move-anything-plaits) | Macro oscillator with 24 synthesis engines (Mutable Instruments Plaits) | Emilie Gillet (port: charlesvestal) |
| [Sample Slicer](https://github.com/j3threejay/move-anything-slicer) | Transient-detecting sample slicer with 32-pad polyphonic playback | Justin Joe |

### Audio FX

| Module | Description | Author |
|--------|-------------|--------|
| [CloudSeed](https://github.com/charlesvestal/move-everything-cloudseed) | Algorithmic reverb by Ghost Note Audio | Ghost Note Audio (port: charlesvestal) |
| [TAPESCAM](https://github.com/charlesvestal/move-everything-tapescam) | Tape saturation and degradation effect | Charles Vestal |
| [PSX Verb](https://github.com/charlesvestal/move-everything-psxverb) | PlayStation 1 SPU reverb emulation | Charles Vestal |
| [TapeDelay](https://github.com/charlesvestal/move-everything-space-delay) | Tape delay with flutter and tone shaping | Charles Vestal |
| [Junologue Chorus](https://github.com/charlesvestal/move-everything-junologue-chorus) | Junologue Chorus - Juno-60 chorus emulation (I, I+II, II modes) | Peter Allwin (port: charlesvestal) |
| [NAM](https://github.com/charlesvestal/move-everything-nam) | Neural Amp Modeler - neural network guitar amp/effect emulation | NeuralAudio by Mike Oliphant (port: charlesvestal) |
| [Ducker](https://github.com/charlesvestal/move-everything-ducker) | MIDI-triggered sidechain ducker - classic pumping without an audio sidechain | charlesvestal |
| [CLAP FX](https://github.com/charlesvestal/move-everything-clap) | Host for CLAP audio effect plugins | charlesvestal |
| [Gate](https://github.com/charlesvestal/move-everything-gate) | Noise gate and downward expander | charlesvestal |
| [Key Detect](https://github.com/charlesvestal/move-everything-keydetect) | Detects the musical key of audio passing through it | charlesvestal |
| [Vocoder](https://github.com/charlesvestal/move-everything-vocoder) | Channel vocoder - uses mic/line-in as modulator to shape synth carrier | charlesvestal |
| [Usefulity](https://github.com/charlesvestal/move-everything-usefulity) | Stereo utility - channel select, width, bass mono, gain, pan, phase, mute, DC filter | charlesvestal |
| [Boris Granular](https://github.com/fillioning/move-anything-boris) | Real-time granular audio effect with live input capture and MIDI sync | Alessandro Gaiba (port: fillioning) |
| [Super Boom](https://github.com/fillioning/super-boom-move) | OTO Boum-inspired master bus destructor with 8-band filterbank, 10 preamp models, vocoder mode, and tape stage | fillioning |
| [Verglas](https://github.com/fillioning/move-everything-verglas) | Mutable Instruments Clouds granular processor â€” granular, stretch, looper, and spectral modes with output filters and limiter | Emilie Gillet (port: fillioning) |
| [Dragonfly Hall](https://github.com/wolfrenegade1976/move-anything-dragonfly-hall) | Dragonfly Hall Reverb â€” lush hall reverb with 25 presets and full parameter control | bradcoomber |
| [Punch-In FX](https://github.com/fillioning/MovePunchFX) | PO-33 style punch-in effects with pressure control — 16 effects on left 4x4 pad grid | fillioning |

### MIDI FX

| Module | Description | Author |
|--------|-------------|--------|
| [Super Arp](https://github.com/handcraftedcc/move-everything-superarp) | Advanced MIDI arpeggiator with progression patterns, rhythm presets, and seeded modifiers | handcraftedcc |
| [Eucalypso](https://github.com/handcraftedcc/move-everything-eucalypso) | Deterministic 4-lane Euclidean MIDI sequencer with held/scale note registers, retrigger modes, and seeded modulation | handcraftedcc |

### Overtake

| Module | Description | Author |
|--------|-------------|--------|
| [M8 LPP Emulator](https://github.com/charlesvestal/move-everything-m8) | Novation Launchpad Pro emulation for Dirtywave M8 | bobbydigitales (port: charlesvestal) |
| [SID Control](https://github.com/charlesvestal/move-everything-sidcontrol) | MIDI controller for SIDaster III synthesizer | charlesvestal |
| [Custom MIDI Control](https://github.com/chaolue/move-anything-control) | Custom MIDI controller with 16 banks of configurable pads/knobs/buttons | chaolue |
| [Performance FX](https://github.com/charlesvestal/move-everything-performance-fx) | 32 punch-in audio FX with pressure control, latch, and tempo sync | charlesvestal |

### Tools

| Module | Description | Author |
|--------|-------------|--------|
| [AutoSample](https://github.com/charlesvestal/move-everything-autosample) | Autosample external MIDI gear to create multisampled SFZ instruments | charlesvestal |
| [Wave Edit](https://github.com/charlesvestal/move-everything-waveform-editor) | Trim, gain adjust, and edit audio files on the Move | charlesvestal |
| [Time Stretch](https://github.com/charlesvestal/move-everything-stretch) | Real-time audio time stretching with Bungee | charlesvestal |
| [Stems](https://github.com/charlesvestal/move-everything-stems) | Separate audio into stems: drums, vocals, accompaniment (0.5x realtime) | charlesvestal |
| [DJ Deck](https://github.com/djhardrich/move-anything-dj) | CDJ/turntable-style 4-track stem player with Bungee timestretch/pitchshift | DJ Hard Rich |
| [Tuner](https://github.com/CatsAreCool710/Move-Everything-Tuner) | Chromatic and instrument tuner with step guide feedback | Jeremiah Ticket |

<!-- MODULE_TABLE_END -->

## Related Repositories

**Installer:**
- [move-everything-installer](https://github.com/charlesvestal/move-everything-installer) - Cross-platform desktop installer (macOS, Windows, Linux)

## Community

- Discord: [https://discord.gg/Zn33eRvTyK](https://discord.gg/GHWaZCC9bQ)
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal


## AI Assistance Disclaimer

This module is part of Move Everything and was developed with AI assistance, including Claude, Codex, and other AI assistants.

All architecture, implementation, and release decisions are reviewed by human maintainers.  
AI-assisted content may still contain errors, so please validate functionality, security, and license compatibility before production use.
