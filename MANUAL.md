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

### Module Management (Command Line)

Install, remove, or update individual modules without reinstalling the full framework.

**Install a module from GitHub:**
```
./scripts/install.sh install-module-github charlesvestal/move-anything-braids
```

**Install from a specific branch:**
```
./scripts/install.sh install-module-github charlesvestal/move-anything-braids/dev
```

**Install from a local tarball:**
```
./scripts/install.sh install-module braids-module.tar.gz
```

**Remove a module:**
```
./scripts/install.sh uninstall-module braids
```

The installer auto-detects the module's type (sound generator, audio FX, etc.) and places it in the correct directory.

---

## Uninstall

Run:
```
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh
```

By default, uninstall exports inactive Set Pages backups to `/data/UserData/UserLibrary/Move Everything Backups/Set Pages/` before removing Move Everything.

To skip that export and permanently delete Move Anything data:
```
curl -L https://raw.githubusercontent.com/charlesvestal/move-anything/main/scripts/uninstall.sh | sh -s -- --purge-data
```

---

## Shortcuts

All shortcuts use **Shift + touch Volume knob** as a modifier:

| Shortcut | Action |
|----------|--------|
| **Shift+Vol + Track 1-4**| Open that slot's editor |
| **Shift+Vol + Note/Session** | Open Master FX |
| **Shift+Vol + Step 2** | Open Global Settings |
| **Shift+Vol + Step 13** | Open Tools Menu |
| **Shift+Vol + Jog Click** | Open Overtake menu (or exit Overtake mode) |
| **Shift+Sample** | Open Quantized Sampler |
| **Shift+Capture** | Skipback (save last 30 seconds) |
| **Shift+Vol + Left/Right** | Switch set page (when enabled) |

**Tip:** You can access slots directly from normal Move mode - you don't need to be in shadow mode first.

### Overlay Knob Shortcut

The knob parameter overlay shown in native Move mode has a separate trigger setting:

- Go to **Global Settings > Display > Overlay Knobs**
- Choose **+Shift** (default), **+Jog Touch**, **Off**, or **Native**
- If native Move's **Shift+Knob** actions (like fine control) are getting blocked, switch this to **+Jog Touch** or **Off**

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

**Tip:** To swap a module that's already loaded, highlight it in the slot editor and hold **Shift + Jog Click**. This opens the module picker so you can replace it without clearing the slot first.

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
| **LFO 1 / LFO 2** | Modulation LFOs (see below) |

**Forward Channel modes:**
- **Auto** (default): Remaps MIDI to the slot's receive channel. If Receive Ch is "All", passes through unchanged.
- **Thru**: Passes the original MIDI channel through unchanged — useful for multitimbral synths that respond differently on each channel.
- **1-16**: Forces all MIDI to a specific channel regardless of what was received.

### Slot LFOs

Each slot has two independent LFOs that can modulate any parameter of any module in the slot's chain (synth, audio FX, MIDI FX), or each other.

**LFO settings:**
- **Target**: Which component and parameter to modulate
- **Enabled**: On/Off
- **Shape**: Sine, Tri, Saw, Square, S&H (sample & hold), Swishy (smooth random walk)
- **Sync**: Free-running (Hz) or tempo-synced (musical divisions from 8 bars to 1/32)
- **Depth**: Modulation amount (0-100%)
- **Phase**: Phase offset (0-360 degrees)
- **Retrigger**: Reset LFO phase on the first note-on of a new phrase

LFOs can also target each other's parameters (depth, rate, phase offset) for complex modulation.

An indicator (`~1`, `~2`, or `~1+2`) appears above targeted components in the chain editor.

### Slot Presets

- **Save / Save As**: Save the entire slot configuration (all modules + settings)
- **Delete**: Remove a saved preset
- **Load**: Scroll left to the slot overview, click to see saved presets

---

## Per-Set Slot State

Each Move Set maintains its own independent slot configurations. When you switch Sets, Move Everything automatically saves the current slots and loads the slots associated with the new Set. A brief "Set Loaded" overlay confirms the switch.

**How it works:**
- Each Set remembers which synths, effects, and settings are loaded in each slot, plus Master FX
- Switching Sets saves the outgoing state and restores the incoming Set's state
- Changes you make to a slot are local to the current Set — they don't affect other Sets

