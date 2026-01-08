# SF2 Synth Module

Polyphonic SoundFont synthesizer for Move Anything using TinySoundFont.

## Features

- Load any .sf2 SoundFont file
- Preset selection via UI
- Octave transpose (-4 to +4)
- Full polyphony
- Velocity sensitivity
- Pitch bend support

## Setup

After deploying Move Anything to your Move device, copy a SoundFont file to the module directory:

```bash
scp your-soundfont.sf2 ableton@move.local:/data/UserData/move-anything/modules/sf2/instrument.sf2
```

The module loads `instrument.sf2` from its directory by default.

## Controls

- **Jog wheel**: Navigate presets
- **Jog click**: Select preset
- **Knob 1**: Octave transpose
- **Pads/Keys**: Play notes (external MIDI keyboard recommended)

## SoundFont Sources

Free SoundFonts are available from:
- https://musical-artifacts.com/artifacts?formats=sf2
- https://www.philscomputerlab.com/general-midi-soundfonts.html
- FluidSynth's FluidR3_GM.sf2

## Technical Details

- Sample rate: 44100 Hz
- Block size: 128 frames
- Output: Stereo interleaved int16
