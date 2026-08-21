# A variable-length signal chain

Status: approved design, not yet implemented
Date: 2026-08-20

## The problem

A slot's chain is a fixed shape: one MIDI FX, one synth, two audio FX. The
limit is not the DSP — `MAX_AUDIO_FX` is already 4 and every mechanism beneath
the param router is index-generic. The limit is the UI, which declares five
literal positions in `CHAIN_COMPONENTS` (`shadow_ui.js:427`) and spells out
`fx1`/`fx2` in 54 places.

## What this is not

Not an arbitrary routing graph. Slots are the PARALLEL dimension (four of them,
deliberate) and the chain is the SERIAL one. Two synths fed by the same MIDI is
already expressible — two slots on the same receive channel — and collapsing
the two axes into one graph would cost the property that makes the current
model legible on a one-screen device: you always know what shape it is.

## The model

The chain is an ordered list. The NAMING does not change:

    midi_fx1..N   synth   fx1..N

Position is derived from the index, so "move right" is a swap of two indices
and their state blobs. Nothing is renamed, so LFO targets and knob mappings —
which key on the name — follow the module without being re-pointed.

**There is no migration.** `fx1:` and `fx2:` keep meaning exactly what they
mean today; `fx3:`..`fxN:` simply become valid. Every saved slot, patch and
preset loads unchanged.

Bounds: `MAX_AUDIO_FX` 4 -> 8, `MAX_MIDI_FX` 2 -> 8. Static allocation keeps
the audio path RT-safe and the state size bounded, and the CPU runs out well
before eight FX on this hardware. "Unlimited" in feel, bounded in fact.

This reverses an earlier decision that one MIDI FX per slot was deliberate.
That reversal is intentional and was made by the owner on 2026-08-20.

## The diagram

    [PATCH] |+| [ARP][CHD] [SF2] [REV][DLY] |+| [SET]

Patch selection and slot settings keep their seats at the extreme ends, just
outside the `+` boxes — muscle memory is preserved and no new page-level
grammar is introduced.

Scrolling is HYBRID: the synth holds the centre while the chain fits, which is
the common case and matches the fixed-anchor sketch exactly. Past that the view
scrolls to keep the SELECTION visible, because a jog-driven UI must never be
editing something off-screen, and the synth box takes a distinct outline so it
still reads as the landmark when it is off to one side.

The header carries the patch name on the left and the SYNTH name on the right —
the anchor that survives scrolling. `CHAIN` is dropped from the right; a chain
diagram does not need to announce itself.

## Interaction

- **Jog** selects, as now.
- **Click** opens: the picker for a module, the patch browser, slot settings.
- **Shift+jog** moves the selected module along the chain. A click cannot
  express direction; Shift+jog can, and Shift-changes-what-the-jog-means is
  already the grammar in the section picker, the page stepper, the preset
  browser and the items list. `handleJog(delta)` currently takes no shift
  argument and never consults `isShiftHeld()`, so it needs threading; nothing
  else on this screen claims the gesture.

  Moving is bounded to the module's OWN section: a MIDI FX cannot cross the
  synth, because the two sides are different kinds of thing and crossing would
  be a type change rather than a reorder. Moving stops at the ends; it does not
  wrap.
- **The footer follows the modifier.** At rest it reads
  `JOG SEL / CLK OPEN / BACK EXIT`; with Shift held, `JOG MOVE`. This is
  already how the knob grid behaves (`JOG PAGE` becomes `JOG SECT`, and
  `KNB FINE` appears), so holding Shift is self-documenting rather than
  something you have to be told. It is also the better answer to the
  discoverability problem that a modifier gesture normally has — better than
  the picker entries, which stay as a second way in but are no longer carrying
  that job alone.
- **`+`** opens the picker and inserts at the position the `+` itself occupies
  — the OUTERMOST end of that section. Adding an audio FX appends it after the
  last one, which is both what the visual position promises and the common
  case. Insert-in-the-middle is reordering: add, then Shift+jog it into place.
- The picker gains `Move Left` / `Move Right` so the gesture is not the only
  way in. **Remove already exists** as the `None` entry the picker leads with
  (`shadow_ui.js:7098`).

SWAPPING and REMOVING are different operations and must not be confused:

- **Swap** — choosing a different module for an occupied position REPLACES the
  occupant and the position does not move. Swapping fx1 from Reverb to Delay
  while fx2 exists leaves Delay at fx1 and fx2 exactly where it was. This is
  not a list operation at all; nothing reorders.
- **Remove** — choosing `None` takes the module OUT, and the list compacts so
  there is no hole. This is a behaviour change: today clearing fx1 leaves fx2
  where it was, with an empty fx1 in front of it.

**Compaction happens on EDIT, never on LOAD.** This distinction was missing from
the first draft of this document and is load-bearing. Position *i* of the editor
list IS `fx(i+1)` in the DSP — that identity is what makes every param address,
LFO target, bypass key and preset prefix land on the right module. So a patch
that already contains a hole must LOAD with the hole intact, drawing an empty
box; compacting it on read would put `fx2` in position 0, label it "FX 1", and
then read and write `fx1:*` — editing an empty slot while the audio ran through
fx2. The renumber happens in the DSP and the UI together, in one gesture, or not
at all.

The distinction matters because both are reached from the same picker, one
entry apart. A swap that silently reordered the chain would be the worst kind
of surprise — you went in to change a reverb and came out with a different
signal path.

## Where the work is

Not the DSP. The two `fx1:`/`fx2:` branches in `chain_host.c` (873, 908, 1615,
1688) are copy-paste twins differing only by an index, so parsing the digit
REMOVES code.

The work is `shadow_ui.js`: `CHAIN_COMPONENTS` becomes derived from the live
chain instead of a literal, and the 54 hardcoded references follow. That file
is already the largest in the tree, so the chain model should come out of it
into its own pure module — a list, its ordering operations, and the scroll
window — testable without a device, the way page_plan and page_controller are.

## Testing

- The chain model is pure: build a chain, move, insert, remove, assert order
  and that indices/state travel with the module. No device.
- The scroll window is pure: given N components and a selection, which are
  visible and is the selection among them. Pin the hybrid rule at the boundary
  (fits -> synth centred; overflows -> selection visible).
- Render assertions through the existing framebuffer harness: nothing clipped
  at 1, 5 and 8+ components; the synth outline distinct.
- A DSP test that `fx7:` routes to index 6, and that `fx1:`/`fx2:` still route
  where they always did.
- Migration: an existing two-FX saved slot loads with both FX in the right
  order.

## Risks

- The 54 references are the real cost, and they are the kind that fail
  silently — an unserved key answers "" rather than null (see the 2026-08-20
  session). Anything reading a chain component should go through the model.
- Eight FX in series is a CPU question this design does not answer. The cap is
  chosen so the user can get themselves into trouble deliberately; the audio
  path should not pretend that is free.
