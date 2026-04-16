# MFX ME-Only Bus Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Stop round-tripping Move's mailbox audio through prescale/postscale. Leave Move's audio untouched; build a separate ME ("Move Engine" / Schwung) bus at unity, apply Master FX to that bus only, and sum into the mailbox at master-volume level. Capture consumers (skipback / quantized sampler / native resample bridge) read a derived unity view instead of the mailbox.

**Architecture:**
1. The mailbox stays at Move's volume level for the whole frame. No more `mailbox *= 1/mv` then `mailbox *= mv`.
2. A stack-local `me_unity[FRAMES_PER_BLOCK*2]` int32 accumulator collects all ME sources (slot synths + slot FX + overtake DSP) at full gain.
3. Master FX runs **only on `me_unity`**, never on Move's audio. This is the product change endorsed by the user: Schwung FX no longer affect Move's audio.
4. Final sum: `mailbox[i] = saturate(mailbox[i] + round(me_unity[i] * mv))`.
5. Capture consumers need unity audio. We build a short-lived `unity_view[]` only when any consumer is active (sampler recording, skipback enabled, or native bridge subscribers), computed as `unity_view[i] = round(mailbox_before_me_sum[i] / mv) + me_unity[i]`. When nothing is active, skip it entirely (idle fast path).
6. Link Audio rebuild path (`rebuild_from_la`) is preserved separately — that path already zeroes the mailbox and reconstructs per-track, so it operates on a different premise and keeps its existing logic (no prescale needed there anyway).

**Tech Stack:** C (shim, LD_PRELOAD), runs in realtime SPI callback context. No allocations, no locks, no logging in the callback path.

**Risk:** This touches every frame going to the DAC and every capture consumer. Getting the math wrong produces silence, +6dB, distortion, or glitched skipback/sampler captures. Do this in a worktree, build locally, and test on-device before merging. Test matrix below is mandatory.

**Recommendation:** Run this plan in a dedicated git worktree (e.g., `git worktree add ../schwung-mfx-me-only`), because the change is in the realtime audio path and the blast radius is the entire speaker output + every capture feature.

---

## Context: What's There Today

Live mix function: `src/schwung_shim.c:1488` — `shadow_inprocess_mix_from_buffer()`.

Today it does this sequence on `mailbox_audio` (the Move mailbox at `AUDIO_OUT_OFFSET=256`):

1. Snapshot Move's audio into `native_bridge_move_component` (line 1499).
2. Mix JACK/RNBO into mailbox at `mv` level (line 1531-1542).
3. **Prescale mailbox up by `1/mv`** to reach unity (line 1544-1554) — the round-trip problem.
4. Mix slot outputs (deferred FX or inline FX) + overtake DSP into mailbox at unity (1568-1762).
5. Accumulate `me_full` alongside for the bridge ME component (1642, 1700, 1748, 1761).
6. Apply overtake DSP FX to combined Move+ME mailbox (1786-1788).
7. Apply Master FX to combined Move+ME mailbox (1791-1796) — the product issue.
8. `native_capture_total_mix_snapshot_from_buffer(mailbox_audio)` (1805).
9. `sampler_capture_audio()` (reads `gmmap + 256` directly at `shadow_sampler.c:832`) and `skipback_capture(mailbox_audio)` (1881-1885).
10. **Postscale mailbox back down by `mv`** (line 1891-1898) to the DAC.

The round-trip in (3) and (10) is where clean-idle distortion and the reported "+6dB" hazard live: if `mv` is stale or off by small error vs the actual volume already baked into Move's audio, the mailbox is left with an incorrect gain stage.

Dead code: `shadow_inprocess_mix_audio()` at `src/schwung_shim.c:941` — has no callers. Part of this plan deletes it.

Bridge split ME reconstruction: `shadow_resample.c:441-459` already consumes `(move_component, me_component, mv)` as three inputs and reconstructs unity by scaling move up by `1/mv`. This is perfectly aligned with the new architecture — no changes needed there beyond ensuring `native_bridge_capture_mv` still reflects `mv` at snapshot time.

Sampler reads mailbox directly (`shadow_sampler.c:832,1227` via `SAMPLER_AUDIO_OUT_OFFSET=256`). This is a problem: the mailbox will no longer be at unity. The plan adds a tiny API change — pass a unity buffer to the sampler — so it reads from the unity view instead.

