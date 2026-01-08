# Signal Chain Module

The Signal Chain module lets you build patchable chains of MIDI FX, a sound generator, and audio FX.

## Architecture

```
[Input] -> [MIDI FX] -> [Sound Generator] -> [Audio FX] -> [Output]
```

Components live under `src/modules/chain/`:
- `midi_fx/`: JavaScript MIDI effects registry and implementations
- `sound_generators/`: Built-in generators (linein)
- `audio_fx/`: Audio effects (freeverb)
- `patches/`: Patch JSON files used by the chain browser

## Patch Format

Example:

```json
{
    "name": "Chord Piano",
    "version": 1,
    "chain": {
        "input": "pads",
        "midi_fx": {
            "chord": "major"
        },
        "synth": {
            "module": "sf2",
            "config": {
                "preset": 0
            }
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.6,
                    "wet": 0.25,
                    "dry": 0.75
                }
            }
        ]
    }
}
```

### Input Routing

`input` controls which MIDI sources the chain accepts:
- `pads`: Move internal pads/controls only
- `external`: external USB MIDI only
- `both`/`all`: allow both sources

### MIDI FX

Native MIDI FX supported today:
- Chord: `major`, `minor`, `power`, `octave`
- Arp: `up`, `down`, `up_down`, `random` with `arp_bpm` and `arp_division`

JavaScript MIDI FX can be attached per patch using `midi_fx_js`:

```json
"midi_fx_js": ["octave_up"]
```

Available JS MIDI FX live in `midi_fx/` (see `index.mjs` for the registry).

### Sound Generators

Sound generators can be built-in or external modules. Built-in: `linein`. External modules that work as chain sound generators include `sf2`, `dx7`, `jv880`, and `obxd` (plus any other module that is `chainable` with `component_type: "sound_generator"`).

### Audio FX

Audio FX are loaded by type. `freeverb` is included.

## Raw MIDI and Knob Touch

By default, the host filters knob-touch notes (0-9) from internal MIDI. To bypass this for Signal Chain, set `"raw_midi": true` in `module.json`.

## External Module Presets

External modules can install chain presets into `patches/` via their install scripts (for example JV-880 and OB-Xd).
