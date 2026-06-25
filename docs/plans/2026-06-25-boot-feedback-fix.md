# Boot-Feedback Fix — Investigation & Plan (2026-06-25)

Branch: `claude/schwung-feedback-issue-2jmyk1`

## Bug report

A user updated to v0.11.3 and booted into massive audio feedback. Slot 2
(receive track 4) had a `linein` (Line In) sound generator + a freeverb that
they say they **never configured**. Isolated to this one user (not a fleet-wide
default).

## Two root causes

### A. Stale global config propagated by the per-set migration

- `shadow_batch_migrate_sets()` — `src/host/shadow_set_pages.c:106-176` — runs
  **once per device** (gated by the `set_state/.migrated` marker, so it fires
  right after the update that introduced per-set state).
- It copies the **global** `SLOT_STATE_DIR`
  (`/data/UserData/schwung/slot_state/slot_N.json`, defined
  `src/host/shadow_set_pages.h:24`) into **every** per-set directory —
  `shadow_set_pages.c:140-149`:
  ```c
  snprintf(src, sizeof(src), SLOT_STATE_DIR "/slot_%d.json", i);
  snprintf(dst, sizeof(dst), "%s/slot_%d.json", set_dir, i);
  shadow_copy_file(src, dst);
  ```
- The user's pre-per-set global slot_2 held linein+freeverb, so the migration
  stamped it into all sets; it now reappears on every boot.
- Nothing **fabricates** the config and nothing ships a default `slot_state`.
  `src/patches/linein_freeverb.json` is a sample patch and is **not auto-loaded**
  (no code references it; confirmed via grep). The cleanup doc
  (`docs/plans/2026-06-11-codebase-cleanup-review.md:157`) calls it a "live
  default" but no loader exists.
- "Isolated to one user" is the signature of *their own* global state
  propagating — other users whose global state never held an `audio_in` slot
  have nothing to propagate.

**Decision (Fix A): the global default should always be empty.** Change the
migration to seed empty `{}` slot/master_fx files into each set (mirror the JS
"new set" path at `shadow_ui.js:14159-14162`) instead of copying module content
from `SLOT_STATE_DIR`. Stale global config must not propagate.

User-device cleanup (not a code fix): delete
`/data/UserData/schwung/slot_state/slot_2.json` and the already-migrated
`set_state/<uuid>/slot_2.json` copies. That `slot_2.json` is also the proof of
origin if the user can send it.

### B. Boot restore bypasses the feedback gate

- The feedback gate lives in `src/shared/feedback_gate.mjs`:
  - `consumesLineInput(meta)` (lines 35-42) = `capabilities.audio_in === true`
    AND `component_type` ∉ {`audio_fx`,`midi_fx`}.
  - `feedbackRiskPresent()` (47-53) = `host_speaker_active() && !host_line_in_connected()`.
  - `maybeConfirmForModule(meta, cb)` (76-80), `confirmLineInput` (64-71),
    `feedbackGateActive()` (85-87) — callback-based, not Promise.
- It is **only** called on interactive selection: `shadow_ui.js:6554`
  (component picker) and `:11757` (tool launch).
- **Cold-boot restore is the C path** in the shim, never the JS `SET_CHANGED`
  loop (that fires on set *switches*): `shadow_chain_init()` /
  `src/host/shadow_chain_mgmt.c:1063-1097` loads each slot via
  `set_param(instance,"load_file",path)` then sets `active=1; fade.target=1.0f`
  HOT (lines 1080-1083). No gate runs → a restored `linein` slot pulls
  line-in/mic to speakers on boot; freeverb adds a wet feedback path.
- Jack state in the shim is `static` and **unknown at boot**:
  `shadow_speaker_active`/`shadow_line_in_connected` + `_known` flags at
  `schwung_shim.c:171-174` start `known=0`, default `speaker_active=1`,
  `line_in=0`. So C cannot reliably evaluate risk at restore time.

**Decision (Fix B): "mute on boot, auto-clear when safe."** The shim brings any
line-input-consuming slot up **muted** on boot; the JS shadow_ui (which polls
reliable jack state and owns the modal) un-mutes once it confirms no risk
(headphones or line-in cable present) or the user acknowledges. Preserves
intentional line-in-on-boot workflows when there's no risk; zero feedback window.

## Integration map (file:line)

