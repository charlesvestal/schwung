# Nam A2

Neural Amp Modeler audio effect module for [Schwung](https://github.com/charlesvestal/move-everything)
on Ableton Move, based on [schwung-nam](https://github.com/charlesvestal/schwung-nam)
by Charles Vestal. Adds a 3-band EQ, a Full/Lite quality switch, and a
"solo" FX chain (Doubler / Echo / Reverb) inspired by the layout of the
Solar Guitars CHUG SOLO pedal.

Built on [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) by Mike
Oliphant and [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore)
by Steven Atkinson.

## Prerequisites

- Schwung installed on your Ableton Move

## Signal chain

```
Input Gain -> NAM Model -> Cab IR -> 3-Band EQ -> Doubler -> Echo -> Reverb -> Output Gain
```

Doubler, Echo and Reverb are each independently bypassable and default to
**bypassed** - they're an occasional "kick it in for the solo" chain, not
an always-on coloration.

## Features

- **Neural amp/effect modeling**: run trained `.nam` / `.aidax` models for
  realistic amp and pedal emulation.
- **Full / Lite quality switch**: Lite halves the neural net's per-block
  work (processes at half rate, holds each output sample for two frames) to
  free up CPU headroom on Move's ARM core when a heavy model plus the cab
  IR, EQ and solo FX chain below would otherwise miss the real-time budget.
  Measure with the CPU page (`docs/DIAGNOSTICS.md` in the host repo) before
  relying on it - it's a genuine tradeoff, not free.
- **Cabinet IR convolution**: apply cabinet impulse responses with optional
  bypass.
- **3-band EQ**: independent gain + frequency for Low (shelf), Mid (bell)
  and High (shelf) bands, sitting after the cab IR.
- **Solo FX chain**:
  - **Doubler** - a subtle modulated-delay chorus that thickens a single
    note into a "doubled" line, with a Mix control.
  - **Echo** - a filtered delay (repeats darken progressively, like a tape
    echo) from short slap-back up to ~2 seconds, with Feedback, Filter and
    Mix controls, plus tap-tempo (tap twice or more on the beat to set the
    time).
  - **Reverb** - a small room emulation (Freeverb-derived comb/allpass
    network) with Room Size, Damping and Mix controls.
- **Model / cabinet browsers**: hierarchical file browsers for selecting
  `.nam` model files and `.wav` cabinet IRs.
- **Input/Output level**: independent gain staging controls.

## Installation

```bash
./scripts/build.sh
./scripts/install.sh
```

## Parameters

| Parameter | Range | Default | Description |
|-----------|-------|---------|-------------|
| `input_level` | 0.0-1.0 | 0.5 | Input gain before model processing |
| `output_level` | 0.0-1.0 | 0.5 | Output gain after the whole chain |
| `quality` | Full / Lite | Full | Neural net CPU/quality tradeoff |
| `cab_bypass` | 0-1 | 0 | Bypass cabinet IR convolution |
| `eq_low_gain` / `eq_low_freq` | -15..15 dB / 40..500 Hz | 0 dB / 100 Hz | Low shelf |
| `eq_mid_gain` / `eq_mid_freq` | -15..15 dB / 200..4000 Hz | 0 dB / 800 Hz | Mid bell |
| `eq_high_gain` / `eq_high_freq` | -15..15 dB / 1000..10000 Hz | 0 dB / 3000 Hz | High shelf |
| `doubler_bypass` | 0-1 | 1 (bypassed) | Doubler on/off |
| `doubler_mix` | 0.0-1.0 | 0.35 | Doubler wet mix |
| `echo_bypass` | 0-1 | 1 (bypassed) | Echo on/off |
| `echo_time_ms` | 50-2000 ms | 350 ms | Delay time |
| `echo_feedback` | 0.0-0.9 | 0.35 | Repeat count |
| `echo_filter_hz` | 400-8000 Hz | 3000 Hz | Feedback-path lowpass (darkens repeats) |
| `echo_mix` | 0.0-1.0 | 0.3 | Echo wet mix |
| `echo_tap` | momentary | - | Tap on the beat (2+ taps) to set `echo_time_ms` |
| `reverb_bypass` | 0-1 | 1 (bypassed) | Reverb on/off |
| `reverb_room` | 0.0-1.0 | 0.5 | Room size |
| `reverb_damping` | 0.0-1.0 | 0.5 | High-frequency damping |
| `reverb_mix` | 0.0-1.0 | 0.3 | Reverb wet mix |

## Adding Models and Cabinets

Place `.nam` model files and `.wav` cabinet IRs in the module directory on
your Move (or via the Schwung Manager web UI at `move.local:7700`):

```
/data/UserData/schwung/modules/audio_fx/nam-a2/models/
/data/UserData/schwung/modules/audio_fx/nam-a2/cabs/
```

NAM models can be trained with the
[Neural Amp Modeler Trainer](https://github.com/sdatkinson/neural-amp-modeler).
Find pretrained models at [tone3000.com](https://tone3000.com).

## Building

```bash
./scripts/build.sh      # Build for ARM64 via Docker
./scripts/install.sh    # Deploy to Move
```

See `BUILDING.md` in the host repo for cross-compilation details; this
module follows the same "External Module Development" layout described in
the host's `CLAUDE.md`.

## Publishing

This repo ships with placeholder GitHub URLs in `release.json` and
`.github/workflows/release.yml` (`YOUR_GITHUB_USERNAME/schwung-nam-a2`).
Before your first tagged release, push this to your own GitHub repo and
update those placeholders (or just tag `v0.1.0` - `release.yml` rewrites
`release.json` for you on every tag push once the repo exists).

## Credits

- **schwung-nam**: [Charles Vestal](https://github.com/charlesvestal/schwung-nam) (MIT License) - the NAM model loading, cab IR convolution and background loader thread this module builds on.
- **NeuralAmpModelerCore**: [Steven Atkinson](https://github.com/sdatkinson/NeuralAmpModelerCore) (MIT License)
- **NeuralAudio**: [Mike Oliphant](https://github.com/mikeoliphant/NeuralAudio) (MIT License)
- **RTNeural**: [Jatin Chowdhury](https://github.com/jatinchowdhury18/RTNeural) (BSD 3-Clause License)
- **math_approx**: [Jatin Chowdhury](https://github.com/jatinchowdhury18/math_approx) (BSD 3-Clause License)
- **Freeverb**: Jezar at Dreampoint (public domain) - the Reverb stage's comb/allpass network.

## License

MIT License - See `LICENSE` file for details. See `THIRD_PARTY_LICENSES.md`
for the licenses of bundled/linked dependencies.

## AI Assistance Disclaimer

This module was developed with AI assistance. All architecture,
implementation and release decisions should be reviewed by a human
maintainer before publishing - in particular, the DSP has not been
validated on real Move hardware; validate CPU headroom (see
`docs/DIAGNOSTICS.md` in the host repo) and audio correctness before
relying on it live.
