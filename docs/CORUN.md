# Co-run

Co-run lets an overtake tool share Move's control surface with a second UI for
the duration of a single user-driven session — e.g. a sequencer keeps the
pads + steps + transport while the Schwung chain editor takes the OLED + jog,
or while the Move firmware's native preset/synth editor takes its knobs.

Two co-run targets ship today:

| Target                    | Peer UI                                 |
| ------------------------- | --------------------------------------- |
| `CORUN_TARGET_CHAIN_EDIT` | Schwung shadow_ui's chain-slot editor   |
| `CORUN_TARGET_MOVE_NATIVE`| Move firmware's native preset / synth editor |

## Ownership model

Every physically distinct Move control is classified into a `CORUN_GRP_*` group,
so a tool can keep or cede **any** input. There are two ways to declare the split:

- **Cede model (recommended).** The tool **keeps the whole surface by default** and
  lists only the groups it **cedes** to the peer, via
  `shadow_corun_begin_cede(target, id, cede_mask)`. Keep-by-default means a control
  the tool doesn't mention stays with the tool — the friendly default, and the one
  to use for new tools.
- **Legacy keep-list model.** `shadow_corun_begin(target, id, keep_mask)` — the tool
  lists what it **keeps**; everything else classified cedes. `keep_mask == 0` uses
  `CORUN_KEEP_DEFAULT = PADS | STEPS | TRANSPORT | MENU | MUTE`. Preserved
  byte-for-byte for existing tools; a tool on this path also keeps the extended
  buttons (transport/edit/nav) it never named, exactly as before they were classified.

