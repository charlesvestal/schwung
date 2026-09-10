# Module taxonomy, automated screenshots, and audio previews

Date: 2026-09-10
Status: design approved, not implemented

## Problem

The catalog carries 133 modules under a single axis, `component_type`, with five
values: 52 `sound_generator`, 40 `audio_fx`, 16 `tool`, 13 `midi_fx`, 12
`overtake`. schwung-manager's `/modules` page already filters, sorts and lays
out cards against that axis — the machinery is there and it is fed one bit of
information. "Sound Generators (52)" is not a filter; it is the list again.

Two of those 133 answers a user actually wants are not in the catalog at all:
what does it look like, and what does it sound like. Seventeen `.m4a` previews
exist on the catalog site, recorded by hand in April. That is 13% coverage and
it has not moved, because the marginal cost of the eighteenth is another manual
recording session.

## Shape of the answer

Three separately shippable pieces, in dependency order:

1. **Taxonomy** — a second categorisation axis plus open tags. Useful the day it
   lands; needs no harness.
2. **Probe harness + screens** — a small ARM64 binary that loads a module
   without a device, feeding the renderer that already exists.
3. **Audio** — the same binary, rendering a score chosen by the taxonomy.

Every piece is automatic on catalog change, so module 134 arrives already
categorised, screenshotted and audible.

---

## 1. Data model

### Three layers

`component_type` is **unchanged**. It is load-bearing for the host — menu
placement, chain slot type, install path (`modules/<component_type>s/<id>/`).
Nothing in this design touches it.

`subcategory` is new: **exactly one** per module, from a closed vocabulary. It
drives the filter chips and the badge on a card.

`tags` is new: an **open list**. It drives search and secondary facets.

### Source of truth: module declares, catalog overrides

A module may declare `capabilities.subcategory` and `capabilities.tags` in its
`module.json`. The catalog entry may carry `subcategory` and `tags` keys, which
**win outright** (not merge — a merge makes "why does this module have that tag"
unanswerable from either file alone).

The resolved entry carries `_taxonomy_source: "module" | "catalog" | "derived"`
per field, so a wrong tag is traceable to one file.

Why both: the catalog layer is what makes this work retroactively across 133
mostly third-party repos with no release churn, and what lets a mis-tag be fixed
in one PR. The module layer is what lets an author own their own metadata going
forward, and is the only layer that can describe a module before it is
catalogued.

**The on-device `module.json` reader is a first-occurrence text matcher** — a
nested block renames the module (see the host JSON parser note in CLAUDE.md).
`subcategory` is a flat string and `tags` a flat array of strings, declared at
the top level of `capabilities`, for that reason. Neither is read by the host at
runtime; only the catalog build reads them, with a real JSON parser. If that
ever changes, the parser constraint becomes real.

### Vocabulary

Lives in `taxonomy.json` at the root of the schwung repo — one file, versioned,
with the display label and the parent `component_type` for each subcategory. CI
**fails** a catalog PR that names a subcategory not in it. Tags stay open, but
CI **warns** on a tag outside the known set so typos surface rather than
silently creating a facet of one.

| component_type | subcategories |
|---|---|
| `sound_generator` | Virtual Analog · FM · Wavetable · Sampler & Rompler · Drum Machine · Physical Modeling · Chiptune & Retro · Granular · Macro & Hybrid · Streaming & Live Input |
| `audio_fx` | Reverb · Delay & Echo · Modulation · Distortion & Saturation · Dynamics · Filter & EQ · Granular & Spectral · Looper & Sampler FX · Multi-FX · Utility |
| `midi_fx` | Arpeggiator · Sequencer · Chord & Harmony · Routing & Utility |
| `tool` | Sequencer · Sampling & Editing · Tuner & Analysis · Assistant · Performance |
| `overtake` | Controller · Sequencer · Sampler & Looper · Performance FX |

Ten is the ceiling for the two large types; below that a filter row stops being
legible at a glance, which is the failure the current single axis already has.

Known tags, cross-cutting: `vintage-emulation`, `mono`, `polyphonic`, `mpe`,
`generative`, `euclidean`, `tape`, `lo-fi`, `spectral`, `bass`, `vocal`,
`drums`, `chord`, `sample-based`, `needs-assets`, `needs-line-in`, `ai`,
`cpu-heavy`, `has-remote-ui`.

**Three of those are derived, never authored.** `needs-assets` from a non-empty
`requires`; `needs-line-in` from `capabilities.audio_in`; `has-remote-ui` from
the presence of `web_ui.html` in the release tarball. A derived tag written by
hand is a fact that can go stale against the thing it describes, so CI computes
them and strips any hand-written copy.

### Initial assignment

One pass over all 133, authored in `module-catalog.json`, reviewed as a single
PR. Not delegated to 133 repos and not deferred to authors — coverage on day one
is the whole point, and the catalog layer exists precisely so this is one file.

---

## 2. `schwung-probe` — the harness

