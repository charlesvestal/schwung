# Schwung as an Ableton Live plugin — Feasibility & Plan (2026-09-13)

Status: **built and running** (2026-09-14). Phases 0 and 1 are done; the
plugin loads in Live, makes sound, and automates. macOS, VST3 + AU.
Implementation in `desktop/` — see `desktop/README.md` for how to build it and
what will bite. This file is the plan and the record of what it cost.

Goal: a plugin that loads Schwung modules inside a Live set, so a chain built on
Move keeps working after a transfer — parameters, LFOs, buses and the 128x64
screens included.

Companion artifact (same content, diagrams drawn):
<https://claude.ai/code/artifact/728657d4-8be5-4f8b-ae36-0f9d21e14b4d>

## The thesis: only the bottom layer is Move-specific

`src/schwung_shim.c` owns the SPI transfer, the mailbox mixing, Link Audio and
the master-volume scan. **That file is the hardware.** Everything worth carrying
into Live sits above it in ordinary POSIX C and QuickJS: the four slots, the
chain host, the LFOs, the buses, Master FX, the param pages, the whole `.mjs` UI
tree.

```
        ON MOVE                             IN LIVE

   shadow_ui process                   shadow_ui process      <- unchanged
          | SHM x12                           | SHM x12       <- unchanged
   +------------------+               +------------------+
   | chain host       |               | chain host       |    <- unchanged
   | slots/LFOs/buses |               | slots/LFOs/buses |       (modules rebuilt)
   +------------------+               +------------------+
   | schwung_shim.c   |               | VST3/AU wrapper  |    <- THE ONLY SWAP
   +------------------+               +------------------+
          | ioctl                             | processBlock
   Move hardware                       Live
   44.1 kHz / 128 fr                   48 kHz / N fr         <- resample + reblock
```

The UI is **already a separate process**: `src/shadow/shadow_ui.c` has its own
`main()` (`:3504`) and its own QuickJS runtime (`:3119`), and speaks to the audio
side exclusively over named SHM (`:75-108`). A plugin that takes over the shim's
half of that protocol inherits the entire user interface with **no changes to
it**. That is the largest surface area of the project and it comes free.

## Port ledger

| Component | State | Why |
|---|---|---|
| The 128x64 UI | **ports** | Separate process, own runtime, SHM-only. All 19 segment names in `shadow_constants.h` are <= 29 chars, inside macOS's 31-char `PSHMNAMLEN`. Blit `js_display_screen_buffer` (`src/host/js_display.c:26`). |
| LFOs, buses, sends, Master FX, bypass, snapshots | **ports** | Plain C in the chain host. No hardware dependency. |
| Set / slot state | **ports** | `set_state/<uuid>/` is JSON. A bundle is a zip plus sample-path rewriting. |
| Host API surface | rebuild | A desktop host serves 6 of `host_api_v1_t`'s fields and NULLs the Move-specific ones. The `reserved[8]` tail already makes NULLing safe. |
| 139 catalog modules | rebuild | All aarch64 Linux `.so`. Swappable prefix (below), but ~100 repos x 3 platforms, forever. |
| Sample rate / block size | rebuild | 44100/128 assumed throughout — 16 hardcodes in `src/modules/chain/dsp/*.c` alone, plus every module's coefficients. |
| Automation | rebuild | Live wants a static param list at instantiation; Schwung params are runtime-discovered string keys. New subsystem, no device equivalent. |
| Overtake modules | **stays** | They *are* the control surface. No pads, steps or LEDs, nothing left. |
| Link Audio rebuild, speaker EQ | **stays** | Both exist to reconstruct and re-colour Move's own audio. Live hands you the track. |
| Skipback, quantized sampler, metronome | **stays** | The DAW does all three natively. |

## Decisions taken

