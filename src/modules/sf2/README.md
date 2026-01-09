# SF2 Synth Module

Polyphonic SoundFont synthesizer for Move Anything using [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard Schelling.

## Features

- Load any .sf2 SoundFont file
- Preset selection via UI
- Octave transpose (-4 to +4)
- Full polyphony
- Velocity sensitivity
- Pitch bend support

## Setup

After deploying Move Anything to your Move device, copy SoundFonts into the module directory:

```bash
scp your-soundfont.sf2 ableton@move.local:/data/UserData/move-anything/modules/sf2/soundfonts/
```

The module loads the first `.sf2` file in `soundfonts/` by default. If the folder is empty, it falls back to `instrument.sf2` in the module root.

## Controls

- **Jog wheel**: Navigate presets
- **Left/Right**: Previous/next preset
- **Shift + Left/Right**: Switch soundfonts
- **Up/Down**: Octave transpose

Switching soundfonts resets to preset 1 in the new file.

## SoundFont Sources

Free SoundFonts are available from:
- https://musical-artifacts.com/artifacts?formats=sf2
- https://www.philscomputerlab.com/general-midi-soundfonts.html
- FluidSynth's FluidR3_GM.sf2

## Technical Details

- Sample rate: 44100 Hz
- Block size: 128 frames
- Output: Stereo interleaved int16

## Credits

- [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard Schelling (MIT license)
