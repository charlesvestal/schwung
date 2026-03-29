# Seamless Set Switching Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate audible clicks and pops when switching patches/sets by adding smooth fade-out/fade-in envelopes at the slot level.

**Architecture:** Add a `slot_fade_t` struct to each shadow chain slot. Instead of hard-muting via `mute_countdown` in chain_host.c, the shim applies a per-sample gain ramp when slots load/unload. Old patches keep rendering during fade-out; all blocking I/O happens while the slot is silent; new patches fade in cleanly.

**Tech Stack:** C, shared memory IPC (existing shadow infrastructure)

---

### Task 1: Add fade state to shadow_chain_slot_t

**Files:**
- Modify: `src/host/shadow_chain_types.h:15-26`

**Step 1: Add fade struct and fields to slot type**

In `src/host/shadow_chain_types.h`, add a fade state struct and embed it in `shadow_chain_slot_t`:

```c
/* Fade envelope for seamless patch transitions */
typedef struct slot_fade_t {
    float gain;            /* current gain 0.0-1.0 */
    float target;          /* target gain: 0.0 (fading out) or 1.0 (fading in) */
    float step;            /* per-sample gain change (1.0/FADE_SAMPLES) */
    int pending_patch;     /* patch index to load after fade-out (-1 = none) */
    uint8_t pending_clear; /* tear down DSP after fade-out completes */
} slot_fade_t;

/* 50ms fade at 44100Hz */
#define SLOT_FADE_SAMPLES 2205
#define SLOT_FADE_STEP (1.0f / SLOT_FADE_SAMPLES)
```

Add `slot_fade_t fade;` as a field in `shadow_chain_slot_t`, after the existing `shadow_capture_rules_t capture` field.

**Step 2: Commit**

```bash
git add src/host/shadow_chain_types.h
git commit -m "feat: add slot_fade_t struct for seamless patch transitions"
```

---

### Task 2: Initialize fade state for all slots

**Files:**
- Modify: `src/host/shadow_chain_mgmt.c:355-367` (slot initialization loop)

**Step 1: Initialize fade fields in slot init**

Find the slot initialization loop in `shadow_chain_mgmt.c` (around line 355-367, the function that sets `instance = NULL`, `active = 0`, etc.) and add fade initialization for each slot:

```c
shadow_chain_slots[s].fade.gain = 0.0f;
shadow_chain_slots[s].fade.target = 0.0f;
shadow_chain_slots[s].fade.step = SLOT_FADE_STEP;
shadow_chain_slots[s].fade.pending_patch = -1;
shadow_chain_slots[s].fade.pending_clear = 0;
```

**Step 2: Commit**

```bash
git add src/host/shadow_chain_mgmt.c
git commit -m "feat: initialize slot fade state on startup"
```

---

### Task 3: Defer patch load/clear requests to fade-out completion

**Files:**
- Modify: `src/host/shadow_chain_mgmt.c` — function `shadow_inprocess_handle_ui_request()` (lines 1333-1437)

**Step 1: Change load request to start fade-out instead of immediate load**

In `shadow_inprocess_handle_ui_request()`, instead of immediately calling `set_param("load_patch", ...)`, set up the fade-out and store the pending patch index:

For the **clear slot** case (when `patch_index == SHADOW_PATCH_INDEX_NONE`, lines 1358-1376):
- If `fade.gain > 0.0f`, set `fade.target = 0.0f` and `fade.pending_clear = 1`, then return (defer actual teardown)
- If `fade.gain == 0.0f` (slot already silent), do the immediate teardown as before

For the **load patch** case (lines 1391-1437):
- If `fade.gain > 0.0f` (slot is currently audible with a patch), set `fade.target = 0.0f` and `fade.pending_patch = patch_index`, then return (defer load)
- If `fade.gain == 0.0f` (slot is silent — empty slot or already faded out), do the immediate load as before, then set `fade.target = 1.0f` to fade in

**Step 2: Commit**

