# RESUME — module preview harness

Parked 2026-09-11 on `feat/preview-harness` ([PR #487](https://github.com/charlesvestal/schwung/pull/487)).
Phase 2–3 of `docs/superpowers/specs/2026-09-10-module-taxonomy-and-previews-design.md`.
Phase 1 (taxonomy) is **merged** — schwung#486 and schwung-catalog-site#4.

## What works

Generate a module's preview assets with no device, no SPI, no QuickJS:

```bash
tools/probe/build.sh                                   # builds build/probe in arm64 Docker
node tools/probe/generate.mjs obxd cloudseed --out build/previews
node tools/probe/generate.mjs --all --out build/previews
```

Per module it fetches the release tarball, dumps the contract, draws the knob
pages with the **movy** renderer, renders and encodes 30 s of audio per preset,
and merges an entry into `build/previews/index.json`.

CI: `.github/workflows/previews.yml`, `ubuntu-24.04-arm` (free for public
repos), uploads an artifact. Publishes nowhere yet — deliberately.

Roughly 23 modules exercised. Screens `ok` on all of them; audio `ok` on most,
with the failures carrying a reason rather than a placeholder.

## Decisions, and why they are not arbitrary

- **The FX reference instrument is `wurl`**, chosen by measurement: it renders
  note 60 at 262.5 Hz with tight pitch and a +27.2 dB attack. The previous
  choice (obxd's default preset) rendered an octave high with a detuned unison
  that smeared whatever the effect did. A slow-attack pad is worse than either
  — hera's Strings 1 is in tune with **−3.5 dB** of attack, so a reverb has
  nothing to excite it. `index.json` records the source id and version, because
  two FX previews are comparable only while the instrument in front of them is
  the same one.

- **One score, one tempo, `scores/demo_score.mjs`.** Arp (0–3.6 s), rest,
  three held chords with gaps (4.4–12.8 s), rest, melody, final chord, then
  silence. 37 notes, silent 34% of the time, longest rest 3.0 s — longer than
  the longest release measured anywhere in the fleet.
  - Bach's WTK I Prelude I was the first attempt and is still in the tree for
    `preview.json` overrides. It was wrong for one reason: **it never stops**,
    so a long-release patch had no room.
  - Adapting the note density per preset was WORSE than the problem it fixed:
    every preset played at a different tempo, which destroys the only thing a
    shared score is for. The space is written into the score instead.

- **Sweeps are chosen by measuring, never by name.** `--sweep-scan` renders a
  param at five points across its range and reports `shape` (normalised
  spectrum movement) and `level_db` separately, because either alone lies:
  without normalising, gain controls win everything; with normalising alone, a
  param that MUTES reads as inert. Level-ish keys are excluded by name — the
  one place a name is trusted, because sweeping `mix` is the effect fading in
  and out. `audio_fx/filter-eq` sweeps its best candidate whatever it scores.

- **Bass plays an octave down**, from the taxonomy: the `mono-bass`
  subcategory, overridden by the `bass` tag so tb3po gets it too.

- **Every failure is a status, never a placeholder**: `needs-assets`,
  `not-applicable`, `no-screens`, `js-only`, `error`.

## Traps that cost real time here

Read these before changing the measurement code.

1. **A ZEROED `host_api_v1_t` IS NOT SAFE.** "Every module guards
   `if (host->fn)`" covers function pointers only; `sample_rate` is a plain
   `int`. With it at 0, 4k-eq passed audio at its defaults and fell silent the
   moment any FLOAT param changed (a biquad recomputing `2*pi*freq/0` → NaN).
   Enums were fine because they touch no coefficients. **Enums fine + floats
   dead is not a plausible module defect; it is a missing sample rate.** I
   filed it as a module bug first. It was mine.

2. **`if (o == SOME_SAMPLE)` inside a `o += BLK` loop never fires** unless the
   sample is a multiple of BLK. This cost THREE separate measurements — a
   fleet-wide "note-off doesn't work" finding, and every release reading as
   infinite. Test the interval: `o <= t && o + BLK > t`.

3. **A module keeps state across presets and across measurements.** The
   previous preset's tail is still ringing when the next is measured (Sub Bass
   read 0.10 s alone, 4.00 s in sequence), and the measurements' own notes rang
   into the start of the clip (braids' Pad opened at −10.9 dBFS and FELL, where
   its attack rises from −74). `settle()` before anything that measures or
   renders.

4. **Do not measure pitch on polyphonic or melodic material.** Autocorrelation
   finds a composite period, not the note. I twice reported numbers from it and
   twice they were meaningless. Hold ONE note.

5. **A sweep must restore its parameter.** Without it every later scan runs on
   top of the previous one pinned at maximum — five of 4k-eq's bands scored
   exactly `0.000`, masked rather than inert. An exact `0.000` is the tell.

6. **`_all` is a BASE the subcategory refines**, not an alternative it
   replaces.

7. **Whole-clip RMS cannot see a module that dies partway through** — a loud
   opening carries the average. Silent *fraction* is also wrong (the score is
   34% rests by design). What distinguishes it is one long UNBROKEN stretch:
   `dead_seconds > 4`.

8. **Pass `--widgets` when the module ships `canvas.js`**, or its own graphics
   fall through to plain dials — a correct fallback and a wrong picture, with
   nothing logged. hank draws an FM waveform across three cells; monksynth a
   formant shape.

## Open

- **Screenshots are a thumbnail, not the hero.** Measured: 52% of a knob page
  is identical across every module and the closest pair differs by 4%. Tested
  two generated alternatives and both were WORSE (waveform 90.6% identical,
  spectrogram 86.7%). The card should lead with audio; a genuinely distinctive
  primary image has to come from the author.
- **Cards are never rendered.** `preview.mjs` has no card support at all, so no
  module's `card_script` surface appears. Monksynth's twelve faces are the
  case.
- **The QuickJS screen pass** for the 28 overtake/tool modules — the one
  unproven piece. They currently get `js-only`.
- **Drum machines have no score.** `sound_generator/drum-machine` is
  `score: null`; it needs one built from the module's declared voices.
- **mverb and tablor render silent** and declare zero params in `module.json`,
  so they need a `preview.json`. Worth re-checking now that `sample_rate` is
  set — that fix may have changed them.
- **Publishing.** `schwung-preview-assets` does not exist yet; Charles creates
  it and then the CI force-pushes a single commit to it.
- `audio/normalize.sh` in the catalog-site repo needs `-ar 44100`: **all 17
  live previews on schwung.dev are 96 kHz** because `loudnorm` resamples
  internally and ffmpeg then picks 96 k for AAC.

## Module bugs found (upstream, not ours)

- **monksynth**: `module.json` declares `card_script: "cards.js#vowel_card"`
  but ships no `cards.js`; the drawer is `globalThis.vowel_card` in
  `canvas.js`, and canvas.js's own comment says the declaration should read
  `canvas.js#vowel_card`. The host caches the failed load, so the face never
  draws on device either.
- **4k-eq was NOT a bug** — see trap 1. Retracted.

## Rebuilding the demo page

The catalog mock-up lived in a session scratchpad and is gone. It was
`catalog.html`/`style.css` from the catalog-site repo with `index.json` wired
in: hero audio, preset chips, screenshot as a 96px thumbnail. Cheap to rebuild
from `build/previews/index.json`; nothing depends on it.