1. **One chain per plugin instance, not the whole four-slot device.**
   MIDI FX -> synth -> 8 FX on one Live track; sends and Master FX become
   instances on returns and master. A transferred set unpacks into four tracks
   plus two returns. Costs a more elaborate import, buys Live-native routing,
   per-track automation, and the ability to use one chain without the other
   three. **Reversible, but only cheaply before task 1.4 is written.**
2. **Fixed 44100/128 internally, converted at the boundary.** Costs a block of
   latency and a resampler; buys behaviour bit-identical to the device. Making
   139 modules sample-rate-aware is not a project anyone finishes.
3. ~~**Keep the two-process split.**~~ **SUPERSEDED.** The original plan was to
   spawn `shadow_ui` as a child exactly as the device does. That cannot work
   as written: the nineteen SHM segment names in `shadow_constants.h` are
   compile-time constants, so two plugin instances — an ordinary Live set with
   two Schwung tracks — attach to the same `/schwung-control` and drive one
   screen between them. The replacement is to reimplement `shadow_shm_map()`
   as a **per-instance allocator**, which makes the whole UI instance-safe
   without editing `shadow_ui.c` at all and drops the child process with it.
   See Open questions.
4. **macOS first; Windows is a separate project.** Live hosts VST3 and AU but
   **not CLAP**, so the wrapper is JUCE. Windows has no `fork`, no POSIX shm and
   no `dlopen` — that is the bottom layer written a third time, not a build flag.

## What a desktop host must serve

`host_api_v1_t` (`src/host/plugin_api_v1.h`) is a small surface. From the DAW
transport: `sample_rate`, `frames_per_block`, `log`, `get_bpm`,
`get_beat_position`, `get_clock_status`. NULL: `midi_send_internal` (LEDs),
`midi_inject_to_move`, `slot_recv_channel` (partial). Callers already guard
every field as `if (host->fn)`, and the `reserved[8]` tail absorbs a module
compiled against a longer header — so NULLing is the designed failure mode, not
a gamble.

One trick worth taking: `mapped_memory` / `audio_in_offset` can be a
**synthetic 768-byte SPI-layout mailbox** whose audio-in region is filled from
the plugin's input bus each block. Then vocoder, talkbox, gate, ducker and NAM
work unmodified.

## The fleet

107 module repos checked out under `schwung-parent/`; 78 have a `src/dsp`
(39 plain C, 39 C++), 33 use CMake, 49 carry submodules.

The build scripts are better than expected. `schwung-braids`,
`schwung-cloudseed` and `schwung-dx7` all read:

```sh
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
${CROSS_PREFIX}g++ -O3 -shared -fPIC ...
```

so a desktop build is `CROSS_PREFIX=""` plus output-name and flag changes
(`-shared` -> `-dynamiclib`), and `chain_host.c:529`'s hardcoded `"%s/dsp.so"`
needs a platform suffix resolved **in one place**, not per call site.

Won't port, and not for compiler reasons: **jv880** (ROMs — licensing, not
source), **jp8000** (fork-parallel children plus a device-wide flock),
**airwindows** (it hosts arbitrary aarch64 Linux CLAP binaries),
airplay, radiogarden, webstream, norns, pipewire, jack.

## Phases

Phase 0 exists to be killed cheaply. Everything after it is scope rather than
risk — except the fleet lane, which never ends.

## What was built (0 and 1, done)

- Chain host + 76 modules build natively on macOS arm64; **no module repo was
  edited** — `desktop/port-module.sh` supplies shim compilers and each repo's
  own `build.sh` does the rest.
- VST3 / AU / Standalone. `auval` SUCCEEDS, including render at 11025–192000 Hz
  and 64–4096 frame blocks.
- 30 of 47 installed modules verified making sound; the rest are asset-limited
  (`desktop/smoke-test.sh` reports it).
- 512 auto-bound macros carrying module parameter names into Live.
- All 16 chain positions; state persistence; MIDI realtime clock; line input.
- Two plugin instances in one process, verified independent.

Still open in phase 1: repeated module swaps in one session are untested, and
the automation write happens once per host block (a fast envelope is stepped
at block rate).