```bash
git add src/host/shadow_chain_mgmt.c
git commit -m "feat: defer patch load/clear until fade-out completes"
```

---

### Task 4: Add fade-out completion handler

**Files:**
- Modify: `src/host/shadow_chain_mgmt.c`

**Step 1: Add a function to process pending fade actions**

Create a new function `shadow_process_fade_completions()` that is called each render cycle (from the same place `shadow_inprocess_handle_ui_request()` is called). For each slot:

```c
void shadow_process_fade_completions(void) {
    shadow_control_t *ctrl = host.shadow_control_ptr ? *host.shadow_control_ptr : NULL;

    for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
        slot_fade_t *fade = &shadow_chain_slots[s].fade;

        /* Only act when gain has fully reached zero */
        if (fade->gain > 0.0f) continue;

        if (fade->pending_clear) {
            /* Do the actual slot teardown (same code as the old immediate path) */
            if (shadow_plugin_v2 && shadow_plugin_v2->set_param && shadow_chain_slots[s].instance) {
                shadow_plugin_v2->set_param(shadow_chain_slots[s].instance, "synth:module", "");
                shadow_plugin_v2->set_param(shadow_chain_slots[s].instance, "fx1:module", "");
                shadow_plugin_v2->set_param(shadow_chain_slots[s].instance, "fx2:module", "");
            }
            shadow_chain_slots[s].active = 0;
            shadow_chain_slots[s].patch_index = -1;
            capture_clear(&shadow_chain_slots[s].capture);
            strncpy(shadow_chain_slots[s].patch_name, "", sizeof(shadow_chain_slots[s].patch_name) - 1);
            shadow_chain_slots[s].patch_name[sizeof(shadow_chain_slots[s].patch_name) - 1] = '\0';
            shadow_ui_state_t *ui_state = host.shadow_ui_state_ptr ? *host.shadow_ui_state_ptr : NULL;
            if (ui_state && s < SHADOW_UI_SLOTS) {
                strncpy(ui_state->slot_names[s], "", SHADOW_UI_NAME_LEN - 1);
                ui_state->slot_names[s][SHADOW_UI_NAME_LEN - 1] = '\0';
            }
            fade->pending_clear = 0;
            /* gain stays at 0, target stays at 0 */
        }

        if (fade->pending_patch >= 0) {
            /* Do the actual patch load (same code as old immediate path) */
            int patch_index = fade->pending_patch;
            fade->pending_patch = -1;

            /* Validate patch count */
            if (shadow_plugin_v2 && shadow_plugin_v2->get_param) {
                char buf[32];
                int len = shadow_plugin_v2->get_param(shadow_chain_slots[s].instance,
                                                      "patch_count", buf, sizeof(buf));
                if (len > 0) {
                    buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
                    int patch_count = atoi(buf);
                    if (patch_count > 0 && patch_index >= patch_count) continue;
                }
            }

            char idx_str[16];
            snprintf(idx_str, sizeof(idx_str), "%d", patch_index);
            shadow_plugin_v2->set_param(shadow_chain_slots[s].instance, "load_patch", idx_str);
            shadow_chain_slots[s].patch_index = patch_index;
            shadow_chain_slots[s].active = 1;

            /* Read back patch name */
            if (shadow_plugin_v2->get_param) {
                char key[32], buf[128];
                snprintf(key, sizeof(key), "patch_name_%d", patch_index);
                int len = shadow_plugin_v2->get_param(shadow_chain_slots[s].instance, key, buf, sizeof(buf));
                if (len > 0) {
                    buf[len < (int)sizeof(buf) ? len : (int)sizeof(buf) - 1] = '\0';
                    strncpy(shadow_chain_slots[s].patch_name, buf, sizeof(shadow_chain_slots[s].patch_name) - 1);
                    shadow_chain_slots[s].patch_name[sizeof(shadow_chain_slots[s].patch_name) - 1] = '\0';
                }
            }

            shadow_slot_load_capture(s, patch_index);

            /* Apply channel settings from patch */
            if (shadow_plugin_v2->get_param) {
                char ch_buf[16];
                int len;
                len = shadow_plugin_v2->get_param(shadow_chain_slots[s].instance,
                    "patch:receive_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int recv_ch = atoi(ch_buf);
                    if (recv_ch != 0) {
                        shadow_chain_slots[s].channel = (recv_ch >= 1 && recv_ch <= 16) ? recv_ch - 1 : -1;
                    }
                }
                len = shadow_plugin_v2->get_param(shadow_chain_slots[s].instance,
                    "patch:forward_channel", ch_buf, sizeof(ch_buf));
                if (len > 0) {
                    ch_buf[len < (int)sizeof(ch_buf) ? len : (int)sizeof(ch_buf) - 1] = '\0';
                    int fwd_ch = atoi(ch_buf);
                    if (fwd_ch != 0) {
                        shadow_chain_slots[s].forward_channel = (fwd_ch > 0) ? fwd_ch - 1 : fwd_ch;
                    }
                }
            }

            shadow_ui_state_update_slot(s);

            /* Begin fade-in */
            fade->target = 1.0f;
        }
    }
}
```

