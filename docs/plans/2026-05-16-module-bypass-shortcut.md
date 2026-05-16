# Module Bypass Shortcut Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Mute+JogClick shortcut to bypass an individual module (MIDI FX, synth, audio FX, or Master FX slot) within a shadow chain. Bypass is host-side: chain skips the component's processing step, audio/MIDI flow through unchanged. State persists with the patch.

**Architecture:** Per-component `bypassed` flag in `chain_host` (and Master FX manager). Render loop skips the component's `render_block`; MIDI FX/audio FX pass their input through; synth bypass still delivers MIDI (so voices age out naturally) but skips render. Shadow UI tracks CC 88 held-state in JS, intercepts jog-click on the focused module position, and toggles the flag via `setSlotParam(slot, "<comp>:bypassed", "1")`. Patch JSON gains a per-component `"bypassed": true` field. Visual indicator: same-height 'B' glyph rendered like the existing `~N` LFO indicator.

**Tech Stack:** C (chain_host DSP, shadow chain mgmt for Master FX), QuickJS (shadow_ui.js), shared param-string protocol (`slot:`, `fx1:`, etc.).

**Testing model:** This project has no automated test suite. Verification is manual on hardware after each task that touches runtime behavior. Build with `./scripts/build.sh`, deploy with `./scripts/install.sh local --skip-modules --skip-confirmation`. Pre-commit only after the behavior verifies on-device.

**No code change without first reading the referenced lines.** All file paths in this plan are relative to the worktree root `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/.worktrees/module-bypass-shortcut/`.

---

## Task 1: Orientation (no code change)

**Goal:** Build a precise mental map of the four code locations bypass touches, so subsequent tasks don't guess.

**Files to read:**
- `src/modules/chain/dsp/chain_host.c` — sections to skim:
  - Synth helpers around `synth_loaded`, `synth_on_midi`, `synth_render_block` (~lines 595-633)
  - Audio FX storage: `g_fx_handles`, `g_fx_plugins`, `g_fx_instances` and `MAX_AUDIO_FX = 4` (~lines 67, 587-647)
  - The main render path — search for `render_block` calls inside the chain's per-frame processor. Note the exact order: MIDI FX tick → synth render → FX chain
  - Param read/write entry points (search `chain_host_get_param`, `chain_host_set_param` or v2 equivalents) — these are where `synth:name`, `fx1_module`, `slot:muted` etc. are dispatched
  - MIDI FX tick: `v2_tick_midi_fx` (~line 1733) — note how it sends out to synth
- `src/shadow/shadow_ui.js`:
  - `getSlotParam`/`setSlotParam` (~lines 2576-2620) — the JS↔chain_host bridge
  - The chain_params definition list with `slot:muted` entry (~line 1554) — bypass entries get added in the same list
  - The module-list view jog-click handler (~line 3525 was a match for "Jog click - select module"; trace from there)
  - LFO `~N` indicator render (`shadow_ui.js:12903`) — 'B' marker uses the same drawing primitive at a different position
- `src/host/shadow_chain_mgmt.c` and `shadow_chain_mgmt.h` — Master FX storage and per-slot param routing
- `docs/plans/2026-05-16-module-bypass-shortcut-design.md` — the approved design (already on `main`)

**Step 1: Read each file.** Take notes (in your head or scratch) on:
1. Exactly which struct field holds the per-component instance pointer
2. Where the param-key dispatcher is for the chain (so you know where to add `synth:bypassed`, `midi_fx1:bypassed`, `fx1:bypassed`..`fx4:bypassed`)
3. Where the Master FX param dispatcher is
4. The Move chain hardware MIDI keycode for CC 88 (Mute) — confirm it's already in `src/shared/constants.mjs` as `MoveMute = 88`

**Step 2: Confirm assumptions.** Before moving on, you should be able to answer in one sentence each:
- "Audio FX bypass means: skip _____ and copy _____ to _____."
- "Synth bypass means: skip _____ but still call _____."
- "MIDI FX bypass means: skip _____ and forward _____ unchanged to _____."
- "Bypass state is read at frame N from _____ and written by _____."