**Presets vs Set State:**
- **Set State** is automatic: whatever you have loaded in your slots is saved when you leave a Set and restored when you come back
- **Slot Presets** are separate: loading a preset copies it into the Set's state. If you tweak it afterwards, the tweaks live in the Set, not back in the preset. Loading the same preset in another Set gives you a fresh copy.

**New Sets** start with empty slots. To carry a configuration to a new Set, save it as a slot preset first, then load it in the new Set.

---

## Set Pages

Set Pages let you organize your sets into 8 switchable pages. Each page holds its own collection of sets, so you can group sets by project, genre, or live performance.

### Usage

- **Shift+Vol+Left**: Switch to the previous page
- **Shift+Vol+Right**: Switch to the next page

A toast overlay shows "Loading Page X/8..." during the switch. Move restarts automatically to load the new page's sets.

### How It Works

When you switch pages, Move Everything:
1. Saves the current set (if dirty)
2. Moves all sets from `Sets/` into a stash directory for the current page
3. Moves the target page's sets from its stash into `Sets/`
4. Restarts Move to load the new sets

Each page's sets are completely independent. Per-set slot state (synths, effects, settings) is preserved per-set as usual.

### Settings

Set Pages is **enabled by default**. To disable:
1. Open **Global Settings** (**Shift+Vol + Step 2**)
2. Navigate to **Set Pages**
3. Toggle **Set Pages** to **Off**

The setting takes effect immediately (no restart needed) and persists across reboots.

---

## Connecting to Move Tracks

1. Set a Move track's **MIDI Out** to a channel (1-4)
2. Set the corresponding slot's **Receive Ch** to match
3. Play the Move track - its MIDI triggers the slot's synth

**Tip:** To prevent the native Move synth from playing on top of your ME synth, load an empty Drum Rack or Sampler preset on the Move track. This silences the native sound while still sending MIDI to Move Everything.

Move Everything also forwards pitch bend, mod wheel, sustain, and other CCs from external MIDI controllers.

**Tip:** Some synths and FX (i.e. Arp) utilize Midi Cloc for tempo sync. Make sure your Move is set to "Midi Clock: Out" for these to pick up sync correctly. 

---

## Master FX

Access via **Shift+Vol + Note/Session**. Contains four audio effect slots that process the mixed output of all instrument slots.

### Master FX LFOs

The Master FX chain also has two LFOs (**LFO 1** and **LFO 2** in the Master FX menu) that can target any parameter of the loaded master effects. These work the same as slot LFOs (same shapes, sync options, depth, and phase) but operate on the master FX chain rather than individual slots.

Global settings (Display, Audio, Screen Reader, Set Pages, Services, Updates, Help) are accessed via **Shift+Vol + Step 2**.

---

## Link Audio (Move 2.0+)

On Move firmware 2.0.0+, Link Audio lets you route Move's own track audio through Move Everything's effects. This gives you access to effects like CloudSeed reverb, TapeDelay, or NAM amp models on your native Move tracks — but it changes how audio is mixed. Understanding the tradeoffs helps you decide when to use it.

### Link Audio On vs Off

| | **Link Audio On** | **Link Audio Off** |
|---|---|---|
| **ME synths** | Processed through slot FX and Master FX | Processed through slot FX and Master FX |
| **Native Move tracks** | Routed through ME slot FX per track | Stay on Move's native path |
| **Move's native Master FX** | Bypassed — ME rebuilds the mix from per-track streams | Active — applied to Move tracks normally |
| **ME Master FX** | Processes everything (Move tracks + ME synths) | Processes everything (Move post-native-FX + ME synths) |
| **Play delay** | Brief delay when pressing Play (Link quantum sync) | No delay |

### How It Works

**Link Audio On:** Move streams each track's audio separately via the Link protocol. Move Everything intercepts these per-track streams, runs them through the corresponding slot's audio FX (combined with any ME synth in that slot), and reconstructs the final mix. Because ME is working with pre-mix audio, Move's native Master FX is bypassed entirely.