| Concern | Location | Notes |
|---|---|---|
| Migration copy → make empty | `src/host/shadow_set_pages.c:140-149` | Seed `{}` instead of copying `SLOT_STATE_DIR` |
| "New set" empty-seed reference | `src/shadow/shadow_ui.js:14159-14162` | Pattern to mirror |
| C boot restore (hot activate) | `src/host/shadow_chain_mgmt.c:1063-1097` | Where to gate; query new param after `load_file` |
| Existing capability parse (synth load) | `src/modules/chain/dsp/chain_host.c:503-533` | Add `audio_in` + `component_type` parse here |
| `synth:` get_param handler | `src/modules/chain/dsp/chain_host.c:1485-1546` | Add `synth:consumes_line_input` key |
| Instance struct (add field) | `src/modules/chain/dsp/chain_internal.h:236-237` | `int synth_consumes_line_input;` |
| JSON helpers | `chain_json.c`: `json_get_int_in_section` (137), `json_get_string_in_section` (117), `json_get_string` (10) | For audio_in (int) + component_type (string) |
| Slot struct (add field) | `src/host/shadow_chain_types.h:32-45` | `int feedback_hold;` |
| Mute semantics | `src/host/shadow_chain_mgmt.h:138` `shadow_effective_volume()` | Returns 0 when `muted` — silences linein (confirmed at `schwung_shim.c:1662,1679`) |
| Mute apply | `src/host/shadow_chain_mgmt.c:612-621` `shadow_apply_mute()` | Reuse for unmute |
| Slot param get/set | `src/host/shadow_chain_mgmt.c:1571-1655` | `slot:muted` (1580/1633); add `slot:feedback_hold` get + clear-set |
| JS slot params | `shadow_ui.js`: `getSlotParam` (2567), `setSlotParam` (2594), `setSlotParamWithTimeout` (2617) | |
| JS module id → meta | `getSlotParam(slot,"synth_module")` (`shadow_ui.js:2530`) → `host_get_module_metadata(id)` (used at `:6547`) | Run `consumesLineInput` in JS |
| JS slot signature refresh | `refreshSlotModuleSignature` (`shadow_ui.js:2539`) | Reconcile hook point |

## Implementation steps

**Fix A**
1. In `shadow_batch_migrate_sets()`, replace the per-slot `shadow_copy_file`
   from `SLOT_STATE_DIR` with writing empty `{}\n` to each
   `set_dir/slot_N.json` and `set_dir/master_fx_N.json` (and skip / empty the
   chain-config copy as appropriate). Net effect: migration seeds empty per-set
   state; the global default never carries module config.

**Fix B**
1. `chain_host.c` synth-load (~503-533): parse `capabilities.audio_in` and
   `component_type`; compute `inst->synth_consumes_line_input` (audio_in true
   AND component_type ∉ {audio_fx,midi_fx} — for a synth slot, audio_in alone is
   effectively sufficient but match the JS predicate). Add the field to
   `chain_internal.h`.
2. `chain_host.c:1485-1546`: add `synth:consumes_line_input` → "0"/"1".
3. `shadow_chain_types.h`: add `int feedback_hold;` to the slot struct.
4. `shadow_chain_mgmt.c:1079-1097`: after `load_file`, query
   `synth:consumes_line_input`; if true, set `muted=1` + `feedback_hold=1`
   instead of leaving the slot hot. (Do **not** read jack state in C — hold by
   default; JS clears.)
5. `shadow_chain_mgmt.c:~1571-1655`: add `slot:feedback_hold` get and a set that
   clears the flag (and unmutes, reusing `shadow_apply_mute`).
6. `shadow_ui.js` reconcile (tick / `refreshSlotModuleSignature` path): for each
   slot with `slot:feedback_hold==1`, fetch meta via `synth_module` +
   `host_get_module_metadata`, run `consumesLineInput`. If `!feedbackRiskPresent()`
   → clear hold + unmute. If risk present → `maybeConfirmForModule`/`confirmLineInput`
   modal; on confirm clear+unmute, on decline leave muted.

## RT / build / release notes

- `shadow_chain_init` runs at init, **not** the SPI callback — file I/O there is
  fine. Keep no file I/O/alloc/locks out of the callback path (CLAUDE.md
  Realtime Safety).
- Log prefixes: `mm:` / `host:` / `shim:`.
- Build: `./scripts/build.sh`. Tests:
  `for t in tests/{host,shadow,store,build}/*.sh; do bash "$t"; done`.
- Docs to update (Release Checklist): `CLAUDE.md` Feedback Protection section
  (note boot-time gate + "global default always empty" migration behavior),
  `docs/plans/2026-04-30-feedback-protection-design.md`, and
  `../schwung-catalog-site/manual.html`.
- Commit + `git push -u origin claude/schwung-feedback-issue-2jmyk1`. No PR
  unless asked.

## Status

Investigation only — **no code changed**. Was mid-read of `chain_host.c`
synth-load (503-533) and the `synth:` get_param handler (1485-1546), plus
`chain_internal.h` (232-256) and `shadow_chain_types.h` (32-45) for where to add
fields.
