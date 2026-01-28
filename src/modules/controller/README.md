# MIDI Controller Module

Turn your Ableton Move into a customizable MIDI controller with 16 banks.

## Features

- 16 independent banks of pad configurations
- Velocity-sensitive pads with octave transpose (+/- 4 octaves)
- C notes highlighted on pads (dynamic with octave shift)
- Works with external USB MIDI devices
- Knobs send absolute CC values (0-127)
- Runs as an overtake module in shadow mode

## Controls

| Control | Action |
|---------|--------|
| **Pads** | Send MIDI notes (velocity sensitive) |
| **Step buttons** | Switch between banks (1-16) |
| **Knobs 1-8** | Send CC 1-8 (absolute, starts at center) |
| **Master knob** | Send CC 9 |
| **Jog wheel** | Transpose octave (+/- 4) |
| **Up/Down** | Transpose octave (+/- 1) |

## Usage

### From Shadow Mode (Recommended)
1. Enter shadow mode: **Shift + Vol + Knob 1**
2. Navigate to **Overtake Modules**
3. Select **MIDI Controller**
4. Exit anytime: **Shift + Vol + Jog Click**

### From Standalone Move Anything
1. Launch Move Anything: **Shift + Vol + Knob 8**
2. Select **MIDI Controller** from the menu
3. Exit: **Shift + Jog Click**

## Banks

16 banks are available, each with its own pad configuration. Press any step button (1-16) to switch banks. The active bank LED is lit white.

## Octave Transpose

Use the jog wheel or Up/Down buttons to transpose +/- 4 octaves. C notes are highlighted brighter on the pads, and their positions update dynamically with the octave shift.

## Knobs

Knobs 1-8 send CC 1-8 with absolute values (0-127). They start at center (64) and accumulate changes. The master knob sends CC 9.

## External MIDI

Connect keyboards, synths, or other MIDI devices to the Move's USB-A port. The controller sends MIDI to all connected devices on cable 2.

## Tips

- Use different banks for different instruments in your setup
- Bank 10 is traditionally used for drums (General MIDI convention)
- Combine with a USB MIDI hub to control multiple devices
- C notes are brighter - use octave shift to align with your scale