```
Move Track 1 → Slot 1 FX → ┐
Move Track 2 → Slot 2 FX → ├→ ME Master FX → Output
Move Track 3 → Slot 3 FX → │
Move Track 4 → Slot 4 FX → ┘
(+ ME synths mixed in per slot)
```

**Link Audio Off:** Move's audio goes through its normal path including native Master FX. ME synths are processed through their slot FX and mixed in. Everything combined runs through ME Master FX.

```
Move (all tracks + native Master FX) → ┐
ME Slot 1 (synth → FX) ────────────────├→ ME Master FX → Output
ME Slot 2 (synth → FX) ────────────────│
...                                     ┘
```

### Setup

1. **Enable Link on Move**: Go to Move's Settings > Link and toggle it on. This runs entirely on-device — no WiFi or USB connection is needed.
2. **Install or update Move Everything** — the installer enables Link Audio support, but routing is off by default.
3. **Enable routing**: In **Global Settings > Audio** (**Shift+Vol + Step 2**), toggle **Route thru ME** on. This routes Move's per-track audio through ME's slot FX.

**Note:** A restart of Move is sometimes required for the Link Audio subscriber to begin capturing audio. If you don't hear Move tracks being processed after enabling routing, restart Move.

### Example: Adding Reverb to a Move Track

1. Make sure Link Audio is enabled (see Setup above)
2. Open Slot 1 (**Shift+Vol + Track 1**)
3. Navigate to **Audio FX 1** and load CloudSeed (or any audio effect)
4. Optionally load a second effect in **Audio FX 2**
5. Play Move Track 1 — you'll hear it processed through your effects

### Notes

