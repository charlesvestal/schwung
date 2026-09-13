# Schwung on the desktop

Schwung's chain host, driven from an ordinary audio callback instead of from
the SPI transfer — as a VST3 / AU plugin for Ableton Live, and as a standalone
app and an offline renderer.

Design and rationale: `docs/plans/2026-09-13-schwung-vst-port.md`.

## What this is

`src/schwung_shim.c` is the only Move-specific layer in Schwung. Everything
above it — the chain, the slots, the LFOs, the buses, Master FX — is portable
C, and `src/shadow/shadow_ui.c` is a separate process that talks to the audio
side over shared memory alone. This directory is a **replacement for the shim**
and nothing else: `chain/dsp.so` is dlopen'd here exactly as the shim dlopens
it on the device.

One plugin instance is **one chain** (MIDI FX → synth → 8 FX), not the whole
four-slot device. Live's own tracks and returns do what slots and sends do.

## Build

```bash
# 1. The chain host, the bundled FX, and any module repos checked out beside
#    this one. Produces build/desktop/modules/ + build/desktop/schwung-render.
desktop/build.sh

# 2. The plugin. JUCE is not vendored.
git clone --depth 1 https://github.com/juce-framework/JUCE.git /path/to/JUCE
cmake -B build/plugin -S desktop -DJUCE_DIR=/path/to/JUCE -DCMAKE_BUILD_TYPE=Release
cmake --build build/plugin --config Release -j8
```

VST3, AU, Standalone and the render test land in
`build/plugin/SchwungPlugin_artefacts/Release/`. The VST3 and AU are copied
into `~/Library/Audio/Plug-Ins/` automatically.

Live hosts VST3 and AU and **not CLAP**, which is why JUCE is the wrapper.

## Install the modules

The plugin looks for a module tree, in this order:

1. `$SCHWUNG_MODULES`
2. `~/Library/Application Support/Schwung/modules` (macOS)
3. nothing — and it says so in the window rather than coming up silent

```bash
rsync -a --exclude '*.dSYM' build/desktop/modules/ \
      ~/Library/Application\ Support/Schwung/modules/
```

The tree mirrors the device layout exactly, `.so` suffix included:

```
modules/chain/dsp.so
modules/sound_generators/<id>/dsp.so
modules/audio_fx/<id>/<id>.so
```

The chain host builds sub-module paths as `"%s/../sound_generators/%s/dsp.so"`.
`dlopen` on macOS does not care what a Mach-O file is called, so the dylibs are
simply named `.so` rather than teaching every path construction in
`chain_host.c` and `chain_bus.c` a platform suffix. The two platforms never
share an install tree.

## Verify

```bash
# The chain, with no DAW, no GUI and no audio device in the way.
./build/desktop/schwung-render --modules build/desktop/modules \
    --synth braids --fx freeverb -o /tmp/out.wav

# The PLUGIN, at 48 kHz in 512-frame blocks -- i.e. entirely through the rate
# bridge. Fails on silence, NaN or clipping.
./build/plugin/schwung-plugin-test_artefacts/Release/schwung-plugin-test

# The AU, as the OS sees it.
auval -v aumu Schw Schw
```

**`auval` and `pluginval` both pass on a plugin that renders pure silence.**
Neither can tell a well-formed wrapper from one that is connected to anything,
which is what `schwung-plugin-test` is for.

## Things that will bite

- **The chain runs at 44100 Hz in 128-frame blocks, always.** Sixteen hardcodes
  in the chain host alone, plus every module's filter coefficients. `RateBridge`
  converts at the boundary; nothing below it ever sees the host's rate. Do not
  try to make the fleet rate-aware.

- **`sample_rate` in `host_api_v1_t` must never be left 0.** It is a plain int,
  so no `if (host->fn)` guard covers it. A module dividing `2*pi*f/0` produces
  NaN and silence *while its enum parameters still work*, which reads as a
  module defect rather than a missing field.

- **The read key is not the write key.** A synth is set with `synth:module` and
  read back with `synth_module`. A `get_param` returning −1 is a **failed read**,
  never "nothing is loaded" — see the three-answer rule in `CLAUDE.md`.

- **A macro's range is read from the module's `chain_params`, never guessed.**
  A key that declares no range is refused, and the editor says why. Falling back
  to 0..1 would write a plausible wrong number into a dB or Hz parameter, which
  is worse than not binding because it makes a sound.

- **`userApplicationDataDirectory` is `~/Library` on macOS**, not
  `~/Library/Application Support`.

## Not here yet

- **The 128×64 shadow UI.** Task 0.8 of the plan. The plugin window binds macros;
  it does not draw Move's screens. That needs `shadow_ui.c`'s process and its
  shared-memory protocol ported, which is the largest remaining piece.
- **Line-input modules.** `mapped_memory` is NULL, so vocoder, talkbox, gate,
  ducker and NAM have no input. Phase 1 fills a synthetic SPI mailbox from the
  plugin's input bus.
- **Set import.** No bundle format yet; the chain comes up empty and is built
  from the window.
- **Windows.** No `fork`, no POSIX shared memory, no `dlopen`.
