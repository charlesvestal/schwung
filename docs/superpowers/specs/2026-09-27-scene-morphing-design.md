# Scene Morphing (Octatrack-style) — Design

Date: 2026-09-27
Status: approved in brainstorm, awaiting spec review

## Summary

Sixteen **scenes** per set. A scene is a set of parameter **locks** across all
four slots, the Master FX bus and both send buses. Two scenes are assigned to
**A** and **B**, and one **crossfader** morphs every locked parameter between
them. The fader is the jog wheel on a dedicated Scenes screen, plus an optional
learned external MIDI CC. Locks are written by **arming** a scene and turning
knobs anywhere in the shadow UI.

Decisions taken during brainstorming:

| Question | Decision |
|---|---|
| Scope of a scene | Global: 4 slots + Master FX + Send A/B |
| Crossfader | Scenes screen (jog) + learnable external CC |
| Lock gesture | Latched scene-edit arm |
| Where interpolation runs | DSP side (chain host + shim), not JS |
| Base under a lock | Read live, never snapshotted |
| Discrete params | Switch at xfade = 0.5 |
| Swapped module | Its locks go dormant, not deleted |
| Knob display while armed | Shows the lock (a read answers what a write would change) |
| Fader CC binding | Global (features.json), not per set |
| Snapshot / recall | Separate; does not capture scenes |

## 1. Data model and semantics

**Scene bank** — per set, 16 scenes. A scene is a list of **locks**
`{scope, target, param, value, module_id}`:

- `scope`: `slot0`..`slot3`, `mfx`, `send1`, `send2`
- `target`: component key inside the scope (`synth`, `fx2`, `midi_fx1`; for
  MFX/sends, `fx<N>`)
- `param`: the module's `chain_params` key
- `value`: float in the param's own range; enums stored by option index
- `module_id`: the module loaded at `target` when the lock was written

**Morph state** — `scene_a`, `scene_b` (0..15 or none), `xfade` (0.0..1.0).

**Evaluation**, per `(target, param)` locked in A or B:

```
va = A locks it ? A.value : base
vb = B locks it ? B.value : base
float / int : effective = lerp(va, vb, xfade)      (int: rounded)
enum / bool : effective = xfade < 0.5 ? va : vb
```

- `base` is the knob value, read live. Turning a knob mid-morph moves the
  unlocked endpoint.
- Locked in neither A nor B → no contribution at all; nothing is pinned.
- LFO offsets still sum on top of the result.
- A == B → fader has no effect. A = none, B = n → morph from base to scene n.
- **Dormancy**: a lock whose `module_id` differs from the module now loaded at
  `target` is skipped (not applied, not deleted); it wakes if that module
  returns.
- `xfade` is live-only: resets to 0 on set load. A and B persist.

**Budget** — at most **64 distinct `(target, param)` pairs per scope**, counted
across all 16 scenes. A lock past the cap is **refused visibly** (section 4),
never silently dropped.

## 2. DSP evaluation

### Pure evaluator — `src/host/scene_morph.h`

Header-only; the formula above and the table type, nothing else. Consumed by
both the chain host and the shim so the rule exists once
(`transport_grid.h` precedent). Unit-tested directly from `tests/host/`.

```c
#define SCENE_COUNT 16
#define SCENE_MAX_PAIRS 64
#define SCENE_NONE 0xFF

typedef struct {
    char target[16];
    char param[32];
    char module_id[32];
    uint16_t lock_mask;          /* bit n = scene n locks this pair */
    float values[SCENE_COUNT];
} scene_pair_t;

typedef struct {
    int pair_count;
    scene_pair_t pairs[SCENE_MAX_PAIRS];
} scene_table_t;
```

(~10 KB per table.) The evaluator resolves one pair to
`{has_contribution, has_a, a, has_b, b}` given `(a, b)`; the final value is
computed against the live base by the consumer.

### Chain slots — `chain_mod.c`

- `scene_table_t` inline on `chain_instance_t`, allocated with the instance,
  never on the callback.