---

## Test Matrix (Manual, On-Device)

This is audio correctness work. Every task ends with a build + deploy + ear-check, not just a code review. The baseline deploy command:

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Then on the device:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
```

The mandatory scenarios to run at the end of Tasks 5, 7, 8, 9:

1. **Clean idle (reported bug repro):** fresh install, no modules, no patch loaded, Move→Schwung OFF, Schwung→Link OFF. Play a Move clip and sweep master volume 0→100→0. Expect: speaker output tracks master volume cleanly; no +6dB or distortion; reducing slot gain has no effect (because no slots are active).
2. **Move-only at 50% volume:** no Schwung patch, Move project playing. Expect: identical audio character to stock Move (within measurement noise).
3. **Single slot synth at 50% volume:** load a patch on slot 1, play Move + synth. Expect: Move audio unchanged; synth mixes in at slot volume × master volume.
4. **Slot with FX at 50% volume:** load synth + slot-local FX (reverb). Expect: slot FX apply to slot audio only, not Move's.
5. **Master FX at 50% volume:** load Master FX (a recognizable one like psxverb). Expect: MFX processes ME only — Move's audio is dry through the speakers. This is the product change.
6. **Skipback / Shift+Capture:** trigger skipback after a 10s jam. Expect: captured WAV is at unity (loud). Master volume should not be baked in.
7. **Quantized resampler, Move Input source:** Shift+Sample, choose Move Input, 1 bar. Expect: captured WAV is at unity.
8. **Quantized resampler, Resample source:** same but source = Resample. Expect: captured WAV is at unity, includes ME audio.
9. **Link Audio rebuild:** Move→Schwung ON, subscribe from Live. Expect: per-track routing still works; no change vs. before.
10. **RNBO / JACK audio:** if an RNBO app is publishing, it should still mix in at the correct level (it's currently scaled by mv in the shim before prescale — needs thought in Task 3).

---

### Task 1: Set up worktree and create failing end-to-end test note

**Files:**
- Create: `docs/plans/2026-04-16-mfx-me-only-bus-testlog.md` (free-form running notes — not committed, or kept scratch)

**Step 1: Create worktree**

Run:
```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
git worktree add ../schwung-mfx-me-only -b mfx-me-only
cd ../schwung-mfx-me-only
```

Expected: worktree created, new branch `mfx-me-only` checked out there.

**Step 2: Baseline build + clean-idle bug repro on device**

On current `main` (before any code change), build + deploy, reproduce the bug: "clean install, no modules, Move→Schwung OFF, Schwung→Link OFF — audio increases by 6dB on Move audio vs. stock."

Run:
```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Then on device: play a Move clip at 80% volume, A/B against MoveOriginal (reboot to stock Move, same clip, same volume). Note any perceptible difference in level or character.

Expected: documented baseline. If the bug does not repro on your device, document the actual observable difference you *can* hear/measure — whatever "audio floor changes" means in practice.

**Step 3: Commit the plan file**

```bash
git add docs/plans/2026-04-16-mfx-me-only-bus.md
git commit -m "plan: MFX ME-only bus refactor"
```

---

### Task 2: Delete dead code `shadow_inprocess_mix_audio()`

The legacy mix function at `src/schwung_shim.c:941-1086` has no callers (confirmed by grep). Deleting it first removes noise so the refactor of `shadow_inprocess_mix_from_buffer()` is clearer.

**Files:**
- Modify: `src/schwung_shim.c:941-1086` (delete the whole function body)

**Step 1: Verify no callers**

Run:
```bash
```

(Use the Grep tool with pattern `shadow_inprocess_mix_audio\b` across `src/`. Expect exactly one hit: the function definition itself. No callers.)

Expected: single hit at the definition line. If any other hit appears, STOP and investigate — do not delete.

**Step 2: Delete the function**

Remove the whole function block starting at `static void shadow_inprocess_mix_audio(void)` and ending at the closing brace before `shadow_inprocess_mix_from_buffer`. Remove any forward declaration if present.

**Step 3: Build**

Run:
```bash
./scripts/build.sh
```

Expected: clean build. No warnings about unused statics that were only used by the deleted function (if there are any, clean them up in the same commit).

**Step 4: Commit**