**Step 2: Declare in header**

Add `void shadow_process_fade_completions(void);` to `src/host/shadow_chain_mgmt.h`.

**Step 3: Commit**

```bash
git add src/host/shadow_chain_mgmt.c src/host/shadow_chain_mgmt.h
git commit -m "feat: process pending patch load/clear after fade-out"
```

---

### Task 5: Call fade completion handler from shim render loop

**Files:**
- Modify: `src/schwung_shim.c`

**Step 1: Add call to shadow_process_fade_completions()**

Find where `shadow_inprocess_handle_ui_request()` is called in the shim's render cycle and add a call to `shadow_process_fade_completions()` right after it. This ensures pending loads/clears are processed each audio frame after fades complete.

Search for `shadow_inprocess_handle_ui_request` in `schwung_shim.c` and add:
```c
shadow_process_fade_completions();
```

**Step 2: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: call fade completion handler each render cycle"
```

---

### Task 6: Apply fade gain ramp in the audio mix path

**Files:**
- Modify: `src/schwung_shim.c` — all locations where `shadow_effective_volume(s)` is used to mix slot audio

**Step 1: Create a helper that combines effective volume with fade gain**

In `src/host/shadow_chain_mgmt.h`, add an inline helper next to the existing `shadow_effective_volume`:

```c
/* Effective volume including fade envelope for seamless transitions.
 * Advances fade gain by one block (frames samples). */
static inline float shadow_effective_volume_with_fade(int slot, int sample_index) {
    return shadow_effective_volume(slot) * shadow_chain_slots[slot].fade.gain;
}