Both produce identical routing — the cede mask is just stored internally as its
keep complement, so one representation drives every routing/LED site. Back is
framework-reserved as the exit gesture by default; see [Exit gesture](#exit-gesture).

### Control-surface groups

| Group | Control | Group | Control |
| ----- | ------- | ----- | ------- |
| `CORUN_GRP_OLED`  | the screen        | `CORUN_GRP_MUTE`    | Mute (CC 88) |
| `CORUN_GRP_PADS`  | 32 pads           | `CORUN_GRP_PLAY`    | Play (CC 85) |
| `CORUN_GRP_STEPS` | 16 step buttons   | `CORUN_GRP_REC`     | Rec (CC 86) |
| `CORUN_GRP_JOG`   | jog turn + click  | `CORUN_GRP_SAMPLE`  | Record/Sample (CC 118) |
| `CORUN_GRP_TRACK_BUTTONS` | rows CC 40-43 | `CORUN_GRP_LOOP`  | Loop (CC 58) |
| `CORUN_GRP_KNOBS` | knobs CC 71-78    | `CORUN_GRP_COPY`    | Copy (CC 60) |
| `CORUN_GRP_MASTER`| master CC 79      | `CORUN_GRP_DELETE`  | Delete (CC 119) |
| `CORUN_GRP_SHIFT` | Shift (CC 49)     | `CORUN_GRP_UNDO`    | Undo (CC 56) |
| `CORUN_GRP_BACK`  | Back (CC 51)      | `CORUN_GRP_CAPTURE` | Capture (CC 52) |
| `CORUN_GRP_MENU`  | Menu (CC 50)      | `CORUN_GRP_NAV_UP/DOWN/LEFT/RIGHT` | arrows CC 55/54/62/63 |
| `CORUN_GRP_TOUCH` | capacitive touch notes 0-9 | | |

Convenience composites: `CORUN_GRP_TRANSPORT` (= Play|Rec|Sample|Loop),
`CORUN_GRP_NAV` (the 4 arrows), `CORUN_GRP_EDIT` (Copy|Delete|Undo|Capture).
The mic/speaker plug-detect CCs (114/115) are sensor state, not routable buttons,
so they stay unclassified and always with the tool. The power button is not a
routable MIDI event and is out of scope.

## Exit gesture

**Back exits co-run by default.** This matches the rest of Move's "Back pops
one layer" semantics. The framework intercepts Back from the user, calls
`shadow_corun_end()`, and never lets the event reach either the tool or the
peer. (`menu` keeps its existing duties outside co-run: tap-dismisses shadow
UI; Shift+Vol+Menu opens Master FX. While a co-run is active, Menu is
forwarded to the tool by default via `CORUN_KEEP_DEFAULT` so the tool can use
it as its own affordance.)

### Opting out of framework Back

Tools that need Back free for in-session peer navigation (sub-view pop in
the chain editor, native back-navigation inside Move firmware) signal it — and
Back then routes per the normal keep/cede rules (cedes to peer unless the tool
also keeps `CORUN_GRP_BACK`):

- **Cede model:** pass `CORUN_F_OWN_BACK` as the `flags` argument:
  ```js
  shadow_corun_begin_cede(CORUN_TARGET_CHAIN_EDIT, slot, cedeMask, CORUN_F_OWN_BACK);
  ```
- **Legacy model:** set `CORUN_KEEP_BACK` in `keep_mask`:
  ```js
  shadow_corun_begin(CORUN_TARGET_CHAIN_EDIT, slot,
                     CORUN_KEEP_DEFAULT | CORUN_KEEP_BACK);
  ```
For `CORUN_TARGET_CHAIN_EDIT`, shadow_ui's own Back handler still ends the
session when the chain editor is at its top-level view (`CHAIN_EDIT`) — it
owns the view stack and can tell, so it provides Charles's "Back exits at
top, navigates within" UX for free even under the opt-out. For other
targets like `CORUN_TARGET_MOVE_NATIVE`, the peer UI's depth isn't
observable from the framework, so the tool is responsible for its own
exit gesture (typically Menu, which is tool-routed under the default
keep-mask) and for calling `shadow_corun_end()` itself.

`CORUN_KEEP_BACK` lives in the high half of `keep_mask` (bit 15) so it doesn't
collide with any `CORUN_GRP_*` bit.

## JS API

Exposed on the global object in shadow_ui's JS runtime:

```js
// --- Cede model (recommended) ---
shadow_corun_begin_cede(target, id, cede_mask, flags)
  // target: CORUN_TARGET_CHAIN_EDIT | CORUN_TARGET_MOVE_NATIVE
  // id:     chain slot 0-3 (CHAIN_EDIT) or tool track 0-7 (MOVE_NATIVE)
  // cede_mask: bitfield of CORUN_GRP_* the tool CEDES; keeps everything else
  // flags (optional): CORUN_F_OWN_BACK to handle Back itself
shadow_corun_set_cede_mask(cede_mask)        // update the cede-list mid-session
shadow_corun_set_led_cede_mask(led_cede_mask)// cede these groups' LEDs; paint the rest

// --- Legacy keep-list model ---
shadow_corun_begin(target, id, keep_mask)    // keep_mask: groups the tool KEEPS; 0 = default
shadow_corun_set_led_keep_mask(led_keep_mask)// groups the tool owns for LEDs; 0 = follow keep

// --- Shared ---
shadow_corun_end()
  // Tear down co-run. Called by the framework on Back, or by the tool to
  // exit programmatically (e.g. on track-mode change).
shadow_corun_state()
  // Returns { target, id, keep_mask, flags } or null when no co-run is active.
  // keep_mask is the effective KEEP set in both models (cede is stored as its
  // complement). Tools poll this each frame to detect framework-driven exit.
shadow_corun_event_owner(status, d1)
  // -> CORUN_OWNER_{TOOL,PEER,NONE,BOTH}. The host's corun_event_owner exposed
  // for JS dispatch decisions (single source of truth — keep/cede spec + legacy
  // carve-out honored). e.g. a raw-MIDI overlay (a type:"canvas" editor) consumes
  // an event only when owner === CORUN_OWNER_PEER, so tool-kept controls fall
  // through to the still-running tool instead of being swallowed.
```

Enum constants are registered as JS globals: `CORUN_TARGET_*`, every
`CORUN_GRP_*` group + the `NAV`/`EDIT`/`TRANSPORT` composites, `CORUN_OWNER_*`,
`CORUN_KEEP_DEFAULT`, `CORUN_KEEP_BACK`, and `CORUN_F_OWN_BACK` (matching
`shadow_constants.h`) — so modules reference them directly instead of hand-copying
bit values.

### Capability gate

Tools that want to ship from a single branch against both stock and patched
Schwung should gate the user-facing entry on the API's presence:

```js
const corunAvailable = typeof shadow_corun_begin === "function";
```

## View addressing — overlays over a co-run target

The two co-run targets above hand the surface to a *fixed* destination for the
session's whole life. **View addressing** is a layer on top: while a co-run is
active, a tool can open any **registered** Schwung screen as a temporary
**overlay** over its current target, and return — without changing `corun.target`,
so the tool never tears down.

```js
shadow_corun_entries()            // -> array of openable screen ids (discovery)
shadow_corun_open(id, keep_mask, args)  // -> true if opened, false on unknown id
shadow_corun_close()              // -> dismiss; return to the underlay
```

These three are plain globals defined by shadow_ui (tool and shadow_ui share one
QuickJS `globalThis`). They are backed by a curated registry, `CORUN_ENTRIES`,
mapping a stable id to an existing screen's enter-function — `slots`,
`chain_editor`, `master_fx`, `global_settings`. The registry is curated and added
to deliberately; it is **never** auto-derived from the `VIEWS` enum (most views
are context-dependent sub-views that need preloaded state). A tool discovers what
this build offers via `shadow_corun_entries()` and gates per-id, so it degrades
gracefully across builds that register different screens.

The only C addition is one SHM helper, **`shadow_corun_overlay(active, keep_mask)`**,
which flips `shadow_display_owner` and applies the keep_mask **without touching
`corun.target`** (JS can't write those `shadow_control` fields directly).

### Overlay model

An overlay **reuses the chain-edit co-run dispatch** rather than running a parallel
router. While one is open:

- The outer `view` stays `OVERTAKE_MODULE`; the addressed screen lives in
  `coRunView` (the entry's view change is captured by `runCoRunChainEdit`).
  Keeping `view` at `OVERTAKE_MODULE` is what keeps the tool addressable: the
  unified dispatch (gated `view === OVERTAKE_MODULE`) keeps delegating
  pads/steps/transport to the tool, so the tool's own gestures (its exit, its
  LEDs) keep working — for free, exactly as in chain-edit co-run.
- `coRunUiActive() = coRunChainEditSlot >= 0 || corunOverlayId != null` gates the
  draw / knob / Back-guard / intercept. `coRunWants(grp)` is one uniform rule for
  every UI-element guard: chain-edit handles the groups the tool **cedes**; an
  overlay handles the groups the tool **keeps** (kept events reach this process;
  ceded ones go to Move). So a tool enables, e.g., overlay knob editing simply by
  keeping `CORUN_GRP_KNOBS` — no view-specific code in the dispatcher.
- `corun.target` is **untouched** → `shadow_corun_state()` still reports the
  original target, and the tool does not run its teardown.
- **Back** pops within the addressed view; at the overlay's root it calls
  `shadow_corun_close()` (return to the underlay). **Menu** (and the tool's own
  exit gesture) still ends co-run; the per-frame `shadow_corun_state()` reconcile
  clears the overlay state when co-run ends, handing the screen back to the tool
  rather than stranding the view.

Additive and backward-compatible: with no overlay open (`corunOverlayId == null`)
`coRunWants` collapses to `coRunCedes` and the dispatch behaves exactly as the
framework does without view addressing.

## Move-firmware coupling

`CORUN_TARGET_MOVE_NATIVE` runs as a pure shim-level split: Move firmware
reads the shadow MIDI_IN buffer directly (separate process), so there's no
JS-side intercept or host-API swap to manage. `shadow_swap_display`
early-returns when `shadow_display_owner == DISPLAY_OWNER_MOVE_FIRMWARE`,
handing the OLED to Move's framebuffer while `shadow_display_mode` stays
armed.

**Why the bypass exists:** the obvious alternative — setting
`shadow_display_mode = 0` to expose Move's framebuffer — also disables the
MIDI filter at the `sh_midi` sync site, which would let the tool's pads,
step buttons, and transport leak through to Move firmware. Splitting
"session active" (`shadow_display_mode`) from "who renders"
(`shadow_display_owner`) lets us yield the OLED without tearing down input
gating.

One Move-specific accommodation lives in the shim: **CC 71-78 detents are
coalesced per audio frame** before being forwarded to Move firmware. Move
spends ~900µs per knob CC on a synth-param write plus OLED redraw; multiple
detents in a single frame stack their cost and overrun the SPI frame budget,
manifesting as sequencer stutter while the user spins a knob. The shim sums
incoming detents per knob within each frame and emits one consolidated CC,
clamped to the one-byte signed delta range (±63 — leftover spills naturally
to the next frame's accumulator). Tools that keep `CORUN_GRP_KNOBS` in their
own `keep_mask` are unaffected (knob CCs never reach Move in that case).

Per-frame collapse is the framework's contract. Tools that generate unusually
heavy concurrent MIDI traffic (e.g. simultaneous pad fire, step LEDs, and
automation lanes during transport) may still see residual stutter on very
fast knob spins because the per-frame consolidated CC plus the rest of the
tool's MIDI is enough to pressure Move's SPI window. That's a tool-side
characteristic worth documenting in the consumer's manual; pushing the
coalesce to multi-frame intervals trades knob latency for a problem most
tools won't hit.

### LED ownership

For symmetry with input routing, Move's LED writes are gated per surface group
during `CORUN_TARGET_MOVE_NATIVE`: Move's outbound CC / note-on / note-off LED
messages for any group the tool **owns for LEDs** are stripped before reaching
hardware, so the tool's own rendering on those surfaces stays uncontested.
Surfaces the tool does not own pass through — Move's LEDs reach the buttons /
pads / knob rings directly.

**Lights vs input split.** LED ownership is its own mask, letting a tool **paint** a
surface while still **ceding its presses** — e.g. dAVEBOx draws the track-button
clip indicator (owns TRACK LEDs) but lets Move/Schwung handle the press (cedes
TRACK input). Cede-model tools set it with `shadow_corun_set_led_cede_mask`
(cede these LEDs, paint the rest); legacy tools use `shadow_corun_set_led_keep_mask`
(own these LEDs, `0` = follow input). The cede setter also sets
`CORUN_F_LED_DISTINCT`, which makes the LED mask authoritative even at `0` — so a
cede tool *can* cede all LEDs while keeping input (the legacy "0 follows input"
sentinel no longer blocks that). Input routing never consults the LED mask.

**Steps** (notes 16–31) are now a first-class surface (`CORUN_GRP_STEPS`), so
their input and LEDs route like any other group. This is backward-safe for the
default split and any STEPS-keeping tool, but a tool that passes an explicit
`keep_mask` **omitting** STEPS now cedes step input + LEDs (previously steps were
unclassified and always kept).

**RGB sysex.** Move lights its RGB pads / clips / grid via Ableton LED sysex
(`F0 00 21 1D 01 01 3B <sub> <idx> <rgb> F7`), where `<idx>` equals the control's
CC/note — so the *same* group map classifies it. The framework strips the whole
command for groups the tool owns for LEDs (color-independent: keyed on index),
and leaves sysex for ceded groups (e.g. knob-ring + master colors, idx 71–79)
untouched. Full-overtake tools that keep Move's sequencer running can opt into
stripping **all** cable-0 sysex via `shadow_set_overtake_suppress_sysex(1)` to
take true full LED control; it defaults off and the framework clears it on
overtake exit.

## Single source of truth

A single predicate, `corun_event_owner(ctrl, type, d1) -> {TOOL, PEER, BOTH,
NONE}` in `shadow_constants.h`, decides which side any given control-surface
event belongs to right now. Both the sh_midi let-through filter (forward to
Move) and the forward-to-shadow_ui suppress filter (forward to tool) call this
helper and switch on the result, so the two routes cannot drift apart. Adding
a new target = extend this function; no mirror checks elsewhere.

It is model-aware via `corun.flags & CORUN_F_CEDE_MODEL`: cede-model sessions
govern the full surface uniformly, while legacy sessions take a frozen path with
a carve-out that keeps the extended buttons (transport/edit/nav) with the tool —
so a pre-cede module behaves byte-identically. The cede mask is stored as its
keep complement (`corun_cede_to_keep`), so the predicate's core test
(`keep & grp`) and every LED/JS site share one representation.
