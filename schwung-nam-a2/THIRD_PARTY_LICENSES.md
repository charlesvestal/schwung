# Third-party licenses

This module links against, and adapts algorithms from, the projects listed
below. Their licenses are honored separately from this module's own MIT
license (see `LICENSE`).

## NeuralAudio (neural amp modeling engine)

- **Project:** [`mikeoliphant/NeuralAudio`](https://github.com/mikeoliphant/NeuralAudio)
- **Author:** Mike Oliphant
- **License:** MIT
- **How it's used:** Statically linked as `libNeuralAudio.a` (built from
  `deps/NeuralAudio`). Runs `.nam` / `.aidax` models loaded by this plugin.

## RTNeural

- **Project:** [`jatinchowdhury18/RTNeural`](https://github.com/jatinchowdhury18/RTNeural)
- **Author:** Jatin Chowdhury and contributors
- **License:** BSD 3-Clause
- **How it's used:** Statically linked via NeuralAudio's submodule.

## NeuralAmpModelerCore

- **Project:** [`sdatkinson/NeuralAmpModelerCore`](https://github.com/sdatkinson/NeuralAmpModelerCore)
- **Author:** Steven Atkinson
- **License:** MIT
- **How it's used:** Headers pulled in via NeuralAudio for `.nam` model
  loading.

## Eigen

- **Project:** [Eigen](https://eigen.tuxfamily.org/)
- **License:** MPL2
- **How it's used:** Header-only, pulled in transitively via RTNeural.

## math_approx

- **Project:** [`jatinchowdhury18/math_approx`](https://github.com/jatinchowdhury18/math_approx)
- **Author:** Jatin Chowdhury
- **License:** BSD 3-Clause
- **How it's used:** Header-only fast-math approximations, pulled in
  transitively via NeuralAudio/RTNeural (`LSTM_MATH`/`WAVENET_MATH=FastMath`).

## Freeverb algorithm (Reverb stage of the solo FX chain)

- **Project:** Freeverb, originally by Jezar at Dreampoint
- **License:** Public domain
- **How it's used:** The Reverb stage's comb-filter / allpass-filter network
  and delay-line tuning tables are adapted from the classic Freeverb
  algorithm, following the same adaptation already shipped as this host's
  own built-in `freeverb.c` audio FX module. Not a NeuralAudio dependency -
  original C++ implementation written for this module.

## schwung-nam (module this plugin is based on)

- **Project:** [`charlesvestal/schwung-nam`](https://github.com/charlesvestal/schwung-nam)
- **Author:** Charles Vestal
- **License:** MIT
- **How it's used:** This module's NAM model loading, cabinet IR WAV parsing
  and time-domain convolution, and background model-loader thread are
  carried over near-verbatim from schwung-nam's `nam_plugin.cpp`, per its
  MIT license. The 3-band EQ, Quality (Full/Lite) switch, and the
  Doubler/Echo/Reverb solo FX chain are new code written for this module.

## No bundled models or cabinet IRs

Unlike some community NAM module builds, this module ships with empty
`models/` and `cabs/` directories - no third-party `.nam` captures or
`.wav` impulse responses are bundled, so no additional data licenses apply
to the release tarball. Users add their own files via the Schwung Manager
web UI or `scp`.
