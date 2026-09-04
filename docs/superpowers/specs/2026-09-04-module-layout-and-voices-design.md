# Module layout & voices — design

**Date:** 2026-09-04
**Status:** approved, not implemented

## Problem

A sequencer driving a Schwung chain slot — movy is the live case — has to lay out
Move's pads for whatever module is in that slot, and it has no way to ask what
that module is. A drum module wants a rack: one voice per pad, and the pad you
hit is the page you want to edit. A synth wants a chromatic keyboard. Nothing in
`module.json` or `ui_hierarchy` says which.

Movy solves it privately today. `schwung-movy-embed/src/types/param.ts` carries a
`DrumConfig` (`padCount`, `padNoteStart`, `rawMidi`, `currentPadParam`,
`padScoping`), fed by a `movy_config.json` a module may ship, by 14 configs
bundled into movy, and by a 4-module override list
(`OVERRIDES_MODULE_FILE = {6w6, 8w8, 9w9, cw78}`) for modules whose shipped
config movy cannot use. `padScoping.concreteKeyTemplate` is a verbatim
re-spelling of `ui_hierarchy`'s `child_key_template`. Every other sequencer that
ever ships would have to build the same table.

Second half of the problem: even with the layout known, a sequencer showing a
per-voice page needs to know **which voice is focused**, and to keep in step when
the module moves that focus itself.

## What already exists

