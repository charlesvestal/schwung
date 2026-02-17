# Move Everything Manual

## What is Move Everything?

Move Everything adds four instrument slots that run in "shadow mode" alongside your normal Move tracks. Each slot can host synthesizers and effects that mix with Move's audio output.

**Key concepts:**
- **Shadow Mode**: A custom UI accessed via keyboard shortcuts
- **Slots**: Four instrument slots plus a Master FX slot
- **Modules**: Synthesizers, audio effects, and MIDI effects that run in each slot
- **Overtake Modules**: Full-screen applications that take over Move's UI (like MIDI controllers)

---

## Important Notice

This is an unofficial project that modifies the software on your Ableton Move. Back up any important sets and samples before installing. Familiarize yourself with Move's DFU restore mode (on Centercode) in case you need to restore your device.

Move still works normally after installation - Move Everything runs alongside it.

---

## Installation

### Desktop Installer (Recommended)

Download the [Move Everything Installer](https://github.com/charlesvestal/move-everything-installer/releases/latest) for your platform (macOS, Windows, Linux). It handles SSH setup, module selection, and upgrades via a graphical interface.

### Command Line

**Prerequisites:** Move connected to WiFi, a computer on the same network.

Run:
```
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

The installer will guide you through SSH setup, download the framework, and offer to install modules.

After installation, Move will restart automatically.

---

## Shortcuts

All shortcuts use **Shift + touch Volume knob** as a modifier:

| Shortcut | Action |
|----------|--------|
| **Shift+Vol + Track 1-4** | Open that slot's editor |
| **Shift+Vol + Menu** | Open Master FX |
| **Shift+Vol + Jog Click** | Open Overtake menu (or exit Overtake mode) |
| **Shift+Vol + Knob 8** | Open Standalone Mode |
| **Shift+Sample** | Open Quantized Sampler |
| **Shift+Capture** | Skipback (save last 30 seconds) |

**Tip:** You can access slots directly from normal Move mode - you don't need to be in shadow mode first.

---

## Instrument Slots

Each of the four instrument slots contains a signal chain:

```
MIDI FX → Sound Generator → Audio FX 1 → Audio FX 2
```

### Navigating a Slot

- **Jog wheel**: Scroll between chain positions (MIDI FX, Synth, FX1, FX2, Settings)
- **Jog click**: Enter the selected position
- **Back button**: Go back one level

### Selecting Modules

1. Navigate to an empty position and click the jog wheel
2. Choose from installed modules of that type
3. To change an existing module: hold **Shift** and click the jog wheel

### Using Modules

Most modules have:
- **Preset browser**: Use jog wheel to browse presets, click to select
- **Parameter menu**: Editable settings organized hierarchically
- **Knob assignments**: Knobs 1-8 control relevant parameters when a module is selected

---

## Slot Settings

The last position in each slot contains settings:

| Setting | Description |
|---------|-------------|
| **Knob 1-8** | Assign any module parameter to a knob. These work even in normal Move mode (hold Shift + turn knob). |
| **Volume** | Slot volume level |
| **Receive Ch** | MIDI channel this slot listens to (match your Move track's MIDI Out) |
| **Forward Ch** | MIDI channel sent to the synth module (see below) |

**Forward Channel modes:**
- **Auto** (default): Remaps MIDI to the slot's receive channel. If Receive Ch is "All", passes through unchanged.
- **Thru**: Passes the original MIDI channel through unchanged — useful for multitimbral synths that respond differently on each channel.
- **1-16**: Forces all MIDI to a specific channel regardless of what was received.

### Slot Presets

- **Save / Save As**: Save the entire slot configuration (all modules + settings)
- **Delete**: Remove a saved preset
- **Load**: Scroll left to the slot overview, click to see saved presets

---

## Connecting to Move Tracks

1. Set a Move track's **MIDI Out** to a channel (1-4)
2. Set the corresponding slot's **Receive Ch** to match
3. Play the Move track - its MIDI triggers the slot's synth

Move Everything also forwards pitch bend, mod wheel, sustain, and other CCs from external MIDI controllers.

---

## Master FX

Access via **Shift+Vol + Menu**. Contains four audio effect slots that process the mixed output of all instrument slots.

---

## Routing Move Tracks Through Custom FX

On Move firmware 2.0.0+, you can route Move's own track audio through Move Everything's audio effects using Link Audio. This lets you add effects like CloudSeed reverb, TapeDelay, or NAM amp models to your native Move tracks.

### Setup

1. **Enable Link on Move**: Go to Move's Settings > Link and toggle it on. This runs entirely on-device — no WiFi or USB connection is needed.
2. **Install or update Move Everything** — the installer enables Link Audio automatically.

### How It Works

When Link Audio is enabled, Move streams each track's audio separately. Move Everything intercepts these streams and routes them through the audio FX in the corresponding slot:

```
Move Track 1 audio → Slot 1 Audio FX 1 → Audio FX 2 → mixed output
Move Track 2 audio → Slot 2 Audio FX 1 → Audio FX 2 → mixed output
...
```

This means you can set up a slot with just audio effects (no synth) and it will process that Move track's audio.

### Example: Adding Reverb to a Move Track

1. Open Slot 1 (**Shift+Vol + Track 1**)
2. Navigate to **Audio FX 1** and load CloudSeed (or any audio effect)
3. Optionally load a second effect in **Audio FX 2**
4. Play Move Track 1 — you'll hear it processed through your effects

### Notes

- Move must be on firmware **2.0.0 or later** for Link Audio support
- Each Move track maps to the matching slot number (Track 1 → Slot 1, etc.)
- A slot can have both a synth (triggered by MIDI) and audio FX (processing Move's track audio) simultaneously

---

## Native Sampler Bridge

Move Everything audio can be fed into Move's native sampler for resampling.

In **Master FX > Settings**, `Resample Src` controls this:

| Option | Behavior |
|--------|----------|
| **Off** | Disabled (default) |
| **Replace** | Replaces native sampler input with Move Everything master output |

Recommended setup to avoid feedback:
1. Set `Resample Src` to **Replace**
2. In native Move Sampler, set source to **Line In**
3. Set monitoring to **Off**

If monitoring is on or routing is configured differently, audio feedback may occur.

---

## Recording

### Quantized Sampler

Access via **Shift+Sample**. Records Move's audio output (including Move Everything synths) to WAV files, quantized to bars.

**Options:**
- **Source**: `Resample` (Move's mixed output including Move Everything) or `Move Input` (whatever is set in Move's sample input - line-in, mic, etc.)
- **Duration**: Until stopped, 1, 2, 4, 8, or 16 bars

**Usage:**
1. Press **Shift+Sample** to open the sampler
2. Use the jog wheel to select source and duration
3. Recording starts on a note event or pressing Play
4. Press **Shift+Sample** again to stop (or it stops automatically at the set duration)

Recordings are saved to `Samples/Move Everything/`.

Uses MIDI clock for accurate bar timing, falling back to project tempo if no clock is available. You can also use Move's built-in count-in for line-in recordings.

### Skipback

Press **Shift+Capture** to save the last 30 seconds of audio to disk.

Move Everything continuously maintains a 30-second rolling buffer of audio. When triggered, it dumps this buffer to a WAV file instantly without interrupting playback.

Files are saved to `Samples/Move Everything/Skipback/`. Uses the same source setting as the Quantized Sampler (Resample or Move Input).

---

## Available Modules

### Built-in

These modules are included with Move Everything:

**MIDI FX:**
- **Chords** - Chord generator with shapes, inversions, voicing, and strum
- **Arpeggiator** - Pattern-based arpeggiator (up, down, up/down, random)

**Audio FX:**
- **Freeverb** - Simple, effective reverb

**Overtake:**
- **MIDI Controller** - 16-bank MIDI controller with customizable knob/pad assignments

### Module Store

When selecting a module, "[Get more...]" opens the Module Store to download additional modules. To update Move Everything itself, access Module Store via Standalone Mode (**Shift+Vol + Knob 8**), then select Module Store.

**Sound Generators:**
- **Braids** - Mutable Instruments macro oscillator (47 algorithms)
- **Dexed** - 6-operator FM synthesizer (DX7 compatible)
- **SF2** - SoundFont player (requires .sf2 files)
- **Mini-JV** - Roland JV-880 emulation (requires ROM files)
- **OB-Xd** - Oberheim-style virtual analog
- **Hera** - Juno-60 emulation with BBD chorus
- **Surge XT** - Hybrid synthesizer (wavetable, FM, subtractive, physical modeling)
- **RaffoSynth** - Monophonic synth with Moog ladder filter
- **Webstream** - Web audio search and streaming

**Audio Effects:**
- **CloudSeed** - Algorithmic reverb
- **PSXVerb** - PlayStation-style reverb
- **TapeDelay** - RE-201 Space Echo style delay
- **TAPESCAM** - Tape saturation/degradation
- **Junologue Chorus** - Juno-60 chorus emulation (I, I+II, II modes)
- **NAM** - Neural Amp Modeler (requires .nam model files)
- **Ducker** - MIDI-triggered sidechain ducker

**Overtake/Utility:**
- **Four Track** - Four-track recorder
- **M8 LPP** - Launchpad Pro emulator for Dirtywave M8
- **SID Control** - Controller for SIDaster III

**Note:** Some modules require additional files (ROMs, SoundFonts, .nam models). Check each module's documentation.

---

## Overtake Modules

Overtake modules take full control of Move's display and controls. Access via **Shift+Vol + Jog Click**.

To exit an overtake module: **Shift+Vol + Jog Click** (works anytime)

**Note:** After exiting an overtake module, Move's pad and button LEDs won't refresh automatically. Change tracks or sets and they'll come back as they light back on.

---

## Screen Reader

Move Everything includes an optional screen reader for accessibility, using text-to-speech to announce UI elements.

Toggle via **Shadow UI > Settings > Screen Reader**, or **Shift+Menu** when Shadow UI is disabled.

Settings:
- **Speed**: 0.5x to 2.0x
- **Pitch**: Low to high
- **Volume**: 0-100

Can be enabled during installation with `--enable-screen-reader`.

---

## Display Mirror

Stream Move's 128x64 OLED display to any browser on your network in real time. Useful for remote monitoring, screen capture, or development.

### Setup

1. Open **Master FX > Settings** (Shift+Vol + Menu, then scroll to Settings)
2. Toggle **Mirror Display** to **On**
3. Open `http://move.local:7681` in a browser

The display updates at ~30 fps and shows whatever is on screen - both normal Move UI and Shadow UI.

### Notes

- Mirror Display is **off by default** and must be enabled via the settings toggle
- The setting persists across reboots
- The display server runs on port 7681 and starts automatically at boot
- When mirroring is off, the server is running but idle (no overhead from the shim)
- Multiple browsers can connect simultaneously (up to 8 clients)

---

## Tips

- Slot settings persist between sessions
- Use **Standalone Mode** (**Shift+Vol + Knob 8**) to run modules without Move's audio, or to access the Module Store for updates
- If something goes wrong, use Move's DFU restore mode to reset