```bash
git add src/schwung_shim.c
git commit -m "refactor: remove dead shadow_inprocess_mix_audio()"
```

---

### Task 3: Introduce `me_unity[]` accumulator inside `shadow_inprocess_mix_from_buffer()` (no behavior change yet)

The goal here is to introduce the scratch buffer and populate it in parallel with the existing mailbox math, but NOT yet change the output. This is a refactor-prep step to make the diff in Task 4 reviewable.

**Files:**
- Modify: `src/schwung_shim.c:1488-1899` — `shadow_inprocess_mix_from_buffer()`

**Step 1: Add the stack-local scratch buffer**

At the top of the function, alongside `int32_t me_full[FRAMES_PER_BLOCK * 2]`, add:

```c
/* ME-only unity bus: sum of slot synths + slot FX + overtake DSP, full-gain,
 * before Master FX and master volume. Used in Task 4 to replace the
 * mailbox round-trip. Sized to avoid stack blowup — 512 bytes. */
int32_t me_unity[FRAMES_PER_BLOCK * 2];
memset(me_unity, 0, sizeof(me_unity));
```

**Step 2: Mirror every place that accumulates into `me_full[]`**

Wherever the existing code does `me_full[i] += ...`, add the same addition to `me_unity[i]`. Lines to touch:

- `src/schwung_shim.c:1642` (rebuild_from_la active slot): `me_full[i] += (int32_t)lroundf((float)fx_buf[i] * vol);` — also add to `me_unity`.
- `src/schwung_shim.c:1700` (deferred FX path): same.
- `src/schwung_shim.c:1748` (inline FX fallback path): same.
- `src/schwung_shim.c:1761` (overtake DSP mix-in): `me_full[i] += (int32_t)shadow_deferred_dsp_buffer[i];` — also add to `me_unity`.

At this point `me_unity[]` is populated but unused. This is deliberate — the next task is where we switch the mailbox logic over.

**Step 3: Build**

Run:
```bash
./scripts/build.sh
```

Expected: clean build. Compiler may warn that `me_unity` is unused — suppress with `(void)me_unity;` at the end of the function for now, or accept the warning if it's a note not an error.

**Step 4: Deploy and verify no regression**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Run test scenarios 1, 2, 3 from the test matrix. Expected: no audible change from current main — we have NOT yet changed behavior.

**Step 5: Commit**

```bash
git add src/schwung_shim.c
git commit -m "refactor: introduce me_unity accumulator (no behavior change)"
```

---

### Task 4: Route Master FX through `me_unity` only; remove prescale/postscale on mailbox

This is the core change. The mailbox stops round-tripping through unity. Master FX stops processing Move's audio. The DAC output becomes `mailbox (Move at mv) + me_unity * mv`.

**Files:**
- Modify: `src/schwung_shim.c:1488-1899` — `shadow_inprocess_mix_from_buffer()`

**Step 1: Delete the prescale loop**

Remove `src/schwung_shim.c:1544-1554` (the `if (!rebuild_from_la && mv > 0.001f && mv < 0.9999f) { ... }` block that multiplies mailbox by `1/mv`). The mailbox now stays at Move's original level through the whole non-rebuild path.

**Step 2: Stop mixing slot output into mailbox in the non-rebuild path**

In the `else if (shadow_chain_process_fx)` branch (line 1668 onward), every place that does:

```c
int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)lroundf((float)fx_buf[i] * gain);
...
mailbox_audio[i] = (int16_t)mixed;
```

Change to ONLY accumulate into `me_unity` (don't touch `mailbox_audio` here at all — we'll sum once at the end with mv applied). Replace with:

```c
me_unity[i] += (int32_t)lroundf((float)fx_buf[i] * gain);
```

Lines affected: 1696-1702, 1744-1750. Keep the `me_full[i] += ...` line for native bridge compatibility (unchanged; `me_full` still represents slot contribution at full gain for the ME component of the split).

**Important:** The `rebuild_from_la` path (lines 1570-1666) is a different story — it zeroes the mailbox first and rebuilds per-track, including inactive slots passing Link Audio through. That path's mailbox writes stay as-is for now (Task 6 revisits). We only touch the non-rebuild branch here.

**Step 3: Stop mixing overtake DSP into mailbox in the non-rebuild path**

