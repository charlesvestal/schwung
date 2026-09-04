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

**`focused_voice` is a resolution rule, not a param key.** Nothing serves
`synth:focused_voice`; it is what `voices.mjs` computes from three raw inputs
(below), and what a sequencer computes the same way from the same inputs. An
earlier draft had the chain host serving it as a key, which stopped being
possible when the fallback moved to a note — a host that does not build the
voice list cannot answer in voice indices.

Resolved, it is the **0-based index into the canonical voice list** — the
list the consumer already has from the same `ui_hierarchy` read, so index → name
/ note / level is a local lookup rather than another round trip. Canonical order:
instance order for a child level; for sibling levels, nav-link order from `root`,
with any voice level `root` does not link appended afterwards in `levels`
declaration order — a voice reachable only from a sub-level must still have a
stable index, and silently dropping it would make the list disagree with itself
between two consumers.

Where the module owns the focus, **writing the module's own param moves it** —
which is how `child_index_param` already behaves, and is what makes the picker
and the module incapable of disagreeing: they are the same write.

The three inputs, in priority order. The first one the module declares wins, and
the rest are not consulted:

| Module declares | Input read | Resolved by |
|---|---|---|
| `child_index_param` (template shape) | `<prefix>:<child_index_param>` — the module's own instance numbering | `childIndexFromWire`, then instance → voice index |
| `focus_param` (new, top-level in `ui_hierarchy`) | `<prefix>:<focus_param>` — a **level name** | level name → voice index |
| neither, but voices declared | `<prefix>:last_note` | note → voice index |

The fallback never runs for a module that owns its focus, so two sources that
disagree and then latch cannot be constructed. A module that moves its own focus
internally (a preset load, mrdrums' auto-select) stays authoritative.

**The fallback reports a NOTE, not a voice.** `chain_host.c` serves
`synth:last_note` — the raw MIDI note number last played into the slot — and the
note → voice lookup happens wherever the voice list already lives (`voices.mjs`
for the grid; a sequencer's own parse for itself). `focused_voice` is still the
single key a consumer asks for; it is resolved in `voices.mjs` rather than in C.

This is deliberate and was changed from an earlier draft. Resolving the index in
C would mean the canonical voice order — nav-link order, then unlinked levels,
then child instances — implemented twice, once in `chain_json.c`'s flat key-scan
helpers, which cannot walk `levels` in order, and once in `voices.mjs`. That is
the shape that produced the metronome and `recall_quantize` off-by-one, and it
fails silently as "the grid follows the wrong pad". One fact, one implementation.

What is left in C is an int store on the note-on, at the `synth->on_midi` call
site in `chain_midi.c` where the note the synth receives is already in hand — no
allocation, no parsing, no logging, nothing that does not belong on the SPI
callback. It sees notes a sequencer injects too, so a sequencer's own pad press
updates it.

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
| `src/shared/param_pages/voices.mjs` *(new)* | Pure: hierarchy object → `{layout, voices[]}`, plus note → voice and the tri-state focus resolution. Both shapes collapse here and nowhere else. Node-testable, sibling to `child_key.mjs`. |
| `src/shared/param_pages/page_controller.mjs` | Read/follow `focused_voice` on the existing `child_index_param` rotation stop. |
| `src/shared/param_pages/page_plan.mjs` / instance picker | Voice names in the instance list. |
| `src/modules/chain/dsp/chain_midi.c` | Record the last note played into the slot at the `synth->on_midi` call site. |
| `src/modules/chain/dsp/chain_host.c` | Serve `synth:last_note`. |
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
(`null` / `""` / out-of-range never move focus); note → voice for contiguous and
sparse maps; and "a declared focus param means `last_note` is never consulted".
Each mutated to prove it can fail before it is trusted green.

## Migration

Contract, host side, docs and tests land here. Fleet PRs follow module by module,
mrdrums (template shape) and 9W9 (sibling shape) first as the two proofs. Movy
drops a bundled config as each module declares.

**The sibling-shape modules publish no hierarchy at all**, which is a bigger lift
than it looked. Measured against `tests/fixtures/module-contracts.json`: 13 of the
100 captured modules answer `ui_hierarchy: null`, and they include **9w9, 6w6,
8w8 and po32-drum** — precisely movy's `OVERRIDES_MODULE_FILE` list plus its
libpo32 config. So for those, declaring a layout means publishing a `ui_hierarchy`
for the first time, not adding a field to one. That is why the POC module in the
plan is the proof rather than a fleet module: there is no sibling-shape module in
the fleet whose hierarchy we could extend today.

It also explains the override list's existence from the other end. Those modules
describe themselves only to movy, in `movy_config.json`, because Schwung offered
them nothing to describe themselves *with* — and a private format with one
consumer is what drifts until a 4-module exception list is needed to correct it.

## Risks

- **`ui_hierarchy` size.** `chain_params` over 64KB will not load; `child_names`
  arrays add to the hierarchy. Negligible for 16 pads, worth a sentence in the
  docs for anyone declaring 200.
- **Dynamic hierarchies.** A module serving `layout` from `get_param` may change
  its answer; consumers re-read on the triggers they already re-plan on.
