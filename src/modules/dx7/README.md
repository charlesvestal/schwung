# DX7 Synth Module

6-operator FM synthesizer based on the Yamaha DX7, using the msfa engine from Dexed.

## Features

- Full 6-operator FM synthesis
- All 32 DX7 algorithms
- Loads standard .syx patch banks (32 voices per bank)
- 16-voice polyphony
- Velocity sensitivity
- Pitch bend support
- Octave transpose (-4 to +4)

## Setup

After deploying Move Anything to your Move device, copy a DX7 .syx patch bank:

```bash
scp your-patches.syx ableton@move.local:/data/UserData/move-anything/modules/dx7/patches.syx
```

The module loads `patches.syx` from its directory by default.

## Controls

- **Jog wheel**: Navigate presets (1-32)
- **Left/Right**: Previous/next preset
- **+/-**: Octave transpose
- **Pads/Keys**: Play notes (external MIDI keyboard recommended)

## Finding DX7 Patches

Thousands of free DX7 .syx patches are available:

- https://homepages.abdn.ac.uk/d.j.benson/pages/html/dx7.html
- https://yamahablackboxes.com/collection/yamaha-dx7-dx9-dx7ii-patches/
- https://www.polynominal.com/yamaha-dx7/

Classic banks include:
- ROM1A/ROM1B - Original factory presets
- E! series - Famous aftermarket patches
- Various artist signature patches

## Technical Details

- Sample rate: 44100 Hz
- Block size: 128 frames (processes in 64-sample blocks internally)
- Output: Stereo (mono duplicated)
- Polyphony: 16 voices
- Engine: msfa from Dexed (Apache 2.0 license)

## Credits

- msfa FM engine: Google (Apache 2.0)
- Dexed integration: Pascal Gauthier
