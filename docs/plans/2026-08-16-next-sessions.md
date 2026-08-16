# Param Pages — what to do next, and where

Three pieces of work remain. They are ordered by dependency, not by size: the
second cannot be verified without the first, and the third wants a device.

Branch: `param-pages` (PR #201, draft). Everything below assumes it is checked out.

---

## Session A — Graphics in core

**Start cold. Everything needed is written down.**

Read first, in this order:

1. `docs/MODULES.md` → *Parameter visualisations (`viz`)* — the contract
2. `docs/plans/2026-07-26-param-pages-audit.md` §13.5 — the position, the
   precedence chain, and the risk ordering
3. `src/shared/param_pages/README.md` — the six rules the library must not break
4. Reference: `DimaDake/schwung-movy` v0.27.0, `src/model/{envelope,filter-viz,lfo-viz,eq-viz}.ts`
   and `src/renderer/{envelope,filter-curve,lfo-wave,wav-form}.ts` (MIT)

### What to build

Graphics are a **first-class renderer capability**, not a detector feature. Build
them so they draw from a resolved group, and keep *how the group was resolved*
entirely separate.

```
resolve a page's groups:
    module chain_params `viz`  →  module layout file  →  host override  →  detector  →  none
```

The module always wins. A host override may correct a detector but must never
overrule an author who did the work.

**Order — by how obvious a mistake is, not by value:**

| # | Graphic | Why this position |
|---|---------|-------------------|
| 1 | envelope (AD / AR / ASR / ADSR) | ADSR naming is near-universal; a wrong shape is obvious on screen |
| 2 | filter response (cutoff + resonance, mode/slope aware) | second most common; 17 fleet modules have the pair |
| 3 | LFO waveform (shape + rate/depth/phase) | |
| 4 | waveform enum silhouettes | single-param, low risk |
| 5 | fader, on/off switch | cosmetic; `switch` is declaration-driven already since a `toggle` is *declared* a toggle |
| 6 | EQ curve | **last** — false positives are hardest to spot, and Movy has patched theirs repeatedly |
| 7 | sample waveform + position marker | needs WAV/AIFF parsing; largest, least essential |

### The rule that matters most

**Corroborate with declared metadata, not vocabulary.** Movy's best idea in
v0.27.0 is not a graphic — it is `isGainRange`: an EQ band must have a *bipolar,
roughly symmetric* range before a param called "gain" is believed. That one test
rejects the crossovers, Q values and random bounds their word lists let through.
Their envelope and filter detectors have no equivalent and are looser than they
should be. Every detector ported here should demand a range/unit/step check
alongside any name match.

### Guardrails to build alongside, not after

- **A fleet detector snapshot**, checked in. One line per module per fired
  detector, over the 76-module fixture. Changing a heuristic then shows up as a
  reviewable diff instead of a surprise on a device.
- **`validate.mjs` reports inferences** — "an EQ curve was inferred over
  lo/mid/hi_gain" — so an author can see a wrong guess and declare their way out.
  Silent wrongness was the real objection to detectors; visible wrongness is fine.
- **Snapshots and the draw-call budget** already exist in
  `tests/host/test_param_pages_render.sh`. Graphics must stay inside the budget:
  bar 120, dial 700 calls per page. Coalesce runs rather than raising it.

### Done when

Each graphic is independently shippable and independently revertable, the fleet
snapshot shows what fires where, and `validate.mjs` distinguishes declared from
inferred.

---

## Session B — Declare `viz` across the modules

**Do not start this before Session A.** Declaring visualisations for renderers
that do not exist yet means nothing can be verified.

Guide: `docs/plans/2026-08-16-viz-migration-guide.md`. It is written for a cold
start, one module repo at a time. 46 of the 104 catalog modules are in
`charlesvestal/` repos.

Two things from it worth repeating because they are the ways this goes wrong:

- **A wrong group is worse than none.** A plain knob is honest; a wrong envelope
  lies. When unsure, leave it undeclared and let the detector make the same guess
  visibly.
- **Watch the `snprintf` buffer.** Modules that hand-write their `chain_params`
  JSON often size that buffer exactly. Adding `viz` objects truncates the string
  silently and the module loses parameters. Note the param count before and after.

---

## Session C — Hardware

The top unresolved risk, and it needs a Move:

1. **Do eight live values per page keep up with real IPC?** The staggered cursor
   is tested for *shape* — exactly one `get_param` per frame — and never for
   timing. If it cannot keep up, the answer is not more reads; it is showing
   stale values honestly until they arrive.
2. **Draw cost in situ.** 52 (bar) / 290 (dial) calls per page, budgeted
   statically, never timed on device.
3. **The screen-reader strings**, written and tested but never spoken.

---

## Small, independent, not yet done

Both are correctness rather than polish, and neither needs a fresh session:

- **`announceContents()` is bound to no gesture.** "Read the page aloud" is
  written, tested and exported, and nothing calls it. With the screen reader on
  that is the difference between navigating a grid by ear and touching all eight
  knobs to discover what they are. Every obvious gesture is taken, so choosing
  one is a design decision rather than a fix.
- **Open policy question, not a bug: should the screen reader force the list?**
  `paramPagesEnabled()` currently says yes. The list's reading order *is* its
  navigation order, which a grid cannot match; but the grid does announce now,
  and silently reversing a setting the user chose has its own cost.

  (An earlier version of this file claimed the gate was broken because it keyed
  off an "internal" TTS flag while the device used an "external" D-Bus reader.
  That was wrong — there is one screen reader, Schwung's espeak engine, and
  `tts_enabled` is its flag. The D-Bus `com.ableton.move.ScreenReader` signal is
  the *delivery* path and `send_screenreader_announcement` emits on it whether or
  not espeak is speaking, so announcements in the log with the reader off are
  expected. The gate reads the right flag.)
- **`visible_if` is evaluated only at plan time.** Toggling a condition source
  never adds or removes the dependent params; `visible()` is called 0 times
  across 500 ticks. The fix is not polling — the planner knows which keys the
  conditions depend on, so re-plan only when one of those values changes.

---

## Not blocking anything

- The layout-file naming question needed megadake only because it looked like it
  had adopters. It has **zero** — no module ships a `movy_config.json`; the 14
  configs that exist are bundled inside Movy. Name it whatever suits.
- PR #201 is a draft. Mark it ready when Session C is done.
