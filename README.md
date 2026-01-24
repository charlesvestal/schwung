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
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

## Usage

- **Launch**: Hold Shift + touch Volume knob + Knob 8
- **Exit**: Hold Shift + click Jog wheel
- **Back**: Return to the host menu (unless a module sets `raw_ui`)
- **Settings**: Menu → Settings for velocity/aftertouch; "Return to Move" exits to the stock app

## Built-in Modules

| Module | Description |
|--------|-------------|
| **Signal Chain** | Modular signal chain with synths, MIDI FX, and audio FX |
| **MIDI Controller** | MIDI controller with 16 banks |
| **Module Store** | Browse, install, and update external modules |

### Signal Chain

The Signal Chain module lets you combine MIDI sources, MIDI effects, sound generators, and audio effects into patches.

#### Quick Start

1. Select **Signal Chain** from the main menu
2. Use **jog wheel** to browse patches
3. **Click jog** to load a patch
4. **Play pads** to hear sound
5. Press **Menu** to access the synth's UI (if available)
6. Press **Back** to return to patch list
7. **Up/Down** to transpose octave while playing

#### Components

**Sound Generators:**
- Line In (external audio input, built-in)
- SF2, Dexed, OB-Xd, Mini-JV, CLAP (install via Module Store)

**MIDI Effects:**
- Chord generator (major, minor, power, octave)
- Arpeggiator (up, down, up-down, random)

**Audio Effects:**
- Freeverb (Schroeder-Moorer reverb, built-in)
- CloudSeed, PSX Verb, TAPESCAM, TapeDelay (install via Module Store)

#### Recording

Signal Chain can record audio output to WAV files:

- **Record Button**: Toggle recording on/off (requires a patch to be loaded)
- **LED Feedback**: Off (no patch), White (ready), Red (recording)
- **Output**: `/data/UserData/move-anything/recordings/rec_YYYYMMDD_HHMMSS.wav`
- **Format**: 44.1kHz, 16-bit stereo WAV

#### Creating Custom Patches

Patches are JSON files in `modules/chain/patches/`. Create your own:

```json
{
  "name": "My Patch",
  "version": 1,
  "chain": {
    "synth": { "module": "dexed" },
    "audio_fx": [
      { "type": "freeverb", "params": { "wet": 0.3 } }
    ]
  }
}
```

Restart Signal Chain to pick up new patches.

## Shadow Mode

Shadow Mode lets you run custom signal chains **alongside** stock Ableton Move without replacing it. Play pads and hear both Move's sounds and your custom synths mixed together.

### How It Works

- **Toggle**: Shift + touch Volume knob + touch Knob 1 (while Move is running)
- **Display**: Shadow UI takes over the screen to show slots and patches
- **Audio**: Shadow synths mix with Move's audio output
- **MIDI Routing**: Move tracks 1-4 send MIDI on channels 5-8 to shadow slots

### Shadow Slots

Shadow mode provides 4 independent slots, each with its own chain patch:

| Slot | MIDI Channel | Default Use |
|------|--------------|-------------|
| Slot A | Ch 5 | Move Track 1 |
| Slot B | Ch 6 | Move Track 2 |
| Slot C | Ch 7 | Move Track 3 |
| Slot D | Ch 8 | Move Track 4 |

### Shadow UI Navigation

**Quick Access Shortcuts (work from anywhere):**
- **Shift+Vol+Track 1-4**: Jump directly to that slot's settings
- **Shift+Vol+Menu**: Jump directly to Master FX settings
- **Shift+Vol+Knob1**: Toggle shadow mode on/off

**While in Shadow UI:**
- **Jog wheel**: Navigate menus and adjust values
- **Jog click**: Select / confirm
- **Back**: Exit to Move (from slot or Master FX view)
- **Track buttons 1-4**: Switch to that slot's settings
- **Knobs 1-8**: Adjust parameters of current slot/effect
- **Touch knob**: Peek at current value without changing it

### Master FX

Shadow mode includes a 4-slot Master FX chain that processes the mixed output of all shadow slots. Access via Shift+Vol+Menu or by scrolling past the slots.

### Shift+Knob Control (Move Mode)

While using stock Move (not in Shadow UI), hold Shift to control shadow synth parameters:

- **Shift + turn knob**: Adjust shadow slot parameter with overlay feedback
- **Shift + touch knob**: Peek at current value

### Shadow UI Store Picker

Install modules directly from Shadow UI without leaving shadow mode:

1. When selecting a synth or effect slot, choose **[Get more...]**
2. Browse modules by category
3. Select a module to see details and install/update/remove options
4. Newly installed modules are immediately available

### Extending Shadow Mode

Shadow mode uses the same chain module and patches as standalone Move Anything. **Any module you install via Module Store is automatically available in shadow mode** - no recompilation required.

To add new synths or effects to shadow mode:

1. Install the module via Module Store or Shadow UI Store Picker
2. Create a chain patch in `modules/chain/patches/` that references the module
3. Select the patch in the shadow UI

See [MODULES.md](docs/MODULES.md#shadow-mode-integration) for details on creating shadow-compatible patches.

## External Modules (via Module Store)

Install these from the Module Store on your device:

**Sound Generators:**
| Module | Description |
|--------|-------------|
| SF2 Synth | SoundFont (.sf2) synthesizer |
| Dexed | 6-operator FM synth (loads .syx patches) |
| OB-Xd | Oberheim OB-X synthesizer emulator |
| Mini-JV | ROM-based PCM rompler (requires ROM files) |
| CLAP Host | Host for CLAP audio plugins |

**Audio FX:**
| Module | Description |
|--------|-------------|
| CloudSeed | Algorithmic reverb |
| PSX Verb | PlayStation 1 SPU reverb emulation |
| TAPESCAM | Tape saturation and degradation |
| TapeDelay | Tape delay with flutter and tone shaping |

**Utilities:**
| Module | Description |
|--------|-------------|
| M8 LPP | Dirtywave M8 Launchpad Pro emulation |

### Using the Module Store

1. Select **Module Store** from the main menu
2. Browse by category: Sound Generators, Audio FX, Utilities
3. Use **jog wheel** to highlight a module
4. **Click jog** to see module details
5. Select **Install** to download and install

**Updating modules:** Modules with available updates show a version badge. Select the module and choose **Update**.

**Removing modules:** Select an installed module and choose **Remove**.

Installed modules appear in the main menu immediately after installation.

## Uninstall

```bash
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh
```

## Building

See [BUILDING.md](BUILDING.md) for build instructions.

## Documentation

- [How It Works](docs/ARCHITECTURE.md) - System architecture and module loading
- [Hardware API Reference](docs/API.md) - JavaScript API for modules
- [Module Development Guide](docs/MODULES.md) - Creating your own modules
- [Signal Chain Module Notes](src/modules/chain/README.md)

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

## Community

- Discord: https://discord.gg/Zn33eRvTyK
- Contributors: @talktogreg, @impbox, @deets, @bobbyd, @chaolue, @charlesvestal

## License

CC BY-NC-SA 4.0 - See [LICENSE](LICENSE) and [THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES)