/* Advance the fade envelope by one sample. Call once per sample in mix loop. */
static inline void shadow_fade_advance(int slot) {
    slot_fade_t *f = &shadow_chain_slots[slot].fade;
    if (f->gain < f->target) {
        f->gain += f->step;
        if (f->gain > f->target) f->gain = f->target;
    } else if (f->gain > f->target) {
        f->gain -= f->step;
        if (f->gain < f->target) f->gain = f->target;
    }
}
```

**Step 2: Apply per-sample fade in mix loops**

There are multiple mix paths in `schwung_shim.c` (pre-render, deferred, Link Audio rebuild, fallback). In each path where slot audio is mixed with `shadow_effective_volume(s)`, change the per-sample loop to advance the fade and use the faded gain.

The current pattern looks like:
```c
float vol = shadow_effective_volume(s);
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    mix[i] += (int32_t)lroundf((float)render_buffer[i] * vol);
}
```

Change to:
```c
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    float vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
    mix[i] += (int32_t)lroundf((float)render_buffer[i] * vol);
    if ((i & 1) == 1) shadow_fade_advance(s);  /* advance once per frame (stereo pairs) */
}
```

**Important:** Only advance the fade once per stereo frame (every 2 samples), not every sample, since L and R samples should have the same gain. The `(i & 1) == 1` check means we advance after each R sample.

**Important:** Only advance the fade in ONE mix path per render cycle — whichever path actually runs. The paths are mutually exclusive (pre-render vs deferred, Link Audio rebuild vs fallback). Pick the primary mix loop in each path. The Link Audio capture loops that use `cap_vol` should also use the faded gain for consistency, but should NOT advance the fade.

For capture-only loops (Link Audio publisher), just read the gain without advancing:
```c
float cap_vol = shadow_effective_volume(s) * shadow_chain_slots[s].fade.gain;
```

**Step 3: Commit**

```bash
git add src/host/shadow_chain_mgmt.h src/schwung_shim.c
git commit -m "feat: apply per-sample fade gain ramp in slot audio mix"
```

---

### Task 7: Remove mute_countdown from chain_host.c

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Step 1: Remove g_mute_countdown (v1 global path)**

- Remove `static int g_mute_countdown = 0;` (line 631)
- Remove `#define MUTE_BLOCKS_AFTER_SWITCH 8` (line 632)
- Remove `g_mute_countdown = 0;` in `unload_patch()` (line 3182)
- Remove `g_mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;` in `load_patch()` (line 3432)
- Remove the mute block in `plugin_render_block()` (lines 4065-4069):
  ```c
  if (g_mute_countdown > 0) {
      g_mute_countdown--;
      memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
      return;
  }
  ```

**Step 2: Remove inst->mute_countdown (v2 per-instance path)**

- Remove `int mute_countdown;` from `chain_instance_t` struct (line 476)
- Remove all `inst->mute_countdown = MUTE_BLOCKS_AFTER_SWITCH;` assignments (lines 6634, 6943, 6986, 7024)
- Remove the mute block in `v2_render_block()` (lines 8348-8352):
  ```c
  if (inst->mute_countdown > 0) {
      inst->mute_countdown--;
      memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
      return;
  }
  ```

**Note:** Keep `MUTE_BLOCKS_AFTER_SWITCH` defined if it's used elsewhere, but grep first — if only used by mute_countdown, remove it entirely.

**Step 3: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "refactor: remove mute_countdown, replaced by shim-level fade envelope"
```

---

### Task 8: Handle edge cases

**Files:**
- Modify: `src/host/shadow_chain_mgmt.c`

**Step 1: Handle rapid patch switching**

In `shadow_inprocess_handle_ui_request()`, if a fade-out is already in progress (`fade.target == 0.0f && fade.gain > 0.0f`) and a new patch request arrives:
- Simply update `fade.pending_patch` to the new index (overwrite the previous pending)
- If a `pending_clear` was set, clear it (load takes priority over clear)

**Step 2: Handle first patch load on empty slot**

When `shadow_inprocess_handle_ui_request()` processes a load request and `fade.gain == 0.0f` and the slot has no active patch (`!shadow_chain_slots[slot].active`), skip the fade-out entirely:
- Load the patch immediately
- Set `fade.target = 1.0f` to fade in

This is already handled by the "gain == 0.0" branch from Task 3.

**Step 3: Commit**

```bash
git add src/host/shadow_chain_mgmt.c
git commit -m "fix: handle rapid patch switching and empty slot edge cases"
```

---

### Task 9: Build and test on hardware

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh
```

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Test on hardware**

Test these scenarios:
- Load a patch into an empty slot → should fade in smoothly (no pop)
- Switch patch in a playing slot → should fade out old, silence during load, fade in new
- Clear a playing slot → should fade out smoothly (no pop)
- Rapidly switch patches (tap multiple patches quickly) → should not crash, last patch wins
- Switch all 4 slots at once (set change) → all should fade smoothly
- Muted/soloed slot patch switch → should still work correctly

**Step 4: Commit any fixes from testing**