Line 1755-1762: today this adds `shadow_deferred_dsp_buffer` to `mailbox_audio` AND to `me_full`. Change to ONLY add to `me_unity` and `me_full`; do NOT touch `mailbox_audio`:

```c
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    me_unity[i] += (int32_t)shadow_deferred_dsp_buffer[i];
    me_full[i]  += (int32_t)shadow_deferred_dsp_buffer[i];
}
```

Note: if `rebuild_from_la` is true, we still want overtake DSP to mix in. Gate this block with `if (!rebuild_from_la)`. Inside the `rebuild_from_la` branch, we'll need to decide (Task 6) whether overtake DSP adds to mailbox or me_unity — for now, leave the rebuild_from_la branch exactly as it is and add the gate here.

**Step 4: Restrict overtake DSP FX to ME only**

Line 1786-1788 runs `overtake_dsp_fx->process_block` on `mailbox_audio` (which in the old world was Move+ME at unity). In the new world, `mailbox_audio` is Move-only at mv. We want overtake DSP FX to apply only to ME. Restructure: run it on a sat-clamped int16 snapshot of `me_unity` — but note this returns int16 so we need a scratch buffer. Add:

```c
int16_t me_unity_i16[FRAMES_PER_BLOCK * 2];
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    int32_t v = me_unity[i];
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    me_unity_i16[i] = (int16_t)v;
}
if (overtake_dsp_fx && overtake_dsp_fx_inst && overtake_dsp_fx->process_block) {
    overtake_dsp_fx->process_block(overtake_dsp_fx_inst, me_unity_i16, FRAMES_PER_BLOCK);
}
```

**Step 5: Route Master FX through `me_unity_i16` instead of `mailbox_audio`**

Lines 1791-1796. Change `s->api->process_block(s->instance, mailbox_audio, ...)` to `s->api->process_block(s->instance, me_unity_i16, ...)`. Master FX no longer touches Move's audio.

**Step 6: Remove the postscale loop**

Delete `src/schwung_shim.c:1891-1898` (the `if (mv < 0.9999f) { ... mailbox_audio[i] = round(mailbox * mv) ... }` block). No longer needed — mailbox already at mv.

**Step 7: Add a final sum of `me_unity_i16 * mv` into mailbox**

Right after the Master FX loop (replacing the deleted postscale), add:

```c
/* Sum ME bus (after MFX/overtake FX) into mailbox at master volume level.
 * Move's audio in mailbox is already at mv; ME needs mv applied. */
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    int32_t scaled_me = (int32_t)lroundf((float)me_unity_i16[i] * mv);
    int32_t summed = (int32_t)mailbox_audio[i] + scaled_me;
    if (summed > 32767) summed = 32767;
    if (summed < -32768) summed = -32768;
    mailbox_audio[i] = (int16_t)summed;
}
```

This is the new output stage.

**Step 8: JACK/RNBO mix adjustment**

Lines 1531-1542 currently mix `jack_audio * mv` into mailbox BEFORE the (deleted) prescale. With prescale gone, we need to decide: is JACK/RNBO part of the "ME" bus or part of Move's bus?

Decision: JACK/RNBO audio is external to both. Today it's effectively passed through at mv (so it appears at 1x after prescale+postscale cancels). In the new world, to preserve that behavior, leave the JACK mix-into-mailbox-at-mv exactly as it is. It now goes directly to DAC without round-trip. Same audible result, simpler path.

Leave `1531-1542` unchanged.

**Step 9: Build**

Run:
```bash
./scripts/build.sh
```

Expected: clean build. If there are unused-variable warnings (e.g. `me_full` if we reduced its use), handle later — it's still used for `native_bridge_me_component`.

**Step 10: Deploy and RUN TEST MATRIX 1-5**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Test scenarios 1, 2, 3, 4, 5 from the matrix. This is the critical checkpoint.

- Scenario 1 (clean idle): the reported bug should now be fixed. Move audio should be indistinguishable from stock Move.
- Scenario 2 (Move at 50%): identical to stock.
- Scenario 3 (slot synth at 50%): synth mixes in at correct level; Move unchanged.
- Scenario 4 (slot FX): FX apply to slot only.
- Scenario 5 (Master FX): **MFX now applies to ME only**. Move audio is dry. Product-intentional change.

