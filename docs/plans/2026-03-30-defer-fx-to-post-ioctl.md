# Defer Per-Slot FX to Post-ioctl Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move per-slot audio FX processing (`shadow_chain_process_fx`) from pre-ioctl to post-ioctl, reducing pre-ioctl `mix_buf` from ~435µs avg / 3ms max to ~5µs (just memcpy+mix).

**Architecture:** Same double-buffer pattern used for synth render. In post-ioctl (`render_to_buffer`), after rendering synth, also run FX and store output in `shadow_slot_fx_deferred[s]`. In pre-ioctl (`mix_from_buffer`), just mix the deferred FX output instead of calling `shadow_chain_process_fx`. The Link Audio path keeps FX in pre-ioctl (needs to combine Link Audio data before FX for sample-accurate inject). Master FX and overtake FX stay in pre-ioctl (they need the combined mailbox).

**Tech Stack:** C (schwung_shim.c)

---

## Background

### Current Architecture

```
POST-IOCTL (render_to_buffer):
  For each slot: render_block(synth only) → shadow_slot_deferred[s]

PRE-IOCTL (mix_from_buffer):
  For each slot:
    copy shadow_slot_deferred[s] → fx_buf
    shadow_chain_process_fx(fx_buf)     ← EXPENSIVE: 435µs avg, 3ms max
    mix fx_buf → mailbox
  Run master FX on mailbox
  Apply volume, capture, etc.
```

### Target Architecture

```
POST-IOCTL (render_to_buffer):
  For each slot:
    render_block(synth only) → shadow_slot_deferred[s]
    shadow_chain_process_fx(deferred[s]) → shadow_slot_fx_deferred[s]   ← MOVED HERE

PRE-IOCTL (mix_from_buffer):
  For each slot:
    mix shadow_slot_fx_deferred[s] → mailbox    ← FAST: just add
  Run master FX on mailbox
  Apply volume, capture, etc.
```

### Idle Gate Behavior

Two idle gates exist:
1. **Synth idle** (`shadow_slot_idle[s]`): synth output is silence → skip render_block
2. **FX idle** (`shadow_slot_fx_idle[s]`): FX output is silence → skip FX + mix

When synth goes idle but FX is not idle, FX still runs on zeros (reverb/delay tail decay). When both are idle, everything is skipped.

In the new design, the idle gate for FX moves to post-ioctl alongside the FX call.

### Link Audio Exception

When Link Audio is active (`rebuild_from_la`), per-track Move audio is combined with synth output BEFORE running FX. This requires FX to run in pre-ioctl where the Link Audio data is available. The Link Audio path is unchanged by this plan.

### What Stays in Pre-ioctl

- Master FX chain (4 slots) — needs combined mailbox
- Overtake DSP FX — needs combined mailbox
- Link Audio FX path — needs Link Audio data
- All volume, capture, skipback, publisher writes

---

## Task 1: Defer per-slot FX to post-ioctl

**Files:**
- Modify: `src/schwung_shim.c`

### Step 1: Add deferred FX buffers

Near line 216 (after `shadow_slot_deferred`), add:

```c
/* Deferred FX output: FX runs in post-ioctl, result mixed in pre-ioctl */
static int16_t shadow_slot_fx_deferred[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];
static int shadow_slot_fx_deferred_valid[SHADOW_CHAIN_INSTANCES];
```

### Step 2: Modify render_to_buffer to run FX after synth

In `shadow_inprocess_render_to_buffer`, the `same_frame_fx` block (starting around line 1283) currently renders synth only. After the synth render and idle check, add FX processing.

Replace the section from line 1283 to line 1342 (the `if (same_frame_fx)` block through the silence check) with:

```c
            if (same_frame_fx) {
                /* New path: synth only → per-slot buffer */
                shadow_chain_set_external_fx_mode(shadow_chain_slots[s].instance, 1);
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               shadow_slot_deferred[s],
                                               MOVE_FRAMES_PER_BLOCK);
                shadow_slot_deferred_valid[s] = 1;
            } else {
                /* Fallback: full render (synth + FX) → accumulated buffer */
                int16_t render_buffer[FRAMES_PER_BLOCK * 2];
                memset(render_buffer, 0, sizeof(render_buffer));
                shadow_plugin_v2->render_block(shadow_chain_slots[s].instance,
                                               render_buffer, MOVE_FRAMES_PER_BLOCK);
                if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS) {
                    float cap_vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++)
                        shadow_slot_capture[s][i] = (int16_t)lroundf((float)render_buffer[i] * cap_vol);
                    if (shadow_pub_audio_shm) {
                        link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                        uint32_t wp = ps->write_pos;
                        for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                            ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] = shadow_slot_capture[s][i];
                            wp++;
                        }
                        __sync_synchronize();
                        ps->write_pos = wp;
                        ps->active = 1;
                    }
                }
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    float vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)(render_buffer[i] * vol);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    shadow_deferred_dsp_buffer[i] = (int16_t)mixed;
                    if (i & 1) shadow_fade_advance(s);
                }
            }

            /* Check if synth render output is silent */
            int16_t *slot_out = same_frame_fx ? shadow_slot_deferred[s] : shadow_deferred_dsp_buffer;
            int is_silent = 1;
            for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                if (slot_out[i] > DSP_SILENCE_LEVEL || slot_out[i] < -DSP_SILENCE_LEVEL) {
                    is_silent = 0;
                    break;
                }
            }

            if (is_silent) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] >= DSP_IDLE_THRESHOLD) {
                    shadow_slot_idle[s] = 1;
                }
            } else {
                shadow_slot_silence_frames[s] = 0;
                shadow_slot_idle[s] = 0;
            }

            /* Run FX in post-ioctl (deferred) when same_frame_fx is active.
             * This moves ~435µs avg / 3ms max out of the pre-ioctl budget.
             * When synth is idle, FX still runs on zeros for tail decay.
             * When both synth AND FX are idle, skip entirely. */
            if (same_frame_fx && shadow_chain_process_fx) {
                if (shadow_slot_fx_idle[s] && shadow_slot_idle[s]) {
                    /* Both idle — FX output is silence, mark valid with zeros */
                    memset(shadow_slot_fx_deferred[s], 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
                    shadow_slot_fx_deferred_valid[s] = 1;
                } else {
                    int16_t fx_buf[FRAMES_PER_BLOCK * 2];
                    memcpy(fx_buf, shadow_slot_deferred[s], sizeof(fx_buf));
                    shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                            fx_buf, MOVE_FRAMES_PER_BLOCK);
                    memcpy(shadow_slot_fx_deferred[s], fx_buf, sizeof(fx_buf));
                    shadow_slot_fx_deferred_valid[s] = 1;

                    /* Track FX output silence for phase 2 idle */
                    int fx_silent = 1;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                        if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                            fx_silent = 0;
                            break;
                        }
                    }
                    if (fx_silent) {
                        shadow_slot_fx_silence_frames[s]++;
                        if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD)
                            shadow_slot_fx_idle[s] = 1;
                    } else {
                        shadow_slot_fx_silence_frames[s] = 0;
                        shadow_slot_fx_idle[s] = 0;
                    }
                }
            }
```

Also update the idle gate at the top of the loop (around line 1273). Currently it `continue`s, skipping both synth AND FX. We need it to only skip when the phase-2 FX idle gate handles it above:

Replace the idle block (lines 1273-1278):
```c
            if (shadow_slot_idle[s]) {
                shadow_slot_silence_frames[s]++;
                if (shadow_slot_silence_frames[s] % 172 != 0) {
                    /* Not a probe frame — skip render, FX in mix_from_buffer runs on zeros */
                    shadow_slot_deferred_valid[s] = 1;
                    continue;
                }
                /* Probe frame: fall through to render and check output */
            }
```

With:
```c
            if (shadow_slot_idle[s]) {
                shadow_slot_silence_frames[s]++;
                int is_probe = (shadow_slot_silence_frames[s] % 172 == 0);
                if (!is_probe) {
                    /* Not a probe frame — skip synth render.
                     * Buffer is already zeros; FX below still runs for tail decay. */
                    shadow_slot_deferred_valid[s] = 1;
                    goto slot_fx_processing;
                }
                /* Probe frame: fall through to render and check output */
            }
```

And add the `slot_fx_processing:` label just before the FX block:

```c
        slot_fx_processing:
            /* Run FX in post-ioctl ... */
            if (same_frame_fx && shadow_chain_process_fx) {
```

### Step 3: Modify mix_from_buffer non-Link Audio path to use deferred FX

Replace the `} else if (shadow_chain_process_fx) {` block (lines 1578-1633) — the non-Link Audio FX path — with:

```c
    } else if (shadow_chain_process_fx) {
        /* Non-Link Audio path: use deferred FX output from post-ioctl */
        for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
            if (!shadow_chain_slots[s].instance) continue;

            /* Use deferred FX output if available (FX ran in post-ioctl) */
            if (shadow_slot_fx_deferred_valid[s]) {
                /* Phase 2 idle: skip if both synth AND FX are idle */
                if (shadow_slot_fx_idle[s] && shadow_slot_idle[s]) continue;

                int16_t *fx_buf = shadow_slot_fx_deferred[s];

                /* Write to publisher shared memory for link_subscriber */
                if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS && shadow_pub_audio_shm) {
                    float cap_vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                    uint32_t wp = ps->write_pos;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                        ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] =
                            (int16_t)lroundf((float)fx_buf[i] * cap_vol);
                        wp++;
                    }
                    __sync_synchronize();
                    ps->write_pos = wp;
                }

                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    float vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    float gain = vol;
                    int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                    me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
                    if (i & 1) shadow_fade_advance(s);
                }
            } else if (shadow_slot_deferred_valid[s]) {
                /* Fallback: FX not deferred (shouldn't happen in same_frame_fx mode,
                 * but handle gracefully by running FX inline as before) */
                int16_t fx_buf[FRAMES_PER_BLOCK * 2];
                memcpy(fx_buf, shadow_slot_deferred[s], sizeof(fx_buf));
                shadow_chain_process_fx(shadow_chain_slots[s].instance,
                                        fx_buf, MOVE_FRAMES_PER_BLOCK);

                if (link_audio.enabled && s < LINK_AUDIO_SHADOW_CHANNELS && shadow_pub_audio_shm) {
                    float cap_vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    link_audio_pub_slot_t *ps = &shadow_pub_audio_shm->slots[s];
                    uint32_t wp = ps->write_pos;
                    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                        ps->ring[wp & LINK_AUDIO_PUB_SHM_RING_MASK] =
                            (int16_t)lroundf((float)fx_buf[i] * cap_vol);
                        wp++;
                    }
                    __sync_synchronize();
                    ps->write_pos = wp;
                }

                int fx_silent = 1;
                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    if (fx_buf[i] > DSP_SILENCE_LEVEL || fx_buf[i] < -DSP_SILENCE_LEVEL) {
                        fx_silent = 0;
                        break;
                    }
                }
                if (fx_silent) {
                    shadow_slot_fx_silence_frames[s]++;
                    if (shadow_slot_fx_silence_frames[s] >= DSP_IDLE_THRESHOLD)
                        shadow_slot_fx_idle[s] = 1;
                } else {
                    shadow_slot_fx_silence_frames[s] = 0;
                    shadow_slot_fx_idle[s] = 0;
                }

                for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
                    float vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
                    float gain = vol;
                    int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;
                    mailbox_audio[i] = (int16_t)mixed;
                    me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);
                    if (i & 1) shadow_fade_advance(s);
                }
            }
        }
    }
```

### Step 4: Clear FX deferred buffers at the start of render_to_buffer

Around line 1247 (where per-slot deferred buffers are cleared), add:

```c
    /* Clear per-slot deferred FX buffers */
    for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
        memset(shadow_slot_fx_deferred[s], 0, FRAMES_PER_BLOCK * 2 * sizeof(int16_t));
        shadow_slot_fx_deferred_valid[s] = 0;
    }
```

### Step 5: Update comment on mix_from_buffer call site

In the pre-ioctl section (around line 3461), update the comment:

```c
        shadow_inprocess_mix_from_buffer();  /* Fast: memcpy+mix (FX deferred to post-ioctl) */
```

### Step 6: Build and test

```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

Verify on hardware:
- `mix_buf` avg should drop from ~435µs to ~5-20µs
- `mix_buf` max should drop from ~3000µs to ~50µs
- Audio quality unchanged (reverb tails decay correctly)
- Idle gate still works (silence → no CPU usage)

### Step 7: Commit

```bash
git add src/schwung_shim.c
git commit -m "fix: defer per-slot FX processing to post-ioctl

Move shadow_chain_process_fx calls from pre-ioctl (mix_from_buffer)
to post-ioctl (render_to_buffer). Pre-ioctl now just mixes the
deferred FX output. Reduces mix_buf from ~435µs/3ms to ~5µs/50µs.
Link Audio path keeps FX in pre-ioctl for sample-accurate inject."
```

---

## Summary

| What | Before | After |
|------|--------|-------|
| mix_buf avg | ~435µs | ~5-20µs |
| mix_buf max | ~3000µs | ~50µs |
| FX latency | +1 frame (same as synth) | +1 frame (unchanged) |
| Link Audio FX | pre-ioctl | pre-ioctl (unchanged) |
| Master FX | pre-ioctl | pre-ioctl (unchanged) |
| Post-ioctl time | ~1100µs | ~1500µs (FX moved here) |

Post-ioctl has no time budget — it runs between SPI transfers. The extra ~400µs there is free.