`tools/probe/`, a small C binary for ARM64 Linux, **built by the existing
`Dockerfile`** (`debian:bookworm`, arm64). Building it in the same image modules
were built in is what guarantees it can load them: same glibc, same
`libdbus`/`libsystemd`/`libespeak` the build environment provides.

### What it does

`dlopen`s a module's shared object and dispatches on **which entry symbol
resolves**, because there are three plugin contracts and they are not
interchangeable:

| | synth | audio FX | MIDI FX |
|---|---|---|---|
| entry symbol | `move_plugin_init_v2` | `move_audio_fx_init_v2` | as chain host resolves |
| render fn | `render_block` | `process_block` | (no audio) |
| `.so` name | `dsp.so` | `<id>.so` | varies |

Getting this wrong fails quietly — the wrong symbol produces a clean load and no
sound — so the probe **reports which contract it matched** in its output and
treats "no known entry symbol" as an error, never as an empty result.

Two modes:

**`--contract <module-dir> -o out.json`** — creates an instance, calls
`get_param("chain_params")` and `get_param("ui_hierarchy")`, emits the JSON
shape `tools/param-pages/dump_contracts_device.js` already produces. This
**retires the device-dump fixture**: contracts stop being a capture that can
silently describe a module as it was six weeks ago, which is a caveat
`audit_sheet.mjs` currently has to print on every run.

A read that does not answer is `null`, a read that answers with nothing is `""`,
and the probe must keep those apart in its output — the whole tri-state rule
from CLAUDE.md applies here, because the renderer downstream will invent a
`float 0..1` knob for missing metadata and it will look plausible.

**`--render <module-dir> --score score.json -o out.wav`** — creates an instance,
applies preset/params, feeds a timed MIDI and param score, calls the render
function in 128-frame blocks at 44100 Hz, writes stereo WAV.

### Fidelity ceiling, stated

The probe runs a plugin **in isolation, not under the chain host**. An FX
preview is therefore the effect on a dry source, not the effect in a slot behind
a synth; there is no bus, no send, no master FX, no LFO overlay. This is the
accepted cost of not maintaining a full emulated host in CI. It is recorded in
the probe's own output (`harness: "probe-isolated"`) so a future full-host
harness can publish the same artifacts at higher fidelity without any consumer
changing.

### What the probe is not

It is not realtime and does not pretend to be. It calls plugin entry points from
an ordinary thread at whatever speed the CPU allows. That means it **cannot
detect the realtime violations** an audit found ~150 of across the fleet — a
module that allocates in `render_block` renders fine here. Nothing in this
design should be read as a realtime check.

---

## 3. Screens

Two passes, one output contract. Both emit PNGs of the 128x64 1-bit display at
4x scale.

### Grid modules (~105)

Probe dumps the contract; the **existing** `tools/param-pages/` renderer draws
the pages. `preview.mjs` already does exactly this — through the device's own
font atlas, so what it draws is what the OLED shows. Nothing new is written for
this path beyond wiring the probe's output in where the fixture goes today.

Hero image is page 1. Remaining pages are gallery images.

Values are synthesised (`fake_values.mjs`). These are **layout** pictures, not
patch pictures, and the published metadata says so.

### Overtake and tool modules (28)

These own the whole surface and have no knob grid to plan, so the grid renderer
has nothing to draw. A QuickJS pass loads the module's `ui.js` with the display
API pointed at a 1024-byte framebuffer instead of SPI, calls `init()`, ticks it
a fixed number of frames, and unpacks the framebuffer to PNG.

This is the one genuinely new renderer, and the risky one: these modules assume
LEDs, MIDI, a jog wheel and often an external device exist. Host bindings are
stubbed. A module that hard-depends on real input produces a blank or garbage
frame.

**A blank frame is a CI failure, not an artifact.** It publishes a status and no
image. The alternative — publishing whatever came out — puts a black rectangle
on a catalog card and gives nobody a reason to look into it.

If this pass proves more expensive than it is worth, it is deferrable: the 28
modules fall back to `status: "no-screens"` and the other three parts of the
design are unaffected.

---

## 4. Audio

### Scores

A score is a JSON document: initial params/preset, then timed MIDI events and
param changes, then a duration. Defaults are keyed on **`subcategory`** and live
in `tools/probe/scores/`. A module may override with `preview.json` in its own
repo, shipped in its release tarball.

| subcategory | default score |
|---|---|
| Virtual Analog, Wavetable, FM, Macro & Hybrid, Physical Modeling, Chiptune & Retro | 30 s phrase, chord bed + lead. Monophonic variant where the module declares mono |
| Drum Machine | pattern over the module's **declared** voices, read from `split_voices` / `ui_hierarchy` — never guessed from note numbers |
| Sampler & Rompler, Granular | phrase over the module's bundled default content |
| all `audio_fx` | fixed dry source loop, committed to the repo, wet-forward |
| all `midi_fx` | driven into **one** reference synth, pinned by version |
| Streaming & Live Input, Assistant, Tuner & Analysis | **no audio preview** |