If any of these fail: STOP. Do not proceed to Task 5. Debug. The most likely failure modes are: (a) double-counting ME (sounds louder than before for slot-only audio at low master volume), (b) MFX silent (dry chain in `me_unity_i16` not being processed correctly), (c) Move audio still affected by MFX (the Step 5 redirect didn't take).

**Step 11: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: route MFX through ME-only bus, eliminate mailbox round-trip"
```

---

### Task 5: Build `unity_view[]` for capture consumers (skipback path first)

Capture consumers need the unity-level sum (Move at unity + ME at unity, post-MFX). The mailbox no longer provides this. We build a short-lived unity view on demand.

**Files:**
- Modify: `src/schwung_shim.c` — `shadow_inprocess_mix_from_buffer()`

**Step 1: Define the buffer and the activation check**

Right before the sampler capture block (around line 1879 in pre-refactor numbering — adjust to wherever sampler_source check is now), add:

```c
/* Build unity_view only if any consumer needs it (skipback, sampler, or
 * native bridge). unity_view = move_at_unity + me_at_unity_post_mfx.
 * Kept separate from mailbox so mailbox stays at mv for DAC. */
int16_t unity_view[FRAMES_PER_BLOCK * 2];
int unity_view_valid = 0;

int need_unity = 0;
if (sampler_source == SAMPLER_SOURCE_RESAMPLE) need_unity = 1;
if (native_bridge_split_valid_consumer_active()) need_unity = 1;  /* see step 4 */
```

(Skipback always captures when `sampler_source == SAMPLER_SOURCE_RESAMPLE`, per current line 1884-1885, so the sampler_source test covers skipback too.)

**Step 2: Populate `unity_view` when needed**

```c
if (need_unity) {
    float inv_mv = (mv > 0.001f) ? 1.0f / mv : 1.0f;
    if (inv_mv > 20.0f) inv_mv = 20.0f;
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        /* Reconstruct Move at unity from native_bridge_move_component,
         * which was captured at line ~1499 before any modifications. */
        float move_unity = (float)native_bridge_move_component[i] * inv_mv;
        float summed = move_unity + (float)me_unity_i16[i];
        if (summed > 32767.0f) summed = 32767.0f;
        if (summed < -32768.0f) summed = -32768.0f;
        unity_view[i] = (int16_t)lroundf(summed);
    }
    unity_view_valid = 1;
}
```

Notes:
- `native_bridge_move_component` was saved at line 1499 (unchanged by Task 4 since we don't touch mailbox before line 1499). Move at unity = `move_component / mv`.
- `me_unity_i16` is the post-MFX ME. This is correct for skipback/sampler (user hears MFX in their capture).
- For clean-idle with no MFX and no ME, `me_unity_i16` is zero and this reduces to `move_unity` — capture is identical to stock Move at full gain, independent of master volume. ✓

**Step 3: Redirect skipback_capture to unity_view**

Line 1885 today: `skipback_capture(mailbox_audio);`. Change to:

```c
if (unity_view_valid) skipback_capture(unity_view);
```

**Step 4: `native_capture_total_mix_snapshot_from_buffer` source**

Line 1805: `native_capture_total_mix_snapshot_from_buffer(mailbox_audio);`. This function (defined at `shadow_resample.c:271`) takes the "total mix at capture time". Before Task 4, that was unity. After Task 4, `mailbox_audio` is at mv. We need unity here.

Change to:
```c
if (unity_view_valid) {
    native_capture_total_mix_snapshot_from_buffer(unity_view);
}
/* else: skip the snapshot; native bridge will rely on move_component +
 * me_component split instead (which shadow_resample.c:441-459 already does). */
