# Shared Parameter UI Layer — design sketch

**Date:** 2026-06-24
**Status:** brainstorm / not built. Captured from a Discord thread so it stops
living in scrollback. Nothing here is validated on hardware.

## Why this, why now

Three separate community threads turned out to be the same feature:

1. **EmDee (Overture):** "I'm not forced into a module's own UI entrypoint — I
   can standardise on a parameter-page UI component and set params across all
   modules consistently." (This is Overture's "sound engine editor" screen.)
2. **Andreeej:** the ob-xd remote web UI (`web_ui.html`) is lovely; could a
   version of it be available for *most* synths instead of the bare-bones
   default? Ideally with higher-level grouping (Oscillators = Osc1+Osc2+Osc3).
3. **The shared-layer conversation:** capable contributors keep building
   reusable capability in personal corners — EmDee's DSP param-change template
   lives in her `moveforge` harness; the polished UI lives in a standalone
   Overture brand — because Schwung doesn't yet offer a *blessed spot* for these
   things. The path of least resistance points away from upstream.

The fix for all three is the same, and the important part is that **it is mostly
not new code** — it's a generic renderer over a contract Schwung already
exposes. Building this is one concrete way to "turn the gravity around": make
the framework the obvious home so the good stuff lands *in* Schwung instead of
next to it.

## The insight: Schwung already declares parameters uniformly

Every chainable module already publishes its parameters in a uniform shape via
get_param:

- **`chain_params`** — array of `{key, name, type (float|int|enum), min, max,
  step, options, default, unit, display_format}`. This is the per-parameter
  metadata.
- **`ui_hierarchy`** — menu structure + knob mappings (levels, `list/count/
  name_param` for presets, `items/select_param` for selection lists, `knobs[]`).

The on-device menu (`hierarchyMenu`) already renders `ui_hierarchy`. The
"standard" remote UI already mirrors it. So a **generic parameter-page UI that
works across every module is a faithful renderer of declared metadata — not a
guesser.** A `webstream` generator with no oscillators is fine: it declares
whatever params it has, the renderer shows exactly those, no oscillator
assumption anywhere.

This is the resolution of the long-standing "bespoke vs generic" tension:
**render declarations, don't infer layout.** The thing to avoid is an
auto-constructor that *infers* panels/structure from param names. The thing to
build is a renderer that displays what the module *says*, plus a way for a human
to author nicer layouts without hand-writing HTML.

## Design: a continuum, not a binary

Today a module author chooses between two extremes: the bare generic default, or
a fully hand-coded `web_ui.html`. This design adds the missing middle so it
becomes a spectrum:

| Tier | Module author effort | Result |
|------|----------------------|--------|
| **1. Bare generic render** | none | every declared param shown in a default layout, driven by `chain_params` / `ui_hierarchy` |
| **2. Editor-authored `layout.json`** | drag params into panels, pick input types, in a tool | bespoke-looking UI, no code written |
| **3. Bespoke `web_ui.html`** | full hand-coding (today's escape hatch) | unchanged; for people building their own stuff |

Each module opts in to whichever tier it wants. Tier 1 is free. Tier 3 already
exists and is untouched. Tier 2 is the new thing that answers both Andreeej ("I
hate copy-pasting") and the "generic, not smart" rule (a *human* authors the
layout; the tool just removes the typing).

## Three artifacts Schwung would own

1. **Generic renderer** — consumes `chain_params` + `ui_hierarchy` and renders a
   parameter page. EmDee has effectively already built the device-side version
   in Overture; the new surface is the **web** renderer in the remote UI. The
   device `hierarchyMenu` is the existing device-side equivalent.
2. **`layout.json` spec** — an optional, per-module layout description: panels,
   per-param input type (knob / slider / toggle / dropdown / XY), ordering,
   grouping. Lives next to the module's `web_ui.html`. When present, the generic
   renderer uses it; when absent, it falls back to the default layout. The
   grouping idea (Oscillators = Osc1+2+3) is just "three params dragged into one
   panel" — no hand-authored JSON.
3. **Layout editor** — a tool that reads a module's `chain_params`, lists every
   parameter, and lets a human assign input types and drag them into panels,
   emitting `layout.json`. **Natural home: `schwung-manager`** (the Go web app at
   `move.local:7700`) — it already serves per-module Remote UIs and the file
   browser, so reading `chain_params` and writing `layout.json` beside
   `web_ui.html` is an extension of what exists, not a new subsystem.

## Control widgets

Build a small **web-native** control library (knob, slider, toggle, dropdown,
XY) as the renderer's vocabulary. Reference designs like
`compose-audio-controls` for *what good controls look and feel like*, but do
**not** import a Compose/Kotlin/wasm runtime into the HTML iframe — that adds a
second UI runtime to maintain forever for a surface that is currently clean
HTML/CSS/JS. Steal the design, not the framework.

## Out of scope (YAGNI)

- **No automagic UI construction.** No inferring structure from param names. The
  renderer renders declarations; layout beyond the default is human-authored.
- **No second UI runtime** (Compose/wasm) in the remote iframe.
- **No change to `web_ui.html`** as the bespoke escape hatch — it stays.
- **No new param contract.** This rides entirely on existing `chain_params` /
  `ui_hierarchy`. If anything, this exercise might surface small gaps in that
  metadata (e.g. richer grouping hints) — fold those into the existing contract,
  not a parallel one.

## Open questions

- **Spec ownership of grouping:** does grouping live only in `layout.json`
  (web-only, authored in the editor), or should `chain_params` / `ui_hierarchy`
  gain an optional grouping hint so the *device* menu can use it too? Leaning
  layout.json-only first (web is where the bling matters), promote to the shared
  contract only if the device wants it.
- **Device vs web scope:** v1 is the web remote renderer + editor. The device
  already has `hierarchyMenu`; do we unify them later or leave them parallel?
- **Relationship to the "standard" remote UI:** is the generic renderer a
  *replacement* for the current standard remote UI, or a better implementation
  of it? Probably the latter — same intent (mirror the declared params), nicer
  floor.
- **How `layout.json` is delivered:** committed in the module repo alongside
  `web_ui.html`, or authored per-install and stored under `/data/UserData/`?
  (Module-repo for shared defaults; per-install for user customisation — likely
  both, with per-install overriding.)

## How this connects to the davebox-first / upstreaming thread

This is one instance of the bigger move discussed alongside it: rather than
absorb forks one at a time, give Schwung the *slots* contributions are reaching
for. EmDee's param-page wants to be the generic renderer; Andreeej's "nicer
default" wants Tier 1+2; the bespoke crowd keeps Tier 3. Shipping this as a
blessed Schwung layer converts "I'll standardise in my template/fork" into
"it's already in the framework." Same principle as making a davebox-first
groovebox an optional Schwung mode instead of a hidden standalone product.

## Suggested next steps

1. Get EmDee's `moveforge` param-page / template in front of us — it's most of
   the Tier-1 renderer already; find the seam between "shared scaffold" and "her
   modules' usage of it."
2. Sketch the `layout.json` schema (panels + per-param input type + order).
3. Prototype the editor in `schwung-manager` reading one module's
   `chain_params`.
4. Build the web control widgets as the renderer vocabulary.