The last row is deliberate. These have nothing deterministic to render — a
webstream module with no stream, an AI assistant with no key, a tuner with no
input. A synthesised preview for them would be a fabrication, so they get
`status: "not-applicable"` and the UI shows no player.

The reference synth for MIDI FX and the dry source loop for audio FX are both
**pinned and committed**, so a preview that changes means the module changed.

### Encoding

`ffmpeg`, at the target `schwung-catalog-site/audio/normalize.sh` already uses:
EBU R128 **-16 LUFS, true-peak -1.5 dBTP, AAC 128k, `+faststart`**. **30 s**
clips, matching the existing hand-recorded set, ~450 KB each.

Reusing that loudness target is what makes a generated preview and a
hand-recorded one comparable in a single list without one of them being twice as
loud.

---

## 5. CI and publishing

### Workflow

`previews.yml` on the schwung repo, `runs-on: ubuntu-24.04-arm` — free for
public repos, and schwung is public. A private repo would need a paid runner;
that is the one hard dependency in this design.

Triggers:
- catalog change (a PR adding or editing a module),
- manual dispatch,
- nightly, restricted to modules whose `release.json` version moved since the
  last run.

Per module: download the release tarball → probe `--contract` → render screens →
probe `--render` → encode → collect.

### Output

Force-pushed as a **single commit** to `schwung-preview-assets`, served by
GitHub Pages. History never grows regardless of how often previews regenerate —
at 30 s clips the working tree is ~50 MB and stays ~50 MB.

Alongside the binaries, `index.json`:

```json
{
  "generated_at": "2026-09-10T00:00:00Z",
  "harness_version": "1",
  "modules": {
    "obxd": {
      "screens": { "status": "ok", "hero": "obxd/page1.png",
                   "gallery": ["obxd/page2.png"], "kind": "grid-synthesised" },
      "audio":   { "status": "ok", "file": "obxd/preview.m4a",
                   "score": "subcategory-default", "harness": "probe-isolated" }
    },
    "minijv": {
      "screens": { "status": "needs-assets" },
      "audio":   { "status": "needs-assets" }
    }
  }
}
```

### Failure is a status, never a placeholder

Statuses: `ok`, `needs-assets`, `not-applicable`, `no-screens`, `error`.

The 17 modules with a non-empty `requires` cannot render in CI — no ROMs, no
soundfonts, no `.nam` models, and **none of those should ever be committed**.
They get `needs-assets` and fall back to a hand-recorded preview where one
exists.

A module with no preview shows no preview, and the reason is in the JSON. A
placeholder image or a silent audio file would be indistinguishable from a
working pipeline producing bad output, which is the failure mode that lets a
broken preview sit in a catalog for months.

---

## 6. Consumers

**Catalog site** (`catalog.html`) — subcategory chips below the existing type
filter; tag pills; hero screenshot on the card; inline audio player. The 17
existing hand-recorded `.m4a` files **stay and win** over a generated one for
the same module.

**schwung-manager** (`/modules`) — the filter, sort, grid/list and persisted
filter state already exist. This feeds a second axis into `data-subcategory` and
an image into the card. Assets load from the assets host directly in the
viewer's browser, so the Move serves no additional bytes.

**Desktop installer** — same catalog JSON, no additional pipeline.

**On-device shadow UI** — nothing. The on-device store is retired; `[Get
more...]` opens the Connect screen. This design adds no on-device surface.

---

## 7. Testing

- `taxonomy.json` validation: every catalog `subcategory` is in the vocabulary
  and belongs to that entry's `component_type`. Fails the PR.
- Derived tags: computed values match, hand-written copies are stripped.
- Probe contract dispatch: a fixture module of each of the three kinds resolves
  to the right contract; an object exporting no known entry symbol is an error,
  not an empty result.
- Probe tri-state: a `get_param` returning failure is `null` in the output and a
  key serving nothing is `""`, distinguishably.
- Renderer: existing `tests/host/` param-page tests, now fed from probe output
  rather than the device fixture.
- `index.json` shape, and that no artifact is published with a non-`ok` status.

The renderer half is already covered by `tests/host/`; the new surface is the
probe and the CI assembly.

---

## 8. Phasing

Each phase ships independently and is useful on its own.

1. **Taxonomy** — `taxonomy.json`, catalog fields, the 133-module assignment
   pass, CI validation, filter UI in the manager and the catalog site. No
   harness.
2. **Probe + screens** — the binary, the contract mode, the grid screen
   pipeline, the assets repo, `index.json`, images in both consumers. The
   QuickJS overtake pass lands here or is deferred.
3. **Audio** — render mode, the score set, encoding, players in both consumers.

## 9. Out of scope

- Full-host or emulated-device fidelity (approach B). The output contract is
  designed so it could replace the probe later without any consumer changing.
- On-device capture rigs.
- Realtime-safety checking.
- Committing any module's external assets.
- Any on-device surface.
