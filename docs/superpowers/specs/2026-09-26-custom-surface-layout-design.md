# Custom surface layout — design (and the control foundation it shares with CC mapping)

Status: agreed 2026-09-26. Steps 1–4 built (control_target, control_map,
layout_custom, control_host + wiring); editor and web editor next. A third navigation layout beside
Map and Knobs (`src/shared/layout_common.mjs`), for both the OXI E16 and the
Faderfox EC4. **Generic CC assignment is built NEXT on the same foundation**,
so this document defines that foundation and the ownership rules that keep
the two from colliding; the CC map's own UI is a later document.

## What the user gets

- **Global Settings → Surfaces → Surface Nav** gains **Custom** (per device,
  like the other two). Surface Nav and Follow Focus are shown only when
  Surface is E16 or EC4; under **CC Only** there is no screen to navigate.
- A custom layout is **up to 16 pages of 16 knobs**. Each knob is assigned to
  one parameter anywhere in the set — a slot module, a Master FX module, a
  slot or master setting. Its page shows the knob's label and value; the ring
  shows the value.
- **Shift hold shows the PAGE MAP** — sixteen cells, one per page, named; the
  current page marked. A push jumps to that page; a push on an empty cell
  creates a page there. This is the Map layout's slot-map gesture with pages
  in place of slots. **Shift + turn** steps pages.
- **Shift tap is the Mixer**, as in every layout.
- **A push performs the parameter's own click**, as on Move's grid: a toggle
  flips, a trigger fires; a continuous parameter does nothing.
- **Learn:** hold a knob's push (≥ 600 ms) → the cell says LEARN. Move a
  parameter on Move (its knob grid, a list, Slot Settings, Master FX Settings)
  → that becomes the knob's target. **While armed, a push on that knob clears
  it**; Shift, or 10 s, cancels. (A Shift + push would collide with the page
  map, which owns pushes while Shift is held.) Move's own track-volume knob is
  NOT learnable: the shim writes `slot:volume` for it without the shadow UI
  seeing a write; learn a slot's volume from Slot Settings instead.
- **A knob whose module has gone goes DARK**: label kept, value "--", no
  writes, until that module is back in that position or the knob is
  reassigned. Never silently drives what now sits there.
- **Per set.** The layout is part of the set, shared by both devices (both are
  4×4). Edited from **Master FX Settings → Surface Layout** (the per-set
  settings screen) and from a **web editor** in schwung-manager.

## Model: ONE per-set control document, two kinds of binding

`set_state/<uuid>/controls.json`:

```json
{ "version": 1,
  "surface": { "pages": [ { "name": "Drums", "knobs": [ <target | null> × 16 ] } ] },
  "cc":      [ { "cable": 2, "channel": 0, "cc": 74, "mode": "abs", "target": <target> } ] }
```

`surface` is this feature. `cc` is the generic CC map, built next — declared
now so the file, the loader and the copy-on-duplicate never need a second
format. A document with no `cc` key, or a `cc` entry this build cannot
interpret, loads with that part empty and is written back **with the part it
did not understand preserved** (the loader keeps unknown top-level keys), so an
older build cannot erase a newer build's bindings.

A target is one of:

| kind | fields | addresses | module check |
|---|---|---|---|
| `param` | `slot`, `component` (`synth`, `fxN`, `midi_fxN`), `key`, `module` | `getSlotParam(slot, "<component>:<key>")` | `<component>_module` of that slot == `module` |
| `master` | `fx` (1–8), `key`, `module` | `shadow_*_param(0, "master_fx:fx<N>:<key>")` | Master FX position N == `module` |
| `setting` | `slot` (or null for master), `key` | slot / master settings keys: `slot:volume`, `slot:pan`, `slot:muted`, `buses:main_send<N>`, `master_fx:filter`, `send<N>:return` | none — always present |

Every target also stores `label` (what the knob was learned as, so a dark knob
can still say what it was for). Ranges, types and options are NOT stored: they
come from the module's live `chain_params`, cached by module id, so a module
update that changes a range is honoured.