```

For this to always have data, `need_unity` must go to 1 when native bridge has subscribers. For now (simplest path), flip `need_unity = 1` unconditionally whenever `link_audio.enabled || shadow_chain_process_fx` is true — i.e. any time the native bridge might be in use. We can refine in a later pass. Leave a TODO comment.

Actually the simpler decision: **always build `unity_view`**. It's 512 bytes stack, ~256 int16 float-mul-add ops. On a modern ARM core doing 44100/128 = 344 frames/sec, this is ~87,000 mul-adds/sec. Negligible. Remove the `need_unity` gate entirely and always compute. Simpler, no hidden skipped snapshots, no edge cases.

Revised step 1+2+4: drop the `need_unity` gate; build `unity_view` every frame in the non-rebuild path. Keep the idle fast-path at the top of the function (Task 7) to skip the whole function when truly nothing is going on.

**Step 5: Build**

```bash
./scripts/build.sh
```

**Step 6: Deploy and run test scenarios 6, 7, 8**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

- Scenario 6 (skipback): verify WAV is at unity (loud) regardless of master volume position during the jam.
- Scenario 7 (Move Input sampler): unity WAV. (This may be a separate path — see Task 6.)
- Scenario 8 (Resample sampler): unity WAV, includes ME audio.

If skipback is now quiet: the redirect to `unity_view` isn't firing, or `mv` inversion is wrong.
If skipback distorts/clips: `me_unity_i16` saturation after MFX may be causing issues — investigate whether MFX is pushing the sum past int16 at unity + moderate ME.

**Step 7: Commit**

```bash
git add src/schwung_shim.c
git commit -m "feat: build unity_view for skipback and native bridge capture"
```

---

### Task 6: Redirect quantized sampler to `unity_view` (API change)

`sampler_capture_audio()` at `shadow_sampler.c:832` reads `gmmap + SAMPLER_AUDIO_OUT_OFFSET` directly. We need to give it the unity view instead.

**Files:**
- Modify: `src/host/shadow_sampler.h` — add new function or parameter
- Modify: `src/host/shadow_sampler.c:823,832` and `:1220,1227` — change to take a buffer
- Modify: `src/schwung_shim.c` — call site at line 1881

**Step 1: Add a new variant `sampler_capture_audio_from_buffer(const int16_t *src)`**

In `shadow_sampler.h`, add:

```c
/* Capture from a caller-provided buffer (unity level). Used by the ME-only
 * bus refactor to decouple capture level from DAC level. */
void sampler_capture_audio_from_buffer(const int16_t *src);
```

**Step 2: Implement it in `shadow_sampler.c`**

Duplicate the body of `sampler_capture_audio()` but read from `src` parameter instead of `gmmap + SAMPLER_AUDIO_OUT_OFFSET`. Keep the old `sampler_capture_audio()` intact for the MOVE_INPUT source path — it reads audio-in, not audio-out, so it's unaffected by the refactor. (Double check: look at the existing implementations at lines 823 and 1220; the one at 823 is RESAMPLE-side reading audio-out; the one at 1227 is MOVE_INPUT reading audio-in. The MOVE_INPUT path is separate — see Task 6 step 4.)

Actually, look at the code more carefully before implementing. Load `shadow_sampler.c` lines 810-900 and 1200-1240 to confirm which is which. If `sampler_capture_audio` unconditionally reads AUDIO_OUT, then we can change it in place to take a buffer and all call sites. If it branches on source, we only change the RESAMPLE branch.

**Step 3: Redirect the call site at `schwung_shim.c:1881`**

```c
if (sampler_source == SAMPLER_SOURCE_RESAMPLE) {
    if (unity_view_valid) sampler_capture_audio_from_buffer(unity_view);
    else                  sampler_capture_audio();  /* fallback, should not happen */
    sampler_tick_preroll();
    skipback_init();
    if (unity_view_valid) skipback_capture(unity_view);
}
```

**Step 4: Move Input source**

The Move Input path (lines ~4499-4503 in the pre-refactor shim, per the original summary) is a different function reading audio-in, not audio-out. It captures microphone/line-in input, which should not be affected by this refactor — no master volume is ever applied to Move Input capture as far as I can tell. Verify by reading lines around 4450-4510 and confirming the MOVE_INPUT branch does not read from `gmmap + AUDIO_OUT_OFFSET`. If it does, redirect similarly.

**Step 5: Build + deploy + test scenarios 7, 8**

As before.

**Step 6: Commit**

```bash
git add src/host/shadow_sampler.c src/host/shadow_sampler.h src/schwung_shim.c
git commit -m "feat: route quantized sampler through unity_view"
```

---

### Task 7: Add idle fast-path early return

When nothing is active (no Link Audio rebuild, no slots loaded, no overtake DSP, no Master FX, no overtake DSP FX), we can skip the entire function body. Today this is not possible because the mailbox round-trip happens unconditionally. With Task 4 done, the mailbox is untouched in the idle case, so we can detect the idle state up front and return immediately.

**Files:**
- Modify: `src/schwung_shim.c` — top of `shadow_inprocess_mix_from_buffer()`

**Step 1: Add the idle check**

Right after `if (!shadow_deferred_dsp_valid) return;`, add:

```c
/* Fast path: nothing active. Leave Move's mailbox untouched. */
int any_slot = 0;
for (int s = 0; s < SHADOW_CHAIN_INSTANCES; s++) {
    if (shadow_chain_slots[s].instance) { any_slot = 1; break; }
}
int any_mfx = 0;
for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
    if (shadow_master_fx_slots[fx].instance) { any_mfx = 1; break; }
}
int any_overtake_dsp = (overtake_dsp_fx && overtake_dsp_fx_inst);
int any_jack = (schwung_jack_bridge_read_audio(g_jack_shm) != NULL);
int any_la_rebuild = (link_audio.enabled && link_audio_routing_enabled &&
                     shadow_chain_process_fx && link_audio.move_channel_count >= 4);