- New contribution kind **`is_morph`** beside `is_override` in
  `mod_source_contribution_t`, storing the endpoints (`has_a, a, has_b, b`)
  plus the current xfade. `chain_mod`'s effective-value computation resolves
  it against `entry->base_value`, so a knob change needs no re-emit. Offsets
  sum on top, as for overrides.
- **`MAX_MOD_TARGETS` 32 → 64.** Measure `sizeof(chain_instance_t)` before and
  after and record it in the pin test.
- Fader input: **dlsym'd export** `chain_set_scene_morph(void *inst, uint8_t a,
  uint8_t b, float x)` — never a field on `plugin_api_v2_t`. Stores and marks
  dirty; the existing mod pass does the work only when dirty (fader moved, A/B
  changed, table changed, module swapped). Runs on idle frames too via
  `mod:tick`.
- Table edit verbs (`set_param`, SPI callback, no allocation, no JSON):
  - `scenes:lock` = `"<n> <target> <param> <value> <module_id>"`
  - `scenes:unlock` = `"<n> <target> <param>"`
  - `scenes:clear` = `"<n>"`
  - `scenes:copy` = `"<src> <dst>"`
  - `scenes:load` = one lock per line, same format; all-or-nothing (a
    malformed line rejects the whole load)
  - GET `scenes:dump` → same line format; GET `scenes:count` → pair count
- Dormancy decided in the mod pass by comparing `module_id` to the loaded
  module at `target`.
- `chain_reorder.c` permutations re-aim `scene_pair_t.target` exactly as they
  re-aim `mod_targets`.

### Master FX and sends — shim

- One `scene_table_t` per bus (mfx, send1, send2), evaluated beside
  `shadow_master_fx_lfo_tick`, only when dirty.
- Writes via the plugin's `set_param`, rate-limited for int/enum as
  `MOD_INT_ENUM_MIN_INTERVAL_MS`.
- **First plan step**: establish how the MFX LFO obtains base. If it snapshots
  rather than tracks, fix it so the live-base rule holds here too.
- Same verbs, routed through `master_fx_key.h` / `send_fx_key.h`
  (`master_fx:scenes:lock`, `send1:scenes:lock`, …).

### Control fields — `shadow_control_t` (appended)

| Field | Type | Meaning |
|---|---|---|
| `scene_a`, `scene_b` | uint8 | 0..15, 0xFF none |
| `scene_xfade_q` | uint16 | 0..65535 target position |
| `scene_edit` | uint8 | armed scene, 0xFF none |
| `scene_rev` | uint16 | bumped on every lock/unlock/clear/copy/load |
| `scene_flash` | uint8 | last refusal (0 none, 1 FULL, 2 N/A), for the badge |

Writers of `scene_xfade_q`: the UI jog handler and the shim's CC tap. The shim
alone slews it (one-pole, ~15 ms) and forwards `(a, b, x)` to each chain every
frame and to its own bus tables.

**RT**: all of the above runs on the SPI callback — no allocation, logging or
file I/O; bounded to 64 pairs × 7 scopes, on dirty frames only.

## 3. Scenes screen

**Entry**: Shift+Vol+Step 3; Shift+hold Step 3 in long-press mode (gated with
`want_shiftvol || want_longpress` beside Steps 2 and 13); `JUMP_TO_SCENES` in
`ui_flags_ext`. Also a **Scenes** row on the Master FX bus menu.

```
┌ Scenes            A 3  B 7 ┐
│ 1 2 3 4 5 6 7 8 ...16      │  filled cell = scene has locks
│     ▲A        ▲B           │
│ A ▕████████░░░░░░░░░▏ B    │  fader, notch = position
│ A: 5 locks   B: 12 locks   │
│ Jog:fade  Step:A  Sh+Step:B │
└────────────────────────────┘
```

| Input | Action |
|---|---|
| Jog turn | Fader, 1/64 per detent; Shift+jog 1/256 |
| Jog click | Snap fader to the nearer end |
| Step tap | Set scene A |
| Shift+Step | Set scene B |
| Copy held + two steps | Copy scene (source, destination) |
| Delete held + step | Clear scene (one-level undo kept) |
| Hold step 500 ms | Arm that scene for editing (tap armed step again to disarm) |
| Shift+Jog click | Learn external fader CC |
| Back / Menu | Leave; fader stays where it is |

- Steps, Copy and Delete are claimed while the screen is up, via the latched
  both-edge `midi_in_swallow`. Pads are **not** claimed — play while morphing.
- Step LEDs are **not** written in v1 (Move repaints them).
- Screen reader: "Scene A 3", "Fader 40 percent" (throttled), "Scene 5
  cleared", "Scene 3 armed".

## 4. Scene-edit arm

**Decided below the UI.** `scene_edit` in `shadow_control_t`; the chain host
(for slot writes) and the shim (for `master_fx:` / `send<N>:` writes) redirect
the write. A UI-side hook would miss module-drawn grids (9W9's own
`ui_chain.js` io) — the p-lock lesson.

**Redirected** while armed: a write to `<target>:<param>` where `param` is in
that component's `chain_params`, and the key is none of `:state`, `:module`,
`:bypassed`, `:effective`, `fx:*` shape verbs, `slot:*`, `buses:*`, `scenes:*`.
It becomes `scenes:lock <armed> <target> <param> <value> <module_id>`. Preset
loads, restores and shape edits therefore behave normally.

**Audition**: while armed, the armed scene applies at 100% and the fader is
ignored.

**Reads**: while armed, a plain read of a param locked in the armed scene
returns the **lock**; otherwise base. A read answers what a write would change.
Arm/disarm invalidates the grid's value cache.

**Unlock one param**: hold Delete + touch or turn a knob while armed. Delete is
claimed (both edges, latched) for as long as a scene is armed.

**Badge**: inverted `SCN n` top-right on every shadow screen while armed,
painted after the view switch so it lands over module-drawn frames too.

**Disarm**: tap the armed step; set change, reboot and overtake entry always
disarm (session-only).

**Refusal**: budget full or param not in `chain_params` → the write goes to
**base** as before, `scene_flash` is set, and the badge flashes `FULL` / `N/A`.

## 5. Persistence and external CC

**Authority**: the DSP side holds the live bank; JS persists it and holds no
independent model.

**File**: `set_state/<uuid>/scenes.json`

```json
{"v": 1, "a": 3, "b": 7,
 "scenes": [{"n": 3, "locks": [
   {"scope": "slot0", "target": "synth", "param": "cutoff",
    "module": "obxd", "value": 0.42}]}]}
