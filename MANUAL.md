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

### Prerequisites

1. Your Move must be connected to WiFi
2. You need a computer on the same network with SSH access

### Setup SSH Access

1. On your computer, generate an SSH key if you don't have one:
   ```
   ssh-keygen -t ed25519
   ```

2. Copy your public key (usually `~/.ssh/id_ed25519.pub`)

3. Open a browser and go to:
   ```
   http://move.local/development/ssh
   ```

4. Paste your public key and save

5. Test the connection:
   ```
   ssh ableton@move.local
   ```

### Install Move Everything

Run the installer:
```
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/install.sh | sh
```

The installer will ask if you want to install all available modules. Choose:
- **Yes** to download all synths and effects (recommended for first install)
- **No** to install just the core - you can add modules later via the Module Store

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
| **Forward Ch** | MIDI channel sent to the synth module (use for multitimbral patches) |

### Slot Presets

- **Save / Save As**: Save the entire slot configuration (all modules + settings)
- **Delete**: Remove a saved preset
- **Load**: Scroll left to the slot overview, click to see saved presets

---

## Master FX

Access via **Shift+Vol + Menu**. Contains four audio effect slots that process the mixed output of all instrument slots.

---

## Connecting to Move Tracks

1. Set a Move track's **MIDI Out** to a channel (1-4)
2. Set the corresponding slot's **Receive Ch** to match
3. Play the Move track - its MIDI triggers the slot's synth

Move Everything also forwards pitch bend, mod wheel, sustain, and other CCs from external MIDI controllers.

---

## Overtake Modules

Overtake modules take full control of Move's display and controls. Access via **Shift+Vol + Jog Click**.

To exit an overtake module: **Shift+Vol + Jog Click** (works anytime)

---

## Built-in Modules

These modules are included with Move Everything:

**MIDI FX:**
- **Chords** - Chord generator with shapes, inversions, voicing, and strum
- **Arpeggiator** - Pattern-based arpeggiator (up, down, up/down, random)

**Audio FX:**
- **Freeverb** - Simple, effective reverb

**Overtake:**
- **MIDI Controller** - 16-bank MIDI controller with customizable knob/pad assignments

---

## Module Store

Access via Standalone Mode (**Shift+Vol + Knob 8**), then select Module Store.

The store lets you download additional modules:

**Sound Generators:**
- **Dexed** - 6-operator FM synthesizer (DX7 compatible)
- **SF2** - SoundFont player
- **Mini-JV** - Roland JV-880 emulation
- **OB-Xd** - Oberheim-style virtual analog

**Audio Effects:**
- **CloudSeed** - Shimmer reverb
- **PSXVerb** - PlayStation-style reverb
- **TapeDelay** - RE-201 Space Echo style delay
- **TAPESCAM** - Tape saturation/degradation

**Overtake:**
- **M8 LPP** - Launchpad Pro emulator for Dirtywave M8
- **SID Control** - Controller for SIDaster III

**Note:** Some modules require additional files (patches, ROMs, SoundFonts). Check each module's documentation.

---

## Standalone Mode

Access via **Shift+Vol + Knob 8**. Includes the full Module Store for downloading modules and updating Move Everything. Also lets you run modules without Move's audio.

---

## Tips

- Slot settings persist between sessions
- Use the Module Store (in standalone mode) to update Move Everything itself
- If something goes wrong, use Move's DFU restore mode to reset