**No commit for this task.** It's pure reading.

---

## Task 2: Add bypass storage in chain_host

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Goal:** Add the storage that holds bypass state. No behavior change yet — just the field.

**Step 1: Add static state.** Near the existing `g_fx_*` static arrays (~line 587), add:

```c
/* Per-component bypass flags. 1 = bypassed (skip processing), 0 = active. */
static int g_synth_bypassed = 0;
static int g_midi_fx1_bypassed = 0;
static int g_fx_bypassed[MAX_AUDIO_FX] = {0, 0, 0, 0};
```

If chain_host uses the v2 instance-based pattern (per-`chain_instance_t`), add the equivalent inside `chain_instance_t` (search for the struct definition — likely near `audio_fx_config_t`). Check both. If both exist (singleton + per-instance), add to both, with the per-instance taking precedence in code.

**Step 2: Wire bypass param keys into get/set.** Find the chain_host get_param/set_param routing for slot-level params. The pattern looks like:

```c
if (strcmp(key, "slot:muted") == 0) { ... }
```

Add handlers for:
- `synth:bypassed` → `g_synth_bypassed`
- `midi_fx1:bypassed` → `g_midi_fx1_bypassed`
- `fx1:bypassed` → `g_fx_bypassed[0]`
- `fx2:bypassed` → `g_fx_bypassed[1]`
- `fx3:bypassed` → `g_fx_bypassed[2]`
- `fx4:bypassed` → `g_fx_bypassed[3]`

Get returns `"1"` or `"0"`. Set parses `atoi(val)`. No clamping outside [0,1].

**Step 3: Reset on unload.** Find `unload_synth`, `unload_all_audio_fx`, and the midi-fx unload. Reset the corresponding `*_bypassed` flag to 0 inside each. This prevents stale bypass state when a slot's component is swapped.

**Step 4: Build only.** Run:

```bash
./scripts/build.sh
```

Expected: clean build. No deploy yet — there's no observable behavior to verify.

**Step 5: Commit.**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "chain_host: add bypass flags (storage only, no skip logic yet)"
```

---

## Task 3: Implement audio FX bypass (render-skip)

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Goal:** When `g_fx_bypassed[i]` is 1, the i-th audio FX is skipped — its input audio passes through to its output unchanged.

**Step 1: Find the FX render loop.** Inside the chain's per-frame processor (the function called from `move_plugin_v2->render_block` or equivalent for v1). The loop processes FX 0..MAX_AUDIO_FX-1 in order, each transforming a stereo buffer in place.

**Step 2: Wrap each FX call with a bypass check.** Pseudocode of the pattern:

```c
for (int i = 0; i < MAX_AUDIO_FX; i++) {
    if (!fx_is_loaded(i)) continue;
    if (g_fx_bypassed[i]) continue;   /* ← NEW: skip processing, leave buffer unchanged */
    /* existing call: fx_render_block(i, buf, frames) or v2 equivalent */
}
```

Because audio FX process the chain's stereo buffer in place, "skip" = leave the buffer alone. The buffer already holds the previous stage's output, which is exactly the "pass through" semantic.

If your read in Task 1 showed FX rendering into a separate buffer that's then mixed/copied, "skip" instead means "copy input to output" — adapt to whichever the code does. **Match what the code is doing; do not invent a new buffer flow.**

**Step 3: Build and deploy.**

```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 4: Verify on hardware.**
- Load a chain with at least one audio FX (e.g., Cloudseed in fx1)
- Confirm audio sounds wet (reverb working)
- From a temporary debug surface: set `fx1:bypassed=1`. Easiest path while no UI exists yet: from shadow UI param editor manually, or via host SHM debug tool, or temporarily add a periodic logger and toggle it by editing the source.

  Simpler alternative: temporarily wire a debug shortcut — e.g., make Step button 1 toggle `g_fx_bypassed[0]` — and use the device, then revert that debug shortcut in Task 6.

- Confirm: audio goes dry. Toggle back, audio goes wet.