int any_capture = (sampler_source == SAMPLER_SOURCE_RESAMPLE);

if (!any_slot && !any_mfx && !any_overtake_dsp && !any_jack &&
    !any_la_rebuild && !any_capture) {
    /* Still need to snapshot Move component in case bridge is queried */
    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    memcpy(native_bridge_move_component, mailbox_audio, AUDIO_BUFFER_SIZE);
    memset(native_bridge_me_component, 0, AUDIO_BUFFER_SIZE);
    native_bridge_capture_mv = shadow_master_volume;
    native_bridge_split_valid = 1;
    return;
}
```

Caveat: `schwung_jack_bridge_read_audio` consumes from the ring, so you can't call it just to check presence. Replace that check with a cheaper "does a JACK client exist" signal. If there's no such signal, leave JACK in the always-mix path — JACK is rare enough that it's fine to not idle-optimize that case (just drop `any_jack` from the condition and accept that JACK presence keeps the full path active; users with JACK probably have a slot loaded too).

**Step 2: Build + deploy + scenario 1**

Scenario 1 must still show clean Move audio unchanged from stock. Also verify: loading a patch mid-playback transitions smoothly (slot becomes active → fast path exits → full path engages without pop).

**Step 3: Commit**

```bash
git add src/schwung_shim.c
git commit -m "perf: idle fast-path in shadow_inprocess_mix_from_buffer"
```

---

### Task 8: Handle `rebuild_from_la` interaction with MFX

The `rebuild_from_la` branch (zero mailbox + reconstruct from Link Audio tracks) operated on a different premise: "mailbox is zero then filled with per-track routed audio." In that world, MFX processing the mailbox was effectively processing ME too (since Move wasn't in the mailbox at all).

In the new world, `rebuild_from_la` still zeroes the mailbox. Everything put back in is ME-like (slot-FX'd tracks + inactive-slot passthrough). MFX should still process this audio. Decision: in the `rebuild_from_la` path, keep the old behavior — MFX runs on `mailbox_audio` (which is all-ME content), not on `me_unity_i16`.

**Files:**
- Modify: `src/schwung_shim.c` — MFX loop at line 1791

**Step 1: Branch MFX target on `rebuild_from_la`**

```c
int16_t *mfx_target = rebuild_from_la ? mailbox_audio : me_unity_i16;
for (int fx = 0; fx < MASTER_FX_SLOTS; fx++) {
    master_fx_slot_t *s = &shadow_master_fx_slots[fx];
    if (s->instance && s->api && s->api->process_block) {
        s->api->process_block(s->instance, mfx_target, FRAMES_PER_BLOCK);
    }
}
```

Similarly for `overtake_dsp_fx->process_block` at line 1786-1788.

**Step 2: In rebuild_from_la path, skip the final `mailbox += me_unity_i16 * mv` sum**

Because rebuild_from_la already wrote the final audio to mailbox directly (with per-slot volume already applied at lines 1635-1644, 1696-1702). Summing `me_unity * mv` on top would double-count. Gate the Task-4 Step-7 sum block:

```c
if (!rebuild_from_la) {
    /* Sum ME bus into mailbox at mv */
    for (...) { ... }
}
```

**Step 3: Unity view under rebuild_from_la**

In rebuild_from_la, the mailbox is effectively at "per-slot-volume" level (not mv). The unity view for capture is trickier: today's code captured mailbox AFTER MFX as the "unity" value — which assumes mailbox is unity. That assumption broke in Task 4 for the non-rebuild path but is still approximately true in rebuild_from_la (slot passthrough is at unity, active slots go through fade×vol which is ≤1.0 of slot audio).

Decision: in rebuild_from_la, use `mailbox_audio` (post-MFX) as the unity view directly. It's not strictly "unity" — it's "slot-summed at per-slot volumes" — but that's what the old code did too, and Link Audio users aren't expecting master volume independence in captures (they're recording a per-track routed mix). Preserve existing behavior.

Concretely:
```c
if (rebuild_from_la) {
    memcpy(unity_view, mailbox_audio, AUDIO_BUFFER_SIZE);
    unity_view_valid = 1;
} else {
    /* build from move_component + me_unity_i16 as in Task 5 */
}
```

**Step 4: Build + deploy + scenario 9**

Link Audio subscribe from Live, route some tracks, verify audio still works and MFX still affects the Link-routed audio.

**Step 5: Commit**

```bash
git add src/schwung_shim.c
git commit -m "fix: preserve MFX behavior under Link Audio rebuild path"
```

---

### Task 9: Final integration test pass + documentation update

**Files:**
- Modify: `docs/GAIN_STAGING_ANALYSIS.md` — mark Option B as implemented, date it
- Modify: `CLAUDE.md` — short note under Signal Chain or a new "Gain Staging" section describing the ME-only MFX bus and the capture unity view

**Step 1: Run full test matrix 1-10**

Don't skip any. Document results in the plan's commit message or a scratch file.

**Step 2: Update GAIN_STAGING_ANALYSIS.md**

Mark Option B implemented. Add a paragraph: "As of 2026-04-16, the shim leaves Move's mailbox audio untouched and builds a separate ME bus that runs through Master FX at unity. Capture consumers read from `unity_view = move_at_unity + me_post_mfx`."

**Step 3: Update CLAUDE.md**

Add under the Signal Chain section or as a new "Gain Staging" note:

```
### Gain Staging (MFX ME-Only Bus)