Send-bus insert parameters (`send<N>:fx<M>:<key>`) are left out of v1; the
target table has room for them.

## The shared foundation (what CC mapping will reuse unchanged)

- **Targets** (`src/shared/control_target.mjs`): the table above — schema,
  validation, a display label, equality. Pure.
- **The target io** (host): `resolve / metaOf / read / write / click`, module
  checks, `chain_params` cache. Both the surface and the CC map write through
  it, so a parameter is addressed one way however it is driven.
- **The learn broker** (host): ONE learn at a time. An owner arms it (a
  surface knob now, a CC-map row next) and receives the next parameter
  written on Move as a target. Arming a second learn cancels the first.
- **The claim table** — who owns an incoming (cable, channel, CC). See below.

## Ownership: who a control message belongs to

Three sources can drive parameters, and each incoming message has exactly
ONE owner, decided in this order:

1. **An active surface in remote mode** owns its protocol's messages
   (the E16: CC 1–16 and notes 0–16 on channel 1, cable 2 — `e16_claim.h`;
   the EC4 uses the same map). A claimed message never reaches the CC map.
2. **A CC-map binding** owns its (cable, channel, CC).
3. Otherwise the message is **not ours**: it goes on to Move and the slots as
   today.

Consequences, all by construction:

- **The E16 as a plain controller.** With Surface = **CC Only** (the setting's
  first option, formerly "Off"), nothing claims
  CC 1–16, so an E16 in its own (non-remote) mode is just a controller and
  the CC map can bind its knobs. Switching the surface on takes them back,
  and the CC map's bindings for them are silent (not deleted) while it does.
- **CC learn refuses a claimed message**, so a surface knob can never be
  learned as a CC binding and double-fire.
- **A bound CC is swallowed** before Move and the slots see it — otherwise it
  would move the bound parameter AND whatever that CC means to the synth. The
  shim's fixed E16 claim becomes a CLAIM TABLE the shadow UI writes (the
  surface's range plus every bound CC), read where `e16_claims_msg` is read
  today.
- **Move's own knobs are a third, separate source.** Each slot's Knob Mapping
  (Slot Settings) maps Move's knobs, CC 71–78 on CABLE 0, inside the chain.
  External CC is cable 2, so the two cannot meet; the CC map will not offer
  cable 0.
- **Two sources may drive the SAME parameter** (a surface knob and a CC
  binding on one cutoff). That is allowed and coherent: both write one value,
  and the surface's display follows writes it did not make (`noteParamWrite`).

## CC learn is a MODE, parameter first

The first build armed ONE binding and took its two halves (a CC and a
parameter) in either order, from a row in Master FX Settings. It could not be
used on hardware: you had to leave the screen to find the parameter, the
order was guesswork, and every binding meant going back to re-arm. It is now
what a DAW does:

- **CC Map → Learn** turns learn MODE on; it outlives the screen.
- Move a parameter on Move: it becomes the one being learned. The broker is
  one-shot, so the CC map re-arms it after every capture (quietly).
- Move a controller CC: bound. The binding message moves nothing (an
  absolute knob would jump); the next one drives, so you hear it while still
  in learn.
- **One CC per parameter**: a second CC brushed while learning MOVES the
  binding, it never adds a duplicate.
- The **footer of whatever screen is up** says where it is — `LEARN move a
  param`, `LEARN Cutoff > CC?`, `LEARN Cutoff: CC18` — painted after the view
  switch. The footer DROPS a hint that does not fit, so the host measures and
  the NAME gives way, never the CC. A change is noticed in the TICK: a
  controller CC is not input on the screen, and the draw path does not run
  without a redraw.
- It ends by **Stop Learn**, or after two minutes with nothing moved.
- **Shift+Vol+Sample** toggles it from anywhere (`SHADOW_UI_FLAG_CC_LEARN_TOGGLE`).
  Shift+Sample is the sampler's, and the sampler only arms with the Schwung
  screen OFF, so the volume touch is what separates the two; the shim takes
  it before the sampler and swallows both edges.

