# Scene Morphing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Octatrack-style scenes: 16 per set, locks across 4 slots + Master FX + 2 send buses, one A↔B crossfader (jog on a Scenes screen, or a learned CC).

**Architecture:** A header-only evaluator (`scene_morph.h`) owns the table, the verbs and the morph formula. The chain host (`chain_scene.c`) projects each slot's locks into `chain_mod` as a MORPH contribution resolved against the live base; the shim (`shadow_scene_bus.c`) does the same for the Master FX and send buses by writing plugin params. Fader / A / B / arm live in `shadow_control_t`, forwarded to each chain through a dlsym'd export every frame. JS draws the Scenes screen, the arm badge, and persists the DSP-held bank per set.

**Tech Stack:** C11 (chain DSP + shim, SPI-callback RT rules), QuickJS ES modules, bash/node host tests.

**User decisions (already made):**
- Scope: global — 4 slots + Master FX + Send A/B.
- Crossfader: Scenes screen (jog) + learnable external CC.
- Lock gesture: latched scene-edit arm.
- Interpolation runs DSP-side (approach A).
- Base read live; enums switch at 0.5; swapped module → locks dormant.
- While armed, a knob shows the lock (a read answers what a write would change) — "It's fine".
- Fader CC global, not per set (see Deviation below).
- Snapshot/recall stays separate.
- Autonomy: "Work without me. Move is connected. You can make noise on it and delete or create anything."

**Deviation recorded during planning:** a generic per-set CC Map (`src/shared/cc_map.mjs`, learn + shim claim/swallow) already exists. The fader CC is implemented as a CC-Map **setting target** `scenes:xfade` instead of a second, shim-side CC path with a features.json key. Consequence: the binding is per set, like every other CC binding. Reported to the user for a decision; switching to global later is a storage change only.

**Planning finding:** the MFX LFO base is tracked live on user writes (`mfx_lfo_update_base_from_set_param`), snapshotted once on engage. The bus scene engine copies that shape.

---

## File map

| File | Responsibility |
|---|---|
| `src/host/scene_morph.h` (new) | Types, verbs (lock/unlock/clear/copy/load/dump), morph formula, edit-key filter. Pure, header-only. |
| `src/modules/chain/dsp/chain_scene.c` (new) | Per-slot table on the instance; apply into chain_mod; armed redirect; armed read. |
| `src/modules/chain/dsp/chain_internal.h` | `MAX_MOD_TARGETS` 64; `is_morph` contribution fields; scene fields on instance; decls. |
| `src/modules/chain/dsp/chain_mod.c` | Resolve morph contributions in `chain_mod_recompute_effective`. |
| `src/modules/chain/dsp/chain_host.c` | Route `scenes:*`; call redirect/read hooks; call apply in `lfo_tick`; export `chain_set_scene_morph`. |
| `src/modules/chain/dsp/chain_reorder.c` | Re-aim scene pairs on permutation. |
| `src/host/shadow_constants.h` | Appended control fields + `JUMP_TO_SCENES` ext flag. |
| `src/host/shadow_scene_bus.{c,h}` (new) | MFX/send scene tables, verbs, tick, redirect, base tracking. |
| `src/host/shadow_chain_mgmt.{c,h}` | dlsym the export; route `master_fx:scenes:*` / `send<N>:scenes:*`; call bus hooks. |
| `src/schwung_shim.c` | Slew fader; forward per slot; bus tick; Shift+Vol+Step3. |
| `src/shadow/shadow_ui.c` | JS bindings for the control fields. |
| `src/shared/scene_doc.mjs` (new) | Pure: dump parse, document build/parse, load lines. |
| `src/shadow/shadow_ui_scenes.mjs` (new) | Scenes screen draw + input. |
| `src/shadow/shadow_ui.js` | View wiring, jump flag, badge, Delete-unlock, persistence, CC target. |
| `src/shared/control_target.mjs` | `scenes:xfade` master setting. |
| tests (new) | `tests/host/test_scene_morph.{c,sh}`, `test_chain_scene.{c,sh}`, `test_scene_bus.{c,sh}`, `test_scene_doc.sh`, `test_scene_control_layout.sh`, `test_scenes_screen_render.sh` |

## Tasks

### Task 1: Pure evaluator + verbs (`scene_morph.h`)
- [ ] Write `tests/host/test_scene_morph.c` covering: lerp both/A-only/B-only/neither; enum at 0.49/0.5; int rounding; A==B; A none; lock/unlock/clear/copy; cap refusal; load all-or-nothing; dump→load byte-exact; edit-key filter.
- [ ] Implement; `bash tests/host/test_scene_morph.sh` passes. Commit.

### Task 2: Chain side (`chain_scene.c` + chain_mod morph)
- [ ] `test_chain_scene.c` against the real chain_mod.c with a fake synth (pattern: `test_chain_mod_override.c`): morph reaches the module; base write mid-morph moves the unlocked end; LFO offset sums; dormant on module mismatch; armed write redirects and is heard at 100%; armed read answers the lock; structural keys not redirected; clear restores base.
- [ ] Implement, wire into chain_host.c (route, lfo_tick apply, export), chain_reorder re-aim. Existing chain_mod tests still pass. Commit.

### Task 3: Control fields + shim forwarding + JS bindings
- [ ] Append `scene_a, scene_b, scene_edit, scene_flash, scene_xfade_q, scene_rev` to `shadow_control_t`; layout test.
- [ ] Shim: slew, call `shadow_chain_set_scene_morph` per active slot each frame; export reads flash/rev back.
- [ ] shadow_ui.c bindings: `shadow_get_scene_state()`, `shadow_set_scene_ab(a,b)`, `shadow_set_scene_xfade(x)`, `shadow_set_scene_edit(n)`. Commit.

### Task 4: Bus scenes (MFX + sends)
- [ ] `test_scene_bus.c` with fake master_fx_slot_t plugins.
- [ ] Implement `shadow_scene_bus.c`; route verbs and redirect in the param handler and `shadow_direct_set_param`; tick beside the MFX LFO tick. Commit.

### Task 5: Shortcut
- [ ] Shift+Vol+Step 3 / Shift+hold Step 3 raise `JUMP_TO_SCENES`. Commit.

### Task 6: JS — Scenes screen, arm badge, Delete-unlock, CC target
- [ ] `scene_doc.mjs` + node test.
- [ ] `shadow_ui_scenes.mjs` screen; render test PNG inspected.
- [ ] Wire view, jump flag, MFX menu row, badge overlay, Delete+knob unlock, `scenes:xfade` target. Commit.

### Task 7: Persistence
- [ ] Save on `scene_rev` change, abort on any null read; load after restore with `scenes:count` read-back, retry, never rewrite unconfirmed. Commit.

### Task 8: Docs
- [ ] SHADOW_UI.md, CHAIN.md, CLAUDE.md bullet, API.md, help_content.json, manual.html.

### Task 9: Build, deploy, hardware pass (spec §6 list, one check per guard), PR.