```

**Save**: the autosave pass compares `scene_rev`; only on change does it read
`scenes:dump` from each scope. **Any `null` read aborts the save** — a failed
read is never written as an empty scope.

**Load** (set change, boot): after the chain restore completes, push
`scenes:load` per scope and **read back `scenes:count`** to confirm. An
unconfirmed scope is retried; **`scenes.json` is never deleted or rewritten
while any scope is unconfirmed**. An unknown `v` is refused and left on disk.

**Snapshot / recall** is unchanged and does not capture scenes. A recall moves
base; scenes keep morphing relative to it.

**External fader CC**:

- Learned on the Scenes screen (Shift+Jog click → "Move a fader…" → next
  cable-2 CC binds; Back cancels).
- Stored globally in `features.json` as `scene_fader_ch`, `scene_fader_cc`
  (install.sh preserves unlisted keys).
- Read in `shim_post_transfer`; the bound CC is **swallowed** from both buffers
  via `midi_in_swallow` so it does not also reach a slot synth; writes
  `scene_xfade_q`.
- Not consumed while an overtake module is active.

## 6. Testing

**Host tests (CI, `tests/host/`)**

- `test_scene_morph.c` — lerp with locks at both / A only / B only / neither
  (no contribution); enum switch at exactly 0.5; int rounding; A == B;
  A = none; dormant pair; 64-pair cap refuses.
- `test_scene_verbs.c` — lock/unlock/clear/copy/load/dump round-trip,
  byte-exact; malformed `load` line rejects the whole load.
- `test_scene_edit_redirect.c` — the write filter: plain param redirected;
  structural suffixes and verbs not; non-`chain_params` key goes to base.
- `test_scene_permute.sh` — locks follow their module across `fx:move`.
- Layout pin — scene fields appended; `sizeof(shadow_control_t) <= 256`;
  `MAX_MOD_TARGETS == 64` with measured `chain_instance_t` size.
- `test_scene_persist.sh` — node: `null` read aborts save; unconfirmed load
  never rewrites the file.
- Render tests — Scenes screen; `SCN n` badge over a module-drawn frame. PNGs
  inspected, not only hashed.

**Hardware pass** (one check per guard)

1. Two scenes on a slot synth's cutoff: jog sweep is smooth; a slot LFO still
   modulates on top.
2. Lock at one end only: turning the knob mid-morph moves the unlocked end.
3. Arm on 9W9's own screen: badge shows, lock lands.
4. Armed + preset load: base changes, no lock created.
5. Delete while armed never deletes a Move clip (check `Song.abl` clip count).
6. Master FX and send-bus locks morph.
7. Reboot: scenes and A/B restored, fader at 0; repeat with a slow slot restore
   and confirm the file survives.
8. External CC drives the fader, does not reach a same-channel synth, and is
   released during overtake.

**Docs**: `docs/SHADOW_UI.md` (screen, arm, persistence), `docs/CHAIN.md`
(morph contribution, verbs), hook bullet in `CLAUDE.md`,
`src/shared/help_content.json`, `../schwung-catalog-site/manual.html`.

## Out of scope (v1)

Step-LED scene display; beat-quantized scene changes; more than 64 pairs per
scope; non-linear morph curves; per-scene fade times.

## Delivery

Branch `feat/scene-morphing` (worktree `../schwung-scene-morphing`), one PR.
Order: pure header + tests → chain side → shim (MFX/sends, control fields, CC)
→ JS screen, arm badge, persistence → docs → hardware pass.

## Implementation notes (2026-09-27, as built)

Changes from the brainstormed design, each forced by something found in the
code or on hardware:

- **Scene B is picked with knob 2, not Shift+Step.** The shim hands Shift+step
  to Move (Move's own shortcut vocabulary; it also dismisses the shadow UI), so
  a Shift+Step gesture could never reach the Scenes screen. A step TAP puts the
  scene on the end the fader is NOT at; knobs 1 and 2 pick A and B explicitly.
- **The fader CC is a CC Map target (`scenes:xfade`) and so PER SET**, not a
  global features.json binding. A generic per-set CC Map with learn, shim-side
  claim and swallow already existed; building a second CC path beside it would
  have duplicated all of that. Shift+Click on the Scenes screen starts CC learn
  with the fader chosen. Revisit if a global binding is wanted.
- **Delete-to-unlock is decided below the UI** (`scene_unlock`), for the same
  reason as the arm; it takes a knob TURN (a touch writes nothing).
- **A `state` read saves the knob, not the morph** -- not in the design; found
  on hardware (a reboot restored the morphed value as the knob).
- **Steps are claimed on the Scenes screen** through a new `step_claim` byte
  riding the existing p-lock step withhold, so no tap is replayed to Move.
- **Undo on the Scenes screen** (one level, swaps: again = redo).
- **A shadow_ui restart ADOPTS the live bank** instead of reloading the file.

Hardware-verified (2026-09-27, device on this branch): slot morph at the
destination (the module's own state blob) at x = 0 / 0.5 / 1; one-ended morph
following a live knob; neither-end release; arm + audition + lock read; Delete
unlock; state read saves the knob; Master FX verbs, base read, arm, unlock,
state read; send verbs; save debounce; Move restart restores bank, A/B, fader
at 0; shadow_ui kill adopts an unsaved edit; the Scenes screen, shortcut,
jog, step tap, knob 2, hold-to-arm, Copy, the SCN badge over another screen.

Not verified on hardware: a set SWITCH (save-old / load-new); the CC Map learn
of the fader with a real controller; audible smoothness of a sweep; that a
Delete while armed cannot reach Move (the positive control -- an unarmed Delete
on the same screen -- did not delete a clip either, so the test could not
tell); Master FX morph at the plugin (covered by the unit test only, since
every param-channel read of a driven bus param deliberately answers the base).