`ui_hierarchy` child levels already declare most of the structure — `child_count`,
`child_label`, `child_key_template`, `child_index_base`, `child_index_digits`,
and `child_index_param`, which is a module-owned focused-instance param that the
host reads and writes in both directions (`docs/MODULES.md`, "when the MODULE
owns the focus"). What is missing is the layout statement, the note map, and an
equivalent of `child_index_param` for modules whose voices are sibling levels
rather than template children.

## The two fleet shapes

|  | mrdrums, po32-drum | 9W9, 6W6, 8W8, CW78 |
|---|---|---|
| Voices | 16 interchangeable, one key template (`p{index}_{key}`) | 11 **differently-shaped** pages, own keys each |
| In `ui_hierarchy` | one child level, `child_count: 16` | sibling levels + nav links from `root` |
| Focus | module-owned param (`ui_current_pad`) | nothing — movy's own bank cursor |
| Non-voice pages | — | Reverb, Delay, Main: navigable, sound nothing |

Both must be expressible, in the level each already has. 6W6/8W8/9W9/CW78 are on
movy's override list precisely because their shipped configs put a `pad` on every
bank including the pages with no voice behind them, and movy reads the leading
run of pad-declaring banks as the voices — so "every bank is a voice" collapses
the module to one page. A design in which a page and a voice are distinguishable
makes that unrepresentable rather than merely fixed.

## Design

### 1. Layout is declared, never inferred

Top level of `ui_hierarchy`, beside `levels`:

```json
{ "layout": "drums", "levels": { } }
```

Values `"drums"` | `"chromatic"`. **Absent is a distinct third state meaning the
module has not said** — which is every one of the 95 fleet modules today. A
consumer picks its own default for unspecified and is never handed `"chromatic"`
by a module that never answered. Same tri-state discipline `shadow_get_param`
already enforces between `null` and `""`, and it is what lets movy keep a bundled
fallback for an undeclared module without that fallback ever overriding a real
declaration.

**Rejected: inferring "drums" from the presence of notes.** A melodic module can
legitimately carry notes on per-zone or per-key pages (a sampler with key zones,
a multitimbral synth, a chord module), and inference would seat it as a rack.
Notes describe voices; they cannot also be made to mean how to lay out a surface.

Living in `ui_hierarchy` rather than `module.json` means it can be served from
`get_param`, so a module whose answer depends on what is loaded (sfz, slicer —
drums or melodic according to the kit) can say so. A module with a fixed answer
puts it in `module.json`'s static hierarchy, which `chain_host.c` already caches
and serves.

### 2. Voices are described, orthogonally

A **voice** is a navigable thing that plays a note.

Sibling levels — a level that declares `note` is a voice; one that does not is a
page:

```json
"levels": {
  "bass_drum":  { "name": "Bass Drum",  "note": 36, "role": "kick", "knobs": [] },
  "closed_hat": { "name": "Closed Hat", "note": 42, "role": "hat",  "knobs": [] },
  "reverb":     { "name": "Reverb", "knobs": [] }
}
```

Template children — the child level gains a note map:

```json
"pads": {
  "child_count": 16, "child_label": "Pad",
  "child_key_template": "p{index}_{key}", "child_index_base": 1, "child_index_digits": 2,
  "child_index_param": "ui_current_pad",
  "child_note_base": 36,
  "child_names": ["Kick", "Snare", "Rim"],
  "child_notes": [36, 38, 42, 46]
}
```

`child_note_base` for the contiguous case (instance *i* in declaration order
plays `base + i`), `child_notes` for a sparse map. `child_names` / `role` and
`child_roles` are optional.

**`role` is a free string and no host behaviour depends on it.** It is a hint a
consumer may use to colour or seat a rack it has never seen, and a consumer that
does not recognise a value ignores it. It is deliberately not an enumeration:
constraining it would mean maintaining a percussion vocabulary in the host for a
field the host never reads.

`layout` and voices are independent on purpose. `"drums"` with no voices declared
is legal (a rack whose pages are not published yet); `"chromatic"` with notes is
legal and correct. No consumer seats a surface from anything but `layout`.

**Movy's `rawMidi` becomes derivable and is not part of the contract.** A
per-voice note describes both a sparse whole-grid map and a 4-wide rack. Voice
order is rack order; seating is the consumer's business.

### 3. Focus: one key, exactly one live source

`focused_voice`, on the component prefix (`synth:focused_voice`), readable and
writable. It answers the **0-based index into the canonical voice list** — the
list the consumer already has from the same `ui_hierarchy` read, so index → name
/ note / level is a local lookup rather than another round trip. Canonical order:
instance order for a child level; for sibling levels, nav-link order from `root`,
with any voice level `root` does not link appended afterwards in `levels`
declaration order — a voice reachable only from a sub-level must still have a
stable index, and silently dropping it would make the list disagree with itself
between two consumers.

Writing it moves focus, which is how `child_index_param` already behaves and is
what makes the picker and the module incapable of disagreeing — they are the same
write.

| Module declares | Source of `focused_voice` |
|---|---|
| `child_index_param` (template shape) | forwarded to the module, translated out of its own numbering |
| `focus_param` (new, top-level in `ui_hierarchy`, value is a level name) | forwarded to the module, translated to an index |
| neither, but voices declared | host fallback: the chain host's last-played voice |

The fallback never runs for a module that owns its focus, so two sources that
disagree and then latch cannot be constructed. A module that moves its own focus
internally (a preset load, mrdrums' auto-select) stays authoritative.

**The fallback** lives in `chain_host.c`. The note → voice table is built once at
component load, beside the existing `parse_ui_hierarchy_cache`; the runtime cost
on a note-on is a scan of at most 32 entries and an int store — no allocation, no
logging, nothing that does not belong on the SPI callback. It sees every note
routed to the slot, including notes a sequencer injects, so a sequencer's own pad
press updates it too.

Two guarantees carried over verbatim from `child_index_param`:

- **A read that does not answer never moves the focus.** Empty, non-numeric or
  out-of-range is ignored, not treated as voice 0.
- **It costs no extra IPC in the grid**, sharing a rotation stop as it does
  today. A consumer that plays the note itself already knows and need not poll
  fast; polling only catches changes originating elsewhere.

### 4. Consumers

**Shadow UI knob grid** — two behaviours, no third:

1. The instance picker shows declared voice names where it says "Pad 1 … Pad 16".
2. Hitting a pad navigates the grid to that voice's page.

**Nothing writes pad LEDs.** Move owns the pads; the follow path is a read plus a
navigation and never a MIDI-out. This gets a source-invariant pin, because "while
I am here I will light the rack" is exactly the change someone makes later in
good faith.

**Movy** is external and out of scope for the code. It reads `layout`, the voice
list and `focused_voice`, and keeps its bundled configs as a fallback for modules
answering *unspecified* — the rule `loader.ts` already states ("delete an entry as
soon as the module ships a config movy can use"). The tri-state is what makes
that safe.

## Files

| File | Change |
|---|---|
| `src/shared/param_pages/voices.mjs` *(new)* | Pure: hierarchy object → `{layout, voices[]}`. Both shapes collapse here and nowhere else. Node-testable, sibling to `child_key.mjs`. |
| `src/shared/param_pages/page_controller.mjs` | Read/follow `focused_voice` on the existing `child_index_param` rotation stop. |
| `src/shared/param_pages/page_plan.mjs` / instance picker | Voice names in the instance list. |
| `src/modules/chain/dsp/chain_host.c` | Serve `focused_voice` (forward or fallback); note → voice table at load; note-on watcher. |
| `docs/MODULES.md` | The contract module authors implement. |
| `docs/CHAIN.md` | What the chain host serves and how the fallback is scoped. |
| `docs/PARAM_PAGES.md` | Grid behaviour, and the no-LED rule. |
| `CLAUDE.md` | One hook bullet per subsystem doc touched. |

## Verification

**A real consumer, before the contract is called done.** The deliverable is a
contract other people implement, and source pins cannot see call ordering. A
POC module declaring both shapes — sibling voices and template children — gets
built, packaged and installed through the real path, and driven on hardware: the
picker shows its voice names, hitting a pad moves the grid to that voice, and
`focused_voice` answers correctly for a module that owns its focus and for one
that does not. Screens verified by looking at them.

**Fleet regression, the load-bearing test.** Run `voices.mjs` over all 95 modules
in `tests/fixtures/module-contracts.json` and assert every one reports
*unspecified* and no existing hierarchy is reinterpreted. The change must be
provably inert for the whole fleet until a module opts in. This is the assertion
that catches a notes-imply-drums inference reappearing, since several fleet
modules carry notes on melodic pages.

**Unit tests:** both shapes' voice ordering; sparse `child_notes`; absent
`layout` reported as unspecified and never as `chromatic`; the focus tri-state
(`null` / `""` / out-of-range never move focus); a C unit for the note table and
for "a declared focus param means the fallback never runs". Each mutated to prove
it can fail before it is trusted green.

## Migration

Contract, host side, docs and tests land here. Fleet PRs follow module by module,
mrdrums (template shape) and 9W9 (sibling shape) first as the two proofs. Movy
drops a bundled config as each module declares.

## Risks

- **`ui_hierarchy` size.** `chain_params` over 64KB will not load; `child_names`
  arrays add to the hierarchy. Negligible for 16 pads, worth a sentence in the
  docs for anyone declaring 200.
- **Dynamic hierarchies.** A module serving `layout` from `get_param` may change
  its answer; consumers re-read on the triggers they already re-plan on.
