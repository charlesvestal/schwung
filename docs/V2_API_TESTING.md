# Plugin API v2 Testing Guide

This document provides a walkthrough for testing the new v2 instance-based Plugin API for both sound generators and audio effects.

## What Changed in v2

The v2 API moves from global state to instance-based state, enabling:
- Multiple instances of the same plugin running simultaneously
- Independent state per instance (different presets, parameters, etc.)
- Better isolation between tracks in Signal Chain

### Sound Generators: plugin_api_v2
- `create_instance()` / `destroy_instance()` replace `on_load()` / `on_unload()`
- All functions now take an `instance` pointer
- Migrated: linein, sf2, dexed, minijv, obxd, clap

### Audio Effects: audio_fx_api_v2
- Same pattern as sound generators
- Migrated: psxverb, cloudseed, tapescam, tapedelay

## Installation

### 1. Install Main Framework
```bash
cd move-anything
./scripts/install.sh local
```

### 2. Install Audio FX Plugins
```bash
cd ../move-anything-psxverb && ./scripts/install.sh
cd ../move-anything-cloudseed && ./scripts/install.sh
cd ../move-anything-tapescam && ./scripts/install.sh
cd ../move-anything-space-delay && ./scripts/install.sh
```

### 3. Install Sound Generator Plugins
```bash
cd ../move-anything-sf2 && ./scripts/install.sh
cd ../move-anything-dx7 && ./scripts/install.sh
cd ../move-anything-obxd && ./scripts/install.sh
cd ../move-anything-clap && ./scripts/install.sh
cd ../move-anything-jv880 && ./scripts/install.sh  # Requires ROMs
```

### 4. Reboot Move
After installation, reboot the Move device to pick up all changes.

## Test Signal Chains

The following test patches are included to validate v2 API functionality:

| Patch Name | Synth | Audio FX | Purpose |
|------------|-------|----------|---------|
| V2 Test: LineIn + CloudSeed | linein (v2) | cloudseed (v2) | Basic v2 audio FX test |
| V2 Test: LineIn + Tapescam | linein (v2) | tapescam (v2) | Tape saturation v2 test |
| V2 Test: Multi FX Chain | linein (v2) | tapescam + tapedelay + cloudseed | Multiple v2 FX instances |
| V2 Test: SF2 + PSXVerb | sf2 (v2) | psxverb (v2) | v2 synth + v2 FX |
| V2 Test: Dexed + TapeDelay | dexed (v2) | tapedelay (v2) | FM synth + delay |
| V2 Test: Dual Reverb | linein (v2) | psxverb + cloudseed | Two reverbs in series |

## Manual Test Procedure

### Test 1: Basic Audio FX (v2)
1. Navigate to **Signal Chain** module
2. Load patch **"V2 Test: LineIn + CloudSeed"**
3. Connect audio input (guitar, synth, etc.)
4. **Verify:** You should hear reverb effect on input audio
5. **Verify:** Turn knobs 2-8 to adjust CloudSeed parameters (mix, decay, size, etc.)
6. **Verify:** Parameters respond smoothly without clicks/pops

### Test 2: Tape Saturation (v2)
1. Load patch **"V2 Test: LineIn + Tapescam"**
2. Send audio through
3. **Verify:** Tape saturation/wobble effect is audible
4. **Verify:** Increase "wobble" knob (CC 75) to hear flutter effect
5. **Verify:** Increase "drive" knob (CC 73) to hear distortion

### Test 3: Multiple FX Instances (CRITICAL)
This tests the core v2 benefit - multiple independent instances:

1. Load patch **"V2 Test: Multi FX Chain"**
2. This chain has: tapescam → tapedelay → cloudseed
3. **Verify:** All three effects are audible in series
4. **Verify:** Each effect's parameters are independently controllable
5. **Verify:** No parameter crosstalk between effects

### Test 4: v2 Synth + v2 FX
1. Load patch **"V2 Test: SF2 + PSXVerb"**
2. Play notes on the pads
3. **Verify:** SF2 synth produces sound
4. **Verify:** PSX reverb is applied to synth output
5. **Verify:** Changing synth preset (knob 1) doesn't affect reverb settings

### Test 5: Dual Reverb (Instance Isolation)
This is the ultimate v2 test - two reverbs with different settings:

1. Load patch **"V2 Test: Dual Reverb"**
2. Send audio through
3. **Verify:** Both PSXVerb and CloudSeed are audible
4. **Verify:** Knobs 1-3 control PSXVerb parameters only
5. **Verify:** Knobs 4-7 control CloudSeed parameters only
6. **Critical:** Changes to one reverb should NOT affect the other

### Test 6: Recording Test
1. Load any v2 test patch
2. Play/input audio
3. Press **Record** button (CC 118) to start recording
4. Play for 10-20 seconds
5. Press **Record** again to stop
6. **Verify:** WAV file created in `/data/UserData/move-anything/recordings/`
7. **Verify:** Playback sounds correct with all FX applied

## What to Look For (Potential Issues)

### If Something Doesn't Work:
1. **No sound from synth**: Check if dsp.so was installed correctly
2. **No effect on audio**: Check if audio_fx .so files are in correct location
3. **Parameter changes affect wrong effect**: Instance isolation failure - check logs
4. **Crashes on patch load**: Likely missing v2 entry point - check console

### Log Messages to Watch:
```
# Good v2 messages:
[chain] Loaded v2 audio FX plugin: cloudseed
[cloudseed-v2] Creating instance
[cloudseed-v2] Instance created

# Bad messages (falling back to v1):
[chain] v2 init not found, trying v1
[cloudseed] CloudSeed plugin initialized  # v1 fallback
```

## Verifying v2 API is Active

SSH into the Move and check the logs:
```bash
ssh root@move.local
cat /tmp/move-anything.log | grep -E "(v2|Creating instance)"
```

You should see v2 initialization messages for each plugin.

## Quick Reference: Knob Mappings

### V2 Test: LineIn + CloudSeed
- Knob 2 (CC 72): Mix
- Knob 3 (CC 73): Decay
- Knob 4 (CC 74): Size
- Knob 5 (CC 75): Diffusion
- Knob 6 (CC 76): Pre-delay
- Knob 7 (CC 77): High Cut
- Knob 8 (CC 78): Mod Amount

### V2 Test: Multi FX Chain
- Knob 1 (CC 71): Tapescam Drive
- Knob 2 (CC 72): Tapescam Wobble
- Knob 3 (CC 73): TapeDelay Time
- Knob 4 (CC 74): TapeDelay Feedback
- Knob 5 (CC 75): TapeDelay Mix
- Knob 6 (CC 76): CloudSeed Decay
- Knob 7 (CC 77): CloudSeed Mix
- Knob 8 (CC 78): CloudSeed Size

## Troubleshooting

### Effect not loading
Check that the .so file exists:
```bash
ls -la /data/UserData/move-anything/modules/chain/audio_fx/*/
```

### Wrong API version
Check the entry point symbol:
```bash
nm -D /data/UserData/move-anything/modules/chain/audio_fx/cloudseed/cloudseed.so | grep move_audio_fx
# Should show both: move_audio_fx_init_v1 and move_audio_fx_init_v2
```

### Instance not created
Check memory allocation - each instance needs significant heap:
- PSXVerb: ~128KB per instance
- CloudSeed: ~2MB per instance (reverb channels)
- Tapescam: ~16KB per instance (delay buffers)