- Move must be on firmware **2.0.0 or later** for Link Audio support
- Each Move track maps to the matching slot number (Track 1 → Slot 1, etc.)
- A slot can have both a synth (triggered by MIDI) and audio FX (processing Move's track audio) simultaneously
- When pressing Play, Move syncs to the Link quantum, which introduces a brief delay before playback starts

---

## Native Sampler Bridge

Move Everything audio can be fed into Move's native sampler for resampling.

In **Global Settings > Audio**, enable **Resample Bridge** to include Move Everything audio when resampling.

The bridge is only active when Move's sample source is set to **Resampling** ("output"). When Move's source is set to Line In, USB, or Mic, the bridge is automatically disabled to prevent feedback.

**Safety features:**
- **Source tracking**: The bridge monitors Move's Settings.json for source changes and deactivates instantly when not resampling
- **Feedback detection**: If audio feedback is detected, the bridge auto-disables and must be manually re-enabled
- **Raw audio input**: Audio-input plugins (Line In, etc.) always read pre-bridge hardware audio, preventing self-feed loops

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
4. Press **Sample** to stop (or it stops automatically at the set duration)

Recordings are saved to `Samples/Move Everything/Resampler/YYYY-MM-DD/`.

Uses MIDI clock for accurate bar timing, falling back to project tempo if no clock is available. You can also use Move's built-in count-in for line-in recordings.

### Skipback

Press **Shift+Capture** (default) to save the last 30 seconds of audio to disk.

Move Everything continuously maintains a 30-second rolling buffer of audio. When triggered, it dumps this buffer to a WAV file instantly without interrupting playback.

The shortcut can be changed in **Global Settings > Audio > Skipback** to **Sh+Vol+Cap** if Shift+Capture conflicts with other uses.

Files are saved to `Samples/Move Everything/Skipback/YYYY-MM-DD/`. Uses the same source setting as the Quantized Sampler (Resample or Move Input).

---

## Available Modules

### Built-in

These modules are included with Move Everything:

**Sound Generators:**
- **Line In** - Line input with conditioning for Line, Guitar, and Phono sources

**MIDI FX:**
- **Chords** - Chord generator with shapes, inversions, voicing, and strum
- **Arpeggiator** - Pattern-based arpeggiator (up, down, up/down, random)
- **Velocity Scale** - Scales MIDI velocity to a configurable min/max range

**Audio FX:**
- **Freeverb** - Simple, effective reverb

**Overtake:**
- **MIDI Controller** - 16-bank MIDI controller with customizable knob/pad assignments

### Module Store

When selecting a module, "[Get more...]" opens the Module Store to download additional modules. To update Move Everything itself, access Module Store via **Global Settings > Updates** (**Shift+Vol + Step 2**).

**Sound Generators:**
- **AirPlay** - AirPlay audio receiver (stream from iPhone, iPad, or Mac)
- **SConnect** - Unofficial Spotify Connect receiver for personal listening
- **Braids** - Mutable Instruments macro oscillator (47 algorithms)
- **Chiptune** - NES 2A03 & Game Boy DMG chiptune synthesizer
- **Dexed** - 6-operator FM synthesizer (DX7 compatible)
- **Hera** - Juno-60 emulation with BBD chorus
- **HUSH ONE** - Monophonic SH-101-style subtractive synthesizer
- **Mini-JV** - Roland JV-880 emulation (requires ROM files)
- **OB-Xd** - Oberheim-style virtual analog
- **Osirus** - Access Virus DSP56300 emulator (requires ROM file)
- **Radio Garden** - Browse and stream live radio from 200 cities worldwide
- **RaffoSynth** - Monophonic synth with Moog ladder filter
- **REX Player** - Propellerhead ReCycle (.rx2/.rex) slice player
- **SF2** - SoundFont player (requires .sf2 files)
- **Surge XT** - Hybrid synthesizer (wavetable, FM, subtractive, physical modeling)
- **Webstream** - Web audio search and streaming

**Audio Effects:**
- **CLAP FX** - Host for CLAP audio effect plugins (requires .clap files)
- **CloudSeed** - Algorithmic reverb
- **Ducker** - MIDI-triggered sidechain ducker
- **Gate** - Noise gate and downward expander
- **Junologue Chorus** - Juno-60 chorus emulation (I, I+II, II modes)
- **Key Detect** - Detects the musical key of audio passing through
- **NAM** - Neural Amp Modeler (requires .nam model files)
- **PSXVerb** - PlayStation-style reverb
- **TapeDelay** - RE-201 Space Echo style delay
- **TAPESCAM** - Tape saturation/degradation
- **Vocoder** - Channel vocoder (mic/line-in as modulator)

**MIDI FX:**
- **Super Arp** - Advanced arpeggiator with progression patterns and rhythm presets

**Overtake/Utility:**
- **Custom MIDI Control** - Custom MIDI controller with 16 banks (community)
- **M8 LPP** - Launchpad Pro emulator for Dirtywave M8
- **SID Control** - Controller for SIDaster III

**Note:** Some modules require additional files (ROMs, SoundFonts, .nam models, .clap plugins). Check each module's documentation.

---

## Overtake Modules

Overtake modules take full control of Move's display and controls. Access via **Shift+Vol + Jog Click**.

To exit an overtake module: **Shift+Vol + Jog Click** (works anytime)

**Note:** After exiting an overtake module, Move's pad and button LEDs won't refresh automatically. Change tracks or sets and they'll come back as they light back on.

---

## Tools Menu

Access via **Shift+Vol + Step 13**. Tools are standalone utilities that run outside the normal slot system.

### File Browser

Browse files and folders on your Move. Two starting roots:
- **User Library**: Your samples, presets, and recordings
- **System Files**: The Move Everything directory (shows a warning before entry)

**Navigation:**
- **Jog wheel**: Scroll through files and folders
- **Jog click**: Open a folder, or show file actions for a file
- **Shift+Jog click**: Show actions for any item (files or folders)
- **Back**: Go up one directory or exit

**File actions:** Play (WAV files), Duplicate, Rename, Delete, Copy to..., Move to..., New Folder (on directories).

**Play:** Selecting Play on a .wav file starts audio preview through Move's speakers. A progress bar and time display are shown. Push jog or press back to stop.

### Song Mode

Sequence clips across time to build full songs from your set's clip layout.

**How it works:** Song Mode reads your set's clips and tempo, then lets you arrange pad assignments into an ordered list of steps. Each step triggers all 4 tracks simultaneously, advancing automatically based on bar timing.

**Building a song:**
1. Select a step with the jog wheel or step buttons
2. Tap pads to assign clips for each track
3. The first pad press on an empty step copies the previous step, so you only change what differs
4. **Shift+Pad** on steps 2+ sets that track to **continue** (shown with a trailing quote mark: `"`) so it won't retrigger
5. Steps display track assignments as clip letters (A-H), or a trailing quote (`"`) for continue

**Playback:**
- **Play**: Start from current step
- **Shift+Play**: Start from beginning
- **Play again**: Stop (returns to where playback started)

**Recording:**
- **Rec**: Record audio from current step
- **Shift+Rec**: Record from beginning
- **Rec again**: Stop recording
- Auto-stops after a configurable tail period (default 2 bars) to capture reverb/delay tails
- **Tail setting**: Last menu item — jog click cycles through 0, 1, 2, 4, 8 bars
- Files saved to `Recordings/Song Mode/` with set name in filename

**Loop mode:** Press **Loop** to toggle. When on, playback wraps back to the first step after the last step ends. Shows `[L]` in the header. Disabled during recording.

**Editing:**
- **Hold step** or **Jog click** on a step: Edit step settings
- **Repeats**: 1, 2, 4, 8, 16, 32, 64
- **Bar Length**: `Longest` (default), `Shortest`, or `Custom`
- **Custom Bars** appears when `Bar Length = Custom`
- In step settings, jog wheel selects a parameter; jog click toggles edit mode for that parameter
- Tracks shown with a trailing quote (`"`) as continue do not retrigger and do not contribute to step bar-length calculation
- Step rows show duration at right as `4`, or `4x2` when repeats are >1
- **Copy**: Duplicate selected step
- **Delete**: Remove selected step
- **Shift+Delete**: Clear entire song
- **Shift+Up/Down**: Reorder steps
- **Undo**: Restore previous state (up to 20 levels)

**Navigation:**
- **Jog wheel**: Scroll through steps
- **Step buttons**: Select steps directly
- **Left/Right**: Page through steps (16 per page)

Step LEDs: green = has content, red = now playing, white = selected. Pad LEDs highlight the current step's assigned clips in red.

**Persistence:** Song arrangements save automatically per set. Re-entering Song Mode for the same set restores your song.

---

## File Browser (Web)

Move Everything includes a web-based file browser for managing files on your Move from any device on the same network.

### Setup

1. Open **Global Settings > Services** (**Shift+Vol + Step 2**)
2. Toggle **File Browser** to **On**
3. Open `http://move.local:404` in a browser

The file browser serves the `/data/UserData` directory, giving you access to samples, recordings, presets, and other user files. You can upload, download, rename, move, and delete files.

### Notes

- File Browser is **off by default** and must be enabled via the settings toggle
- Starts immediately when enabled (no restart required)
- The setting persists across reboots
- No authentication is required — anyone on the network can access it
- Shell command execution is disabled for safety

---

## Screen Reader

Move Everything includes an optional screen reader for accessibility, using text-to-speech to announce UI elements.

Toggle via **Global Settings > Screen Reader** (**Shift+Vol + Step 2**), or **Shift+Note/Session** when Shadow UI is disabled.

Settings:
- **Speed**: 0.5x to 6.0x
- **Pitch**: 80-180 Hz
- **Volume**: 0-100%
- **Debounce**: 0-1000ms

Can be enabled during installation with `--enable-screen-reader`.

---

## Display Mirror

Stream Move's 128x64 OLED display to any browser on your network in real time. Useful for remote monitoring, screen capture, or development.

### Setup

1. Open **Global Settings > Display** (**Shift+Vol + Step 2**)
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

## In-App Help

Move Everything includes a built-in help viewer accessible from **Global Settings > [Help...]** (**Shift+Vol + Step 2**). It contains a quick reference for shortcuts, slot setup, recording, and other features — readable directly on Move's display.

If the screen reader is enabled, help pages are read aloud automatically when opened.

---

## Mute and Solo

You can mute and solo individual slots using the Mute button (next to the track buttons):

| Shortcut | Action |
|----------|--------|
| **Mute + Track 1-4** | Toggle mute on that slot |
| **Shift + Mute + Track 1-4** | Toggle solo on that slot |

Muted slots are silenced but continue processing MIDI. Solo isolates a single slot.

---

## Tips

- Each Move Set has its own slot configurations — switch Sets to switch between different instrument setups
- Use Set Pages to organize sets by project or performance — Shift+Vol+Left/Right to switch
- If something goes wrong, use Move's DFU restore mode to reset