**Step 5: Commit.**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "chain_host: skip audio FX render when bypassed (no UI yet)"
```

---

## Task 4: Implement MIDI FX bypass (passthrough)

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Goal:** When `g_midi_fx1_bypassed` is 1, MIDI from upstream reaches the synth unchanged — the MIDI FX neither receives input nor emits output.

**Step 1: Find the MIDI dispatch path.** Two places matter:
1. Where incoming MIDI is delivered to the MIDI FX (per-event)
2. The MIDI FX tick (`v2_tick_midi_fx` ~line 1733) which may emit notes to the synth

**Step 2: Wrap both.** Pattern:

```c
/* Incoming MIDI dispatch */
if (midi_fx_loaded && !g_midi_fx1_bypassed) {
    midi_fx_on_midi(msg, len, source);
} else {
    /* bypassed or not loaded: deliver MIDI directly to synth */
    synth_on_midi(msg, len, source);
}

/* Tick / emit */
if (midi_fx_loaded && !g_midi_fx1_bypassed) {
    v2_tick_midi_fx(inst, frames);
}
```

**Step 3: All-notes-off on toggle (deferred).** Per the design doc, we explicitly chose **not** to send all-notes-off when bypass flips. If the user reports stuck notes during testing, add it then — not now.

**Step 4: Build, deploy, verify.**
- Load a chain: arpeggiator in MIDI FX slot + any synth.
- Play and hold a pad — hear arpeggiation.
- Toggle `midi_fx1:bypassed=1` (same temporary mechanism as Task 3).
- Hear: single sustained note (no arp).
- Toggle off: arp resumes.

**Step 5: Commit.**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "chain_host: bypass MIDI FX (passthrough to synth)"
```

---

## Task 5: Implement synth bypass (render-skip only, MIDI still flows)

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Goal:** When `g_synth_bypassed` is 1, the synth's `render_block` is skipped (output is silence into the FX chain), but `on_midi` is still called so voices age out naturally.

**Step 1: Wrap `synth_render_block` call.** Find the line in the per-frame processor:

```c
synth_render_block(buf, frames);
```

Change to:

```c
if (g_synth_bypassed) {
    /* Silence the synth bus — FX chain still processes (tails ring out from prior audio). */
    memset(buf, 0, frames * 2 * sizeof(int16_t));
} else {
    synth_render_block(buf, frames);
}
```

The choice of `memset` vs "skip and leave the buffer alone" depends on what the buffer holds before the synth runs. If the synth is the *first* producer in the chain (buffer holds residual data from prior frame), `memset` is necessary. If the chain initializes the buffer to zero before each render, "skip" is sufficient. **Match what the existing zero-initialization (or lack thereof) does. Pick the option that produces silence, not garbage.**

**Step 2: Do not wrap `synth_on_midi`.** MIDI continues to flow. This is the deliberate semantic from the design.

**Step 3: Build, deploy, verify.**
- Load a chain: any synth + Cloudseed.
- Play a sustained pad — hear synth + reverb.
- Toggle `synth:bypassed=1`.
- Expected: synth audio cuts immediately. Reverb tail rings out, then silence.
- Hold a new pad: silent (synth is bypassed). Release.
- Toggle off: synth audio returns.