## Parts of THIS feature

1. **`src/shared/control_map.mjs`** — the document: parse (tolerant: unknown
   kinds become null knobs, never a thrown error; unknown keys preserved),
   serialize, page ops (add, rename, delete, move), assign/clear.
   `control_target.mjs` beside it. Both pure.
2. **`src/shared/layout_custom.mjs`** — the third layout, keeping the layout
   contract: screens `custom` (a page) and `pagemap`, rings, readings,
   `handle`. Reads and writes through an injected **target io**:
   `resolve(target) -> live | dark`, `metaOf(target)`, `read(target)`,
   `write(target, value)`, `click(target)`. Its own value cache, refreshed by
   ONE read per tick in rotation (a full page in ~270 ms at 60 Hz) — never a
   read per cell per frame. A write updates the cache at once, and the host's
   existing `noteParamWrite` feed updates it for writes made on Move.
   Turning goes through `knobStep` with the target's meta, after the device's
   pulse→detent feel, exactly as a module page's knob does.
3. **Device drawing** — E16: `renderCustomPage` (header = page name + n/m,
   sixteen label/value cells, dark cells drawn with a dotted label) and
   `renderPageMap` (renderMap's boxes with page names). EC4: both through
   `screenLabels` + the reading overlay.
4. **Host wiring (`shadow_ui.js`)** — the control store: load on set change
   (`activeSlotStateDir`), save on every edit, reload when the file's content
   changes (≤ 1 Hz check — the web editor writes it), and **copied on set
   duplication** (the copy list is by name; a file not on it is lost). The
   target io: module resolution from the chain shape and `master_fx:modules`,
   `chain_params` per module id (cached, one blocking read on first use).
   The learn broker, which **listens at two writers**: `setSlotParam` (every slot, component
   and Master FX module write from Move's grids and lists) and the Master FX
   Settings io's `writeParam`. Surface Nav gets a third option.
5. **Master FX Settings → Actions → Surface Layout** — a list: pages (add,
   rename, delete, reorder) and each page's sixteen knobs with their targets;
   a knob row clears it.
6. **Web editor** — schwung-manager `/surface-layout`: the active set's layout,
   a picker per knob (slot → module → parameter, from `chain_params` over
   ShmParams), page ops, drag to rearrange; writes the JSON atomically
   (tmp + rename). The shadow UI picks it up on its next content check.

## Rules carried over

- **A read that did not answer is not a value** (tri-state): a cell whose read
  returns null keeps its last value; one never read draws no value.
- **Dark is decided from positive knowledge only**: a module-id read that
  failed leaves the knob as it was, never darkens it — a timed-out read must
  not look like a removed module.
- **Nothing blocking on the draw path**: meta is fetched on resolve (once per
  module id), values in the rotation.
- **The file is the truth; the layout is a view of it.** Every edit is
  load-modify-save, so the device and the web editor cannot silently overwrite
  each other's pages (last writer wins per edit, not per session).

## Build order

1. Targets + the control document + storage, tests.
2. The layout with a fake target io, tests (including dark, learn, page map).
3. Device drawing, render checked by eye.
4. Host wiring + Surface Nav option; an assembly test driving learn end to end
   through a fake setSlotParam. **Hardware test here** — the feature is usable
   with learn alone.
5. Master FX Settings editor.
6. Web editor.
7. Docs (EC4_SURFACE.md, E16_REMOTE.md, manual when the E16 work is closed).

## Open for later

- Separate push assignments (a push that does something other than the
  parameter's own click).
- Send-bus insert targets.
- **The CC map itself (next):** the `cc` section, a CC-learn that listens to
  cable-2 input (refusing claimed messages), absolute / relative modes (and
  pickup for absolute), the claim table in the shim, and its editor — in
  Master FX Settings beside Surface Layout, and in the web editor. #403
  (chain-side CC map, stale since 09-04) is the prior art: its per-page
  labelling and its fixes (`shadow_master_fx_forward_midi` never receiving
  cable-2 CC; menu rows that could not navigate) should be carried over.