**The next piece is the shadow UI**, and its blocking decision is recorded
under Open questions below.

### Phase 0 — Vertical slice (DONE)

One chain, three modules, audio + screen + one automated macro, in Live on macOS.

- **0.1** Revive a non-device host build. `src/host/pcaudio_stub.c` and
  `src/schwung_host.c:2161`'s `main()` are an existing off-device entry point.
  Expect bit-rot — per memory, `main()` has not run on device in a long time.
  **This is the first real unknown and it is first for that reason.**
- **0.2** Port the SHM layer (`src/host/shadow_shm_util.c`). `shm_open` works on
  macOS; verify unlink semantics and that the child attaches.
- **0.3** Desktop `host_api_v1_t` provider (see above).
- **0.4** Platform-suffix the module loader (`chain_host.c:529`), one place.
- **0.5** Desktop builds of freeverb, braids, sh101 — pure C, no submodules,
  all honour `CROSS_PREFIX`. **Produce the shared `build-desktop.sh` here**; the
  fleet adopts it in phase 3.
- **0.6** Fixed-rate audio bridge; report added latency for delay compensation.
- **0.7** JUCE VST3/AU shell — audio I/O, MIDI in, transport.
- **0.8** The OLED in a window. Scale the 128x64 buffer; map mouse/keyboard onto
  encoder CCs 71-78, jog (CC 14 / 3), Shift 49, Menu 50, Back 51.
- **0.9** One macro automated end to end. Deliberately one.

**Exit criteria:** load a `slot_N.json` copied off the device, hear it, drive it
from the on-screen OLED, automate one param from a clip envelope, hold a
128-sample buffer with no dropouts. If any of that resists, stop — the cost of
finding out was one slice, not one fleet.

### Phase 1 — Host parity (DONE except 1.5, 1.6)

- **1.1** **Serve params off the audio thread.** On Move every entry point *is*
  the SPI callback, and the 2026-08 audit found ~150 confirmed RT violations
  across the fleet. In a DAW those are someone's dropout. The desktop host is
  free to do what the device cannot.
- **1.2** Load and instantiate on a worker — reuse `chain_bus.c` /
  `fx_load_gate.h` (sequence number; close the gate *before* the publish).
- **1.3** Synthetic mailbox for `mapped_memory` (see above).
- **1.4** Bundle format + importer: set JSON, presets, referenced samples, and a
  module id/version manifest. **A missing module must fail by name** — a chain
  that loads silently with a hole in it is undiagnosable from the DAW.
- **1.5** Persist into the Live set, not a `/data/UserData` equivalent.
- **1.6** Decide the accessibility path — whether `shadow_screenreader` drives
  anything on desktop, or the plugin uses the platform API.

### Phase 2 — The macro layer (new subsystem)

- **2.1** Fixed bank of macros exposed at instantiation (VST3/AU requirement).
- **2.2** Assign/learn: bind a macro to a runtime key (`slot2:fx1:cutoff`).
- **2.3** Persist bindings; survive a module swap gracefully.
- **2.4** Range mapping read from the module's own `chain_params` — dB or
  linear. **Not found means refused, never a guessed scale** (same rule as the
  voice sends).
- **2.5** Settle LFO exposure. Internal LFOs work untouched; automating *them*
  means giving rate/depth macros of their own.

### Phase 3 — The fleet (ongoing, unbounded)

- **3.1** Shared `build-desktop.sh` + a CI matrix job each repo adopts.
- **3.2** Sweep the 39 C-only repos first — cheapest, and they prove the script.
- **3.3** Then C++/CMake (toolchain-file split) and the 49 submodule repos.
- **3.4** Triage and publish the won't-port list, so the plugin can say **why** a
  module is absent.
- **3.5** **A missing desktop artifact must fail the release, not warn.** This
  repo's own history is unambiguous: the link-subscriber sidecar and the manager
  binary both rode through weeks of deploys because a build step could be
  skipped in silence. A build step that can be skipped silently defeats every
  bisect that follows.