**Step 4: Commit.**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "chain_host: bypass synth render, keep MIDI delivery"
```

---

## Task 6: Shadow UI — track Mute held state and intercept jog-click

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Goal:** When CC 88 (Mute) is held and the user jog-clicks while a non-empty module position is focused in the chain module list, toggle that component's `bypassed` param.

**Step 1: Add a held-state tracker.** Near the other modifier-tracking state at the top of `shadow_ui.js` (search for `shift` held or similar), add:

```javascript
let muteHeld = false;
```

**Step 2: Hook CC 88 in `onMidiMessageInternal`.** Find the main MIDI message handler. CC 88 arrives as `[0xB0, 88, value]`. Note-down ≈ `value > 0`, note-up ≈ `value === 0`.

```javascript
if ((data[0] & 0xF0) === 0xB0 && data[1] === 88) {
    muteHeld = data[2] > 0;
    /* do NOT return; let other handlers see it too (mute may have other meanings) */
}
```

**Step 3: Identify the module-list jog-click handler.** From Task 1's read, you have the line number for "Jog click - select module" (~3525). Find the handler that runs when jog-click fires while focused on a chain slot's module position.

**Step 4: Add the bypass-toggle branch.** At the top of that handler, before the existing "open editor" path:

```javascript
if (muteHeld && hasModuleAtFocus(slot, componentPosition)) {
    const key = bypassKeyForPosition(componentPosition); /* "synth:bypassed", "midi_fx1:bypassed", "fx1:bypassed" ... */
    const cur = parseInt(getSlotParam(slot, key) || "0");
    const next = cur ? 0 : 1;
    setSlotParam(slot, key, String(next));
    announce(next ? "Bypass on" : "Bypass off");
    return; /* consume the click */
}
```

Implement `bypassKeyForPosition` as a small switch on the position enum/string (`"synth"`, `"midi_fx1"`, `"fx1"`, `"fx2"`, `"fx3"`, `"fx4"`).

Implement `hasModuleAtFocus` by reading the existing `*_module` param for that position (`synth_module`, `midi_fx1_module`, `fx1_module`, etc.) and checking it's non-empty.

**Step 5: Add the chain_params entries.** Around `slot:muted` at line 1554, add (alongside, not in place of):

```javascript
{ key: "synth:bypassed",    label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
{ key: "midi_fx1:bypassed", label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
{ key: "fx1:bypassed",      label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
{ key: "fx2:bypassed",      label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
{ key: "fx3:bypassed",      label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
{ key: "fx4:bypassed",      label: "Bypassed", type: "int", min: 0, max: 1, step: 1 },
```

**Step 6: Build, deploy, verify.**
- Open shadow UI, load a chain with synth + Cloudseed.
- Navigate to Cloudseed module position.
- Hold Mute, press jog click. Audio goes dry. Hear "Bypass on" announcement.
- Hold Mute, press jog click again. Audio goes wet. "Bypass off".
- Without holding Mute, jog click opens module editor as before — confirm the shortcut doesn't fire when Mute isn't held.
- Confirm empty-position + Mute+JogClick is a no-op.

**Step 7: Commit.**

```bash
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: Mute+JogClick toggles module bypass"
```

---

## Task 7: Shadow UI — draw 'B' marker

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Goal:** Show a same-height 'B' glyph at any module slot position whose `bypassed` param is 1.

**Step 1: Find the module-list rendering code.** This is where each chain slot's module names are drawn. Each row/column shows the module name and any LFO indicator.

**Step 2: Read bypass per position.** Where the row's text is being prepared:

```javascript
const isBypassed = parseInt(getSlotParam(slot, `${positionKey}:bypassed`) || "0") === 1;
```

**Step 3: Draw the 'B' glyph.** Follow the same primitive used by the existing `~N` LFO indicator at line 12903 (the 4px-high draw style). The glyph for 'B' should be:
- Same height as the existing tilde indicator
- Positioned somewhere it doesn't collide with the LFO `~N` marker (e.g., opposite corner of the module's box, or appended after the name with a 1px gap)
- Visible whether or not the row is the cursor-focused row (don't rely on inverting the cursor highlight)

If the existing LFO indicator code uses hand-rolled pixel data, hand-roll 'B' in the same pattern (~5 pixels wide, ~4-5 pixels tall):

```
##.
#.#
##.
#.#
##.
```

(Or whatever fits the existing glyph-height; trust the implementer's pixel taste here.)

**Step 4: Build, deploy, verify.**
- Bypass an FX (via Task 6's shortcut). Confirm 'B' marker appears at that module's position.
- Toggle off — 'B' disappears.
- Bypass multiple modules — multiple 'B' markers appear.
- Verify no collision with `~N` LFO indicator (try a slot with both an LFO and a bypassed FX).

**Step 5: Commit.**

```bash
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: draw 'B' indicator on bypassed modules"
```

---

## Task 8: Persist bypass in chain patch JSON

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c` (or wherever patch save/load is implemented in the chain module)
- Possibly: `src/modules/chain/ui.js` (if patch I/O lives in JS rather than C)

**Goal:** Patch save writes `"bypassed": true` per component when set. Patch load applies it.

**Step 1: Find patch save and load code.** Grep for patch JSON serialization — likely in `chain_host.c` (look for the patch-save function — probably triggered by record-or-something, or by ui.js). Also check `src/modules/chain/ui.js` near line 1889 where the "Save current state as new patch" comment lives.

**Step 2: Save path.** Where each component (synth, midi_fx, audio_fx[i]) is serialized into the patch JSON, append the `bypassed` field if it's 1. Omit if 0 (forward-compat: older host readers ignore it; new ones treat absent as 0). Pattern:

```c
if (g_synth_bypassed) {
    /* append "bypassed": true to the synth's JSON object */
}
```

**Step 3: Load path.** Where the patch JSON is parsed and component params are restored:

```c
int bypassed = 0;
if (json_get_bool(component_json, "bypassed", &bypassed) == 0) {
    /* set the appropriate g_*_bypassed flag */
}
```

Use the existing `json_get_bool` helper (~line 1155) which already exists for this kind of optional bool field.

**Step 4: Build, deploy, verify.**
- Bypass an FX in a chain. Save the patch.
- Reload the patch (switch away and back, or restart shadow UI). Confirm the FX is still bypassed (B marker present, audio dry).
- Edit the patch JSON on disk; confirm `"bypassed": true` is present.
- Load an old patch (one without any `bypassed` field). Confirm nothing is bypassed.

**Step 5: Commit.**

```bash
git add src/modules/chain/dsp/chain_host.c src/modules/chain/ui.js
git commit -m "chain: persist module bypass in patch JSON"
```

---

## Task 9: Master FX bypass — storage and render-skip

**Files:**
- Modify: `src/host/shadow_chain_mgmt.c`, `src/host/shadow_chain_mgmt.h`
- Possibly: `src/schwung_shim.c` if Master FX rendering happens in the shim

**Goal:** Apply the same bypass semantics to each Master FX slot.

**Step 1: Read Master FX structure.** Find where Master FX slots are stored (per-slot plugin handle, per-slot params). Confirm slot count.

**Step 2: Add per-slot bypass field.** In the slot struct, add an `int bypassed;` field. Initialize to 0.

**Step 3: Wire param routing.** Add handlers for `mfx<N>:bypassed` (or whatever naming the existing Master FX param routing uses — match it). Pattern mirrors Task 2.

**Step 4: Wrap render call.** In the Master FX render loop, skip the slot's render when bypassed (same passthrough semantic as audio FX in chain).

**Step 5: Add chain_params entries in shadow_ui.js for Master FX bypass keys** (analogous to Task 6 Step 5).

**Step 6: Extend the Mute+JogClick handler** in Task 6 to recognize Master FX module-list positions and use the corresponding `mfx<N>:bypassed` keys.

**Step 7: Extend the 'B' marker rendering** (Task 7) to cover Master FX module list.

**Step 8: Build, deploy, verify.**
- Open Master FX (Shift+Vol+Menu).
- Bypass a Master FX slot via Mute+JogClick. Confirm audio passes through unchanged.
- Toggle off; confirm FX engages again.

**Step 9: Commit.**

```bash
git add src/host/shadow_chain_mgmt.c src/host/shadow_chain_mgmt.h src/shadow/shadow_ui.js
git commit -m "master_fx: add per-slot bypass with same Mute+JogClick UX"
```

---

## Task 10: Master FX patch persistence

**Files:**
- Modify: wherever Master FX patches/configs are saved and loaded (`src/host/shadow_chain_mgmt.c` or related)

**Goal:** Master FX bypass survives reload.

**Step 1: Same pattern as Task 8** but for the Master FX patch/config format.

**Step 2: Build, deploy, verify.**
- Bypass a Master FX slot, save Master FX state (if it's auto-saved, just wait or trigger explicit save).
- Reboot / restart shadow UI.
- Confirm bypass state restored.

**Step 3: Commit.**

```bash
git add src/host/shadow_chain_mgmt.c
git commit -m "master_fx: persist bypass across save/load"
```

---

## Task 11: Autosave coverage

**Files:**
- Inspect: `src/shadow/shadow_ui.js` and any related autosave code

**Goal:** Bypass state survives an unexpected reboot via autosave, not just explicit save.

**Step 1: Find the slot autosave path** (`autosaveSuppressUntil` and friends in `shadow_ui.js`, and the C-side autosave snapshot).

**Step 2: Confirm bypass is included.** If autosave writes the full patch JSON for each slot, bypass should already be picked up via Task 8's changes — but verify. If autosave writes a separate state file (volumes, channels, mute, solo per-set), add bypass there too (matching `slot:muted` in `chain config (volumes, channels, mute/solo)` at line 3740-3754).

**Step 3: Verify.** Bypass an FX, force-kill the shim, restart. Confirm bypass restored.

**Step 4: Commit (if any code changed).**

```bash
git add <files>
git commit -m "autosave: include module bypass state"
```

If autosave already covered it via Task 8, skip this commit and note in the task tracker that Task 11 was a no-op.

---

## Task 12: Documentation

**Files:**
- Modify: `CLAUDE.md` (Shadow Mode Shortcuts section), `MANUAL.md`, `docs/MODULES.md` if applicable

**Goal:** Document the new shortcut so the next dev/user finds it.

**Step 1: `CLAUDE.md` — Shadow Mode Shortcuts** (search for "Shift+Vol+Track 1-4" — that's the right section):

```markdown
- **Mute+Jog Click** (on focused module position in chain or Master FX): Toggle bypass for that module. Audio passes through unchanged; MIDI FX become passthrough; synth render is skipped while MIDI still flows (FX tails ring out).
```

**Step 2: `MANUAL.md`** — add a user-facing equivalent in the shortcuts list.

**Step 3: Commit.**

```bash
git add CLAUDE.md MANUAL.md docs/MODULES.md
git commit -m "docs: document Mute+JogClick module bypass shortcut"
```

---

## Task 13: Full-flow manual test

**Goal:** Confirm the full feature works end-to-end before declaring done.

**Test scenarios on hardware:**

1. **Audio FX bypass** — Load chain with synth + Cloudseed in fx1. Mute+JogClick on Cloudseed → dry. Again → wet.
2. **Synth bypass with tail** — Cloudseed loaded. Hold a sustained pad. Mute+JogClick on synth. Confirm synth output cuts immediately; reverb tail rings naturally.
3. **MIDI FX bypass** — Arp in midi_fx1, synth loaded. Hold a pad → arp plays. Mute+JogClick on arp → single sustained note. Again → arp resumes.
4. **Multiple bypasses** — Bypass synth + an FX simultaneously. Confirm chain is silent. Unbypass each and confirm audio returns to normal.
5. **'B' markers** — Each bypass shows 'B' at the right position. Multiple bypasses → multiple Bs. No collision with `~N` LFO marker (set up an LFO on a bypassed FX's param to verify).
6. **Persistence — chain** — Bypass FX, save patch, reload patch. Bypass restored.
7. **Persistence — Master FX** — Same, in Master FX.
8. **Autosave** — Bypass, kill shim, restart. Bypass restored.
9. **Empty-position safety** — Navigate to an empty FX slot, Mute+JogClick. Confirm no-op.
10. **No-mute click** — Confirm jog-click without Mute opens editor as before (no regression).

If any scenario fails: open the relevant task, fix, re-deploy, re-verify, re-commit.

---

## Out of scope for this plan

- All-notes-off on MIDI FX bypass toggle (deferred per design)
- LED indication of bypass (visual is on-screen only)
- Bypass-all shortcut
- Animations / transition effects

---

## Final commit and merge

After all tasks pass Task 13:

```bash
git log --oneline ed1c92ac..HEAD   # review the commit chain
```

Then use the superpowers:finishing-a-development-branch skill to decide between merge, PR, or further review.
