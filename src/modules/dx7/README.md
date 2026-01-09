# DX7 Synth Module

6-operator FM synthesizer based on the Yamaha DX7, using the msfa engine from Dexed.

## Features

- Full 6-operator FM synthesis
- All 32 DX7 algorithms
- Loads standard .syx patch banks (32 voices per bank)
- 16-voice polyphony with voice stealing
- Velocity sensitivity
- Aftertouch modulation (polyphonic and channel)
- Mod wheel, breath controller, foot controller support
- Sustain pedal (CC 64)
- Pitch bend support
- Octave transpose (-4 to +4)
- Adjustable output level for polyphony headroom

## Setup

### Loading Patches

Copy a DX7 .syx patch bank to the module directory:

```bash
scp your-patches.syx ableton@move.local:/data/UserData/move-anything/modules/dx7/patches.syx
```

The module loads `patches.syx` from its directory on startup.

### Patch File Format

The module expects standard DX7 32-voice bank sysex files:
- 4104 bytes total (with sysex headers)
- 4096 bytes of patch data (32 patches x 128 bytes each)
- Standard VMEM packed format

Single-voice VCED dumps are not supported.

## Controls

### Move Hardware
- **Jog wheel**: Navigate presets (1-32)
- **Left/Right**: Previous/next preset
- **Up/Down**: Octave transpose
- **Pads**: Play notes (velocity sensitive, with aftertouch)

### External MIDI
- **Note On/Off**: Play notes
- **Velocity**: Controls operator output levels
- **Pitch Bend**: +/- 2 semitones
- **Mod Wheel (CC 1)**: LFO pitch/amplitude modulation
- **Aftertouch**: Pitch/amplitude modulation
- **Breath Controller (CC 2)**: Amplitude modulation
- **Foot Controller (CC 4)**: Available for modulation
- **Sustain Pedal (CC 64)**: Hold notes

## Parameters

Set via `host_module_set_param(key, value)`:

| Parameter | Values | Description |
|-----------|--------|-------------|
| `syx_path` | file path | Load a .syx patch bank |
| `preset` | 0-31 | Select preset from loaded bank |
| `octave_transpose` | -4 to 4 | Transpose by octaves |
| `output_level` | 0-100 | Output volume (default 50) |

Get via `host_module_get_param(key)`:

| Parameter | Description |
|-----------|-------------|
| `patch_name` | Current patch name |
| `preset` | Current preset number |
| `preset_count` | Number of loaded presets |
| `algorithm` | Current algorithm (1-32) |
| `polyphony` | Active voice count |
| `output_level` | Current output level |

## Finding DX7 Patches

Thousands of free DX7 .syx patches are available:

- https://homepages.abdn.ac.uk/d.j.benson/pages/html/dx7.html
- https://yamahablackboxes.com/collection/yamaha-dx7-dx9-dx7ii-patches/
- https://www.polynominal.com/yamaha-dx7/

Classic banks include:
- ROM1A/ROM1B - Original factory presets
- E! series - Famous aftermarket patches
- Various artist signature patches

## Troubleshooting

**No sound**: Ensure a .syx file is loaded. The default "Init" patch is very basic.

**Clipping with chords**: Lower the `output_level` (default 50 provides headroom for 4+ voices).

**Wrong pitch**: Ensure you're using standard DX7 .syx files.

## Technical Details

- Sample rate: 44100 Hz
- Block size: 128 frames (64-sample internal blocks)
- Output: Stereo (mono duplicated)
- Polyphony: 16 voices
- Engine: msfa from Dexed (Apache 2.0 license)

## Credits

- msfa FM engine: Google (Apache 2.0)
- Dexed integration: Pascal Gauthier
- Portamento: Jean Pierre Cimalando

See THIRD_PARTY_LICENSES in the repository root for full license details.
