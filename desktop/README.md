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

One plugin instance is **one chain** (8 MIDI FX → synth → 8 audio FX), not the
whole four-slot device. Live's own tracks and returns do what slots and sends
do.

Working today:

- All 16 chain positions, picked from the window.
- **512 automatable macros**, auto-bound to the loaded module's own parameters
  and carrying its names into the host's automation list. The bank is sized
  for the largest module in the fleet (minijv, 433 parameters), not the median.
- A Live set that reopens **sounding the same** — the module's own opaque
  `state` blob is saved per position.
- **MIDI realtime clock** synthesised from the playhead, without which every
  tempo-synced module is silent.
- **Line input** via a sidechain, for the modules that read the SPI mailbox.
- Any host rate and block size; `auval` exercises 11025–192000 Hz at 64–4096
  frames.

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

## Module data, and the /data mirror

21 of 78 module repos hardcode device-absolute paths — overwhelmingly Move's
user library:

```
/data/UserData/UserLibrary/Wavetables   10 uses
/data/UserData/UserLibrary/Samples       6
/data/UserData/UserLibrary/Tablor        3
```

plus per-module stores like breakbeat's. These are not a configuration
mistake — on the device that path is simply where content lives — so the fix
is to make the path exist here rather than to patch 21 repos and every future
one.

```bash
desktop/setup-data-mirror.sh          # builds the tree, prints the one
                                      # privileged step (it does not run it)
desktop/setup-data-mirror.sh --check  # status
```

macOS's root filesystem is read-only and SIP-protected, so `/data` cannot be
`mkdir`'d. `/etc/synthetic.conf` is Apple's supported mechanism for declaring
symlinks at `/`, and `apfs.util -B` applies it without a reboot. The mirror
target deliberately contains **no spaces**: `synthetic.conf` is tab-separated
and a path with a space in it is a good way to get a silently ignored line.

## Verify

```bash
# The chain, with no DAW, no GUI and no audio device in the way.
./build/desktop/schwung-render --modules build/desktop/modules \
    --synth braids --fx freeverb -o /tmp/out.wav

# --play is not optional for anything tempo-synced (see below).
./build/desktop/schwung-render --modules build/desktop/modules \
    --synth breakbeat --play --bpm 120 \
    --set synth:A_sample_path=/path/to/loop.wav -o /tmp/beat.wav

# A line-input module, fed a tone straight into the mailbox.
./build/desktop/schwung-render --modules build/desktop/modules \
    --synth linein --in-tone -o /tmp/thru.wav

# Ask the chain a question. --get keeps the three answers apart.
./build/desktop/schwung-render --modules build/desktop/modules \
    --synth braids --get synth:chain_params --get synth:state

# Every installed module, loaded in a real chain, reported by level.
desktop/smoke-test.sh

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

- **Setting the host transport is NOT the same as sending a clock, and a
  tempo-synced module can only see the second.** The chain overrides
  `get_clock_status` with its own (`chain_midi.c`), answered from MIDI
  realtime bytes it has actually received — `0xFA` / `0xF8` / `0xFC`. On the
  device the shim broadcasts Move's clock into every slot; the plugin
  synthesises it from the playhead in `TransportClock`. `get_bpm` and
  `get_beat_position` feed the chain's own LFOs and are invisible to this.
  breakbeat with the transport set but no bytes: `clock_status=1`, silence.
  With the bytes: `clock_status=2`, −19.6 dBFS.

- **A module can be silent for more than one reason at a time.** breakbeat
  needed the clock *and* a sample, and fixing only the first leaves it looking
  exactly as broken as before.

- **With one input bus and one output bus, JUCE lays them over the SAME buffer
  channels.** So `buffer.clear()` at the top of `processBlock` zeroes the
  sidechain before it can be read, and a line-input module hears silence with
  everything correctly routed. Only the output bus is cleared, and only after
  the input has been taken.

- **The input FIFO is primed with one block of silence on purpose.** `pull()`
  keeps one Schwung block buffered ahead, so input consumption permanently
  leads supply by that block — a deficit the steady state can never repay,
  leaving every block short and reading as silence. Priming turns it into
  ~2.9 ms of input latency.

- **Input is published BEFORE `render_block`.** A module reads the mailbox
  *during* the render, so filling it afterwards delivers every block one late:
  a fixed lag that a signal-present check cannot see.

- **A macro adopts the module's current value; it never pushes its own.**
  Auto-binding that wrote macro defaults outward slammed a freshly loaded
  module to its minimums. The exception is a state restore, where the saved
  values win and are pushed in — the two directions want opposite answers.

## Not here yet

- **The 128×64 shadow UI.** The plugin window picks modules and binds macros;
  it does not draw Move's screens. `shadow_ui.c` is already a separate process
  talking only over shared memory, but the nineteen segment names are
  compile-time constants — fine for one host per machine, wrong for one per
  track. Reimplementing `shadow_shm_map()` as a per-instance allocator makes
  the whole UI instance-safe without editing it.
- **Repeated module swaps in one session** are untested. `dlclose` is a no-op
  for a C++ module carrying unique symbols, and the chain reinstantiates on
  every `:module` write.
- **Automation resolution.** Macros are written once per host block, so a fast
  envelope is stepped at block rate rather than smoothed. That may be fine; it
  should be a measured decision rather than an accident of where the write
  happens.
- **Set import.** No bundle format yet; the chain comes up empty and is built
  from the window.
- **Windows.** No `fork`, no POSIX shared memory, no `dlopen`.
