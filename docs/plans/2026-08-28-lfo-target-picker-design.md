# LFO target picker: group by level, and land on what it already points at

2026-08-28

## What is wrong

`enterLfoTargetPicker` / `enterLfoTargetParamPicker` (`src/shadow/shadow_ui.js`)
build two flat lists and hard-reset the cursor to 0 on each entry.

Measured against `tests/fixtures/module-contracts.json` (95 modules):

| | |
|---|---|
| minijv modulatable params | **418** |
| surge / forge / mrdrums | 303 / 250 / 231 |
| median module | 18 |
| modules publishing `ui_hierarchy.levels` | **84 of 95** |

Two separate costs:

1. **The list is unstructured.** `lfoTargetParamsFor` flattens the component's
   whole `chain_params` into one run. The author's own named levels — "Patch",
   "Perf", "Voice", "Mix", "FX" — are right there in `ui_hierarchy`, and the
   knob grid uses them. The picker throws them away.
2. **The cursor forgets.** Re-aiming an LFO already routed at param #143 costs
   the same 143 jog steps as the first time, and the answer is already stored
   in `target` / `target_param`.

## Shape

Component -> **group** -> param. One new step, never more: nested levels are
flattened to a one-deep list, so minijv is 55 group rows rather than a 5-deep
tree or 418 params.

## `src/shared/lfo_target_groups.mjs` (new, pure)

```js
groupLfoTargetParams({ hierarchy, chainParams, minToGroup })
  -> { grouped, flat, groups: [{ key, label, params: [{key,label}] }] }
```

- `flat` is exactly the list the picker offers today: `chain_params` order,
  types `float | int | enum | wav_position`. That allowlist moves here from
  `lfoTargetParamsFor`; `wav_position` is in it for the reason recorded there.
- Labels come from `chain_params` (`name || label || key`), not from the
  hierarchy, so nothing gets renamed by this change.
- The walk follows the same two edges `page_plan.mjs` follows — a `params`
  entry naming a `level`, and `children` — from `root`, or from every entry in
  `modes` when the module declares them (minijv has no `root`), or from the
  first level.
- Group names come from `page_plan.mjs`'s own namer, so **a group is called
  what the corresponding grid page is called**, including the 6-char
  `Prefix/Base` form for depth >= 2. This is the point of the feature; a second
  copy of the naming rule would defeat it.
- A param belongs to the **first** group that lists it (knobs then params, in
  declaration order). Groups are disjoint.
- **Orphan sweep -> "Other".** Every modulatable `chain_param` no level claimed
  lands in a trailing group. The union of the groups is byte-for-byte `flat`.
  Grouping must never cost you a target that the flat list offers today.
- `grouped: false` — the caller skips the group step entirely — when there is
  no usable hierarchy (11 of 95 modules), when `flat.length <= minToGroup`
  (default 8: a menu level over 6 params is a regression), or when the walk
  yields one group. LFO-to-LFO targets (3 hardcoded params) fall out of this
  for free.

### No mode filter, deliberately

`planPages` drops levels owned by an inactive mode. This does not, so minijv's
picker lists both modes' levels. A routing to a mode-inactive param is still a
valid routing that the DSP will honour, and the orphan sweep would relocate
those params to "Other" anyway — a worse answer than naming the level they
came from. The picker and the grid therefore disagree about minijv's level
list. That is intended.

## Picker changes (`src/shadow/shadow_ui.js`)

- New `VIEWS.LFO_TARGET_GROUP` between COMPONENT and PARAM.
- `enterLfoTargetParamPicker` reads `ui_hierarchy` (via the existing
  `chainTargetHierarchy`) alongside the `chain_params` read it already makes,
  and calls the grouper. Two IPC reads instead of one, once, on entry — not a
  draw path.
- Back: PARAM -> GROUP -> COMPONENT -> LFO_EDIT. `returnToSlotGridFromLfoTarget`
  is unchanged.
- Screen-reader announce at each step, as the two existing steps do.

## Seeding

The three `= 0` resets are replaced by a lookup of the LFO's stored
`target` / `target_param`: select the matching component, then the group
containing that param, then the param. Unrouted, or a stale key no longer
offered, falls back to index 0 exactly as now. **No new persisted state**, so
there is nothing that can go stale or need a heal.

## `null` is not "no hierarchy"

`chainTargetHierarchy` collapses a failed read and a genuine absence to `null`
— the granny bug. Nothing here caches (the list is rebuilt on every entry), so
the blast radius is one wrong-shaped menu rather than a latched plan, but the
call site branches on the raw string before parsing and retries once before
falling back to the flat list.

## Tests

`tests/host/test_lfo_target_groups.sh`, node over the 95-module fixture:

- **Losslessness**: for every module, the union of the groups equals `flat`,
  same keys, same labels, no duplicates. Mutated to prove it can fail.
- `grouped: false` for the 11 hierarchy-less modules, for a short list, and for
  a one-group walk.
- Group names match the names `planPages` gives the corresponding pages.
- minijv reaches both modes' levels.
- Seeding: a stored target resolves to the right group and param index; an
  unknown key falls back to 0 without throwing.

## Docs

`docs/PARAM_PAGES.md` gets the grouping rule and the losslessness invariant;
one bullet in `CLAUDE.md`'s hook for it. `manual.html` gets the new step.
No new setting and no changed gesture elsewhere.