Master FX processes only Schwung's internal audio (slot synths, slot FX,
overtake DSP) — never Move's audio. The shim builds:

- mailbox (DAC output) = Move audio at mv + me_bus × mv
- unity_view (captures) = Move at unity + me_bus_post_mfx

Skipback, quantized sampler, and the native resample bridge read
unity_view so captures are independent of master volume. Clean-idle
(no modules loaded) leaves Move's mailbox untouched; no round-trip.
```

**Step 4: Commit docs**

```bash
git add docs/GAIN_STAGING_ANALYSIS.md CLAUDE.md
git commit -m "docs: document MFX ME-only bus and capture unity view"
```

**Step 5: Merge back to main**

When all scenarios pass and no regressions are found, merge:

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
git merge --no-ff mfx-me-only
git push
git worktree remove ../schwung-mfx-me-only
```

(Don't force anything. Don't delete the branch until you're sure.)

---

## Rollback Plan

If at any point a task regresses clean-idle or any capture scenario and you can't fix it quickly, revert the specific commit and re-evaluate. Every task is one commit; `git revert <sha>` should cleanly back out any single step.

The safest rollback is to Task 2's commit (just the dead code deletion) — everything before Task 4 is behavior-preserving. If Task 4 shows regressions you can't explain in an afternoon, revert Task 4+ and think harder about the order of operations (the gate between rebuild_from_la and non-rebuild, or the overtake DSP FX redirect).

## Notes

- **No new allocations in the audio callback.** All new buffers are stack-allocated int16/int32 arrays sized to `FRAMES_PER_BLOCK * 2` (512/1024 bytes). Safe for realtime.
- **No new locks, no new I/O.** This is pure arithmetic restructuring.
- **mv staleness** is now a cosmetic issue for capture level only (slightly-wrong unity reconstruction of Move in `unity_view`). It no longer affects DAC output. This is the main reason this refactor is worth doing.
- **The product change** (MFX no longer affects Move's audio) is user-endorsed. If users complain, the counter is: stock Move has its own master FX; Schwung's MFX is for Schwung's signal. Update MANUAL.md if the distinction surfaces in user-facing docs.
