# Signal Chain Architecture Design

## Overview

A modular signal chain system for Move Anything that allows combining input sources, MIDI processors, sound generators, and audio effects into configurable patches. This is the foundation for evolving Move Anything into a groovebox with multiple chains and sequencers.

## Architecture

### Fixed Chain Order

```
[Input] → [MIDI FX (JS)] → [MIDI FX (Native)] → [Sound Generator] → [Audio FX] → [Output]
           (optional)        (optional)                              (optional)
```

The chain host runs as a module that orchestrates components:
- Loads patch files describing the chain configuration
- Instantiates each component (input handler, MIDI FX, synth, audio FX)
- Routes MIDI: Input → JS MIDI FX → Native MIDI FX → Sound Generator
- Routes Audio: Sound Generator → Audio FX chain → Output

### Component Types

| Type | Description | Implementation |
|------|-------------|----------------|
| `sound_generator` | Takes MIDI, outputs audio | Native DSP plugin (existing: SF2, DX7) |
| `audio_fx` | Takes audio, outputs audio | Native DSP plugin (new) |
| `midi_fx` | Takes MIDI, outputs MIDI | JS (simple) or Native (timing-critical) |
| `input` | Generates MIDI | Hardcoded for V1 (pads, external MIDI) |

### Module Capabilities

Modules declare chainability in `module.json`:

```json
{
    "id": "sf2",
    "name": "SF2 Synth",
    "capabilities": {
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

The chain host scans modules to discover available components.

## Component Interfaces

### Sound Generator (existing plugin API)

No changes needed - existing `plugin_api_v1_t` works:
- `on_midi()` - receives MIDI
- `render_block()` - outputs audio

### Audio FX Plugin (new)

```c
typedef struct audio_fx_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *config_json);
    void (*on_unload)(void);
    void (*process_block)(int16_t *audio_inout, int frames);  // in-place stereo
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
} audio_fx_api_v1_t;

// Entry point
audio_fx_api_v1_t* move_audio_fx_init_v1(const host_api_v1_t *host);
```

### JS MIDI FX

```javascript
// Called for each MIDI message
// Returns array of messages to output (can be 0, 1, or many)
globalThis.processMidi = function(status, data1, data2) {
    // Example: pass-through
    return [[status, data1, data2]];

    // Example: chord generator (add major third and fifth)
    // return [[status, data1, data2],
    //         [status, data1 + 4, data2],
    //         [status, data1 + 7, data2]];
};

// Optional: called on load with config from patch
globalThis.init = function(config) {};
```

### Native MIDI FX (for arpeggiator)

```c
typedef void (*midi_output_fn)(const uint8_t *msg, int len);

typedef struct midi_fx_api_v1 {
    uint32_t api_version;
    int (*on_load)(const char *module_dir, const char *config_json);
    void (*on_unload)(void);
    void (*process_midi)(const uint8_t *msg, int len, midi_output_fn output);
    void (*tick)(uint32_t sample_time);  // called each audio block for timing
    void (*set_param)(const char *key, const char *val);
    int (*get_param)(const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;
```

## Patch Files

Patches stored in `/data/UserData/move-anything/patches/` as JSON:

```json
{
    "name": "Arp Piano Verb",
    "version": 1,
    "chain": {
        "input": "pads",
        "midi_fx_js": ["chord_generator"],
        "midi_fx_native": [],
        "synth": {
            "module": "sf2",
            "config": {
                "soundfont": "piano.sf2",
                "preset": 0
            }
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.8,
                    "wet": 0.3,
                    "dry": 0.7
                }
            }
        ]
    }
}
```

## File Structure

```
modules/chain/
    module.json
    ui.js                    # Patch browser and status UI
    dsp/
        chain_host.c         # Chain orchestration
    midi_fx/
        chord_generator.js
        scale_quantizer.js
    audio_fx/
        freeverb/
            freeverb.c
            freeverb.h
patches/
    arp_piano_verb.json
    dx7_strings_delay.json
```

## UI Design

**Patch Browser (default):**
```
┌────────────────────────┐
│ Signal Chain           │
├────────────────────────┤
│ > Arp Piano Verb       │
│   DX7 Strings Delay    │
│   Pad Chords           │
│                        │
│ Jog:Select  Click:Load │
└────────────────────────┘
```

**Playing View:**
```
┌────────────────────────┐
│ Arp Piano Verb         │
├────────────────────────┤
│ Synth: SF2 Piano       │
│ FX: Freeverb  Wet:30%  │
│ Voices: 4              │
│                        │
│ L/R:Params  Back:Menu  │
└────────────────────────┘
```

## Implementation Phases

### Phase 1: Chain Host Foundation ✅ COMPLETE
- Create `modules/chain/` module structure
- Chain host DSP that loads one sound_generator sub-plugin
- Pass MIDI through, route audio out
- Hardcoded single patch (no browser)
- **Test:** Load SF2 as sub-plugin, play pads → sound

### Phase 2: Patch Files + Browser ✅ COMPLETE
- JSON patch file parsing
- Patch browser UI with jog wheel navigation
- Sample patches: piano, dx7_brass, strings, organ
- Dynamic synth module loading (SF2 or DX7)
- **Test:** Multiple patches, switch between them with jog wheel

### Phase 3: Audio FX ✅ COMPLETE
- Implement `audio_fx_api_v1` interface
- Port Freeverb (Schroeder-Moorer algorithm)
- Chain host routes synth → FX → output
- Patch JSON parsing for audio_fx array
- **Test:** Piano + Reverb patch working

### Phase 4: Native MIDI FX (Chord Generator) ✅ COMPLETE
- Chord generator integrated directly into chain host
- Supports major, minor, power, and octave chord types
- Patch JSON parsing for midi_fx.chord field
- Chord notes generated for note on/off events
- **Test:** Chord Piano, Minor Strings, Power Organ patches working

### Phase 5: Native MIDI FX (Arpeggiator)
- Native MIDI processor interface with tick()
- Implement basic arpeggiator
- **Test:** Pads → Arp → SF2 → Reverb (full chain)

## Future Considerations

- **Input components:** Sequencers, MIDI file players, random generators
- **Multiple chains:** Parallel signal chains for layering/splits
- **Live editing:** Connect/disconnect components on screen
- **Parameter automation:** Per-step parameter changes
- **MIDI routing:** Multiple MIDI channels, splits, layers

## Design Decisions

1. **Fixed chain order** over flexible routing - simpler UI and implementation
2. **Patch files** over hardcoded modules - easier to create/share presets
3. **Hybrid MIDI FX** - JS for simple transforms, native for timing-critical
4. **Embedded FX libraries** - use Freeverb etc. rather than building from scratch
5. **Capability flags** - modules opt-in to being chainable via module.json