### Phase 4 — Windows (separate project)

Only once macOS is real. Win32 layer under `shadow_shm_util.c`, `LoadLibrary`
for the loader, a thread where the device uses a child process.

## What it actually cost

Nine defects, and they cluster into two shapes — a value that looks guarded
and is not, and a failure that produces no signal. Recorded because every one
of them was found by USING the thing, not by reading it.

| What happened | The rule |
|---|---|
| `mapped_memory` NULL → vocoder SIGSEGV | It is a pointer to *data*, not a callback, so the `if (host->fn)` convention never covered it. In a DAW that is the host going down with the set unsaved. |
| Auto-binding slammed every parameter to its minimum | A new binding must **adopt** the module's value, never push its own default. |
| …and the fix then ate a restored set's macro values | Restore and fresh-bind want opposite answers; one code path cannot serve both without being told which. |
| Host callbacks answered per *process* | `get_bpm(void)` has no context. Thread-local, because a DAW renders tracks on several threads at once. |
| `_GNU_SOURCE` placed beside the header it was for | glibc latches `__USE_GNU` at the first libc header. Three implicit declarations linked clean and would have failed at `dlopen` on the Move. |
| `sem_init` returns ENOSYS on macOS | Not deprecated — unimplemented. The bus worker would never start and nothing would say so. |
| Audio FX filed under `sound_generators/` | `component_type` lives in the catalog, not `module.json`. Installed, looks installed, cannot be picked. |
| Built inside a module repo | Left a Mach-O at `dist/<id>/dsp.so` — the file that gets packaged and shipped to a Move. |
| Setting the transport is not sending a clock | The chain answers `get_clock_status` from MIDI realtime bytes it received. Every synced module was dead. |

And two about instruments rather than code:

- **`auval` and `pluginval` both pass on a plugin that renders pure silence.**
  Neither can tell a well-formed wrapper from a connected one.
- **RMS over a fixed render is not reproducible**, so it cannot decide whether
  a restored instance "sounds the same". The same processor rendered twice
  gave −24.2 then −19.2 dBFS; braids' phase and freeverb's tail carry across.
  The state blob is the reproducible instrument. A positive control is what
  established that, and it is why the save/restore test compares what it does.

## Risks

- **Resampling audibly changes modules.** The fixed-rate bridge assumes boundary
  conversion is transparent. For anything with aliasing character or
  sample-accurate transients it may not be, and the fix would be per-module.
- **RT violations bite harder than expected.** Serving params off the audio
  thread handles most of it; a module blocking inside `render_block` itself has
  nowhere to hide in a DAW.
- **The fleet lane never converges** unless desktop builds are automatic. Left
  manual, the plugin's module set drifts behind the device's and the promise
  quietly stops being true.
- **One chain per instance may be the wrong call** if a faithful picture of the
  device is what's actually wanted.
- **Nobody has checked that the off-device host still builds** (task 0.1).

## Open questions

- **The shadow UI's shared memory — allocator or namespaced segments?** This is
  the one blocking phase 2. `shadow_ui.c` only ever reaches the audio side
  through `shadow_shm_map()`, so reimplementing that one function as a
  per-instance allocator makes the UI instance-safe with no edits to it and no
  child process. The alternative — keeping real SHM but suffixing the names per
  instance — preserves the process boundary (a crash in the UI would not take
  the host down) at the cost of touching `shadow_constants.h`, which the device
  shares. Nothing is decided; the allocator is the current preference.
- Does a bundle carry samples by value (portable, large) or by reference
  (small, breaks when the project moves machines)?
- Should the plugin read a set directly off a Move over the network, or is
  export-then-import the whole story?
- Is there a version handshake between a device set and a plugin build, or does
  a mismatched module version simply refuse to load?
- Does the plugin ship modules, or install them — and if the latter, is that
  schwung-manager again, or something local?
