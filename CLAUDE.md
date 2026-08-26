# CLAUDE.md

Instructions for Claude Code when working with this repository.

Schwung is a framework for custom JavaScript and native DSP modules on Ableton Move hardware (pads, encoders, buttons, 128x64 1-bit display, audio I/O, MIDI via USB-A).

Keep this file, `docs/API.md`, `docs/MODULES.md`, and the user manual in `../schwung-catalog-site/manual.html` in sync with code changes (see Release Checklist).

## Code Style

**C**: snake_case. Prefix module manager fns `mm_`, JS host bindings `js_`. Log with `mm:`, `host:`, `shim:` prefixes.
**JavaScript**: `.mjs` = shared ES modules, `.js` = UI modules. Host fns are `snake_case` (`host_load_module`).
**Naming**: Module IDs lowercase-hyphenated (`song-mode`). Param keys lowercase_underscored (`tail_bars`). LED colors PascalCase (`BrightRed`).

## Build / Deploy

```bash
./scripts/build.sh           # Build with Docker
./scripts/package.sh         # Create schwung.tar.gz
./scripts/install.sh         # Deploy from GitHub release
./scripts/install.sh local   # Deploy from local build
./scripts/uninstall.sh       # Restore stock Move
```

**Deploy shortcut**: `./scripts/install.sh local --skip-modules --skip-confirmation` — **never scp individual files**. The install script handles setuid, symlinks, feature config, and service restart.

Cross-compile via `${CROSS_PREFIX}gcc` for Move's ARM. See `BUILDING.md`.

## Testing

Static/regression suite: `for t in tests/{host,shadow,store,build}/*.sh; do bash "$t"; done`
(~95 shell tests: source-invariant pins, compiled C units, node-run .mjs units).
**CI gates the `tests/host/` subset** — `.github/workflows/ci.yml` runs `host-tests`
(`make -C tests/host test` + all `tests/host/*.sh`, all green), `go`
(`schwung-manager`), and `cross-compile` (ARM64 Docker build) on every PR and push
to `main`. `main` is branch-protected: **all three checks are required and direct
pushes are blocked** — work on a branch and open a PR (see `CONTRIBUTING.md`;
install the fast local checks with `./scripts/install-hooks.sh`). The broader
`tests/{shadow,store,build}` suites are **not** run by CI — ~20 stale failures pin
since-moved code (see the cleanup review doc). On-hardware behavior is verified
manually.

**`gh pr merge` reports a failure it did not have when `main` is checked out in
a worktree.** The merge lands on GitHub, then `gh` tries to update the local
checkout and dies with `fatal: 'main' is already used by worktree at ...` —
which also skips `--delete-branch`, leaving the remote branch behind. Confirm
with `gh pr view <n> --json state,mergeCommit` rather than the exit status, and
delete the branch yourself. Re-running the merge on the strength of that error
is the actual hazard.

Enable the unified logger:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
```

JS: `console.log()` (auto-routed) or import `shared/logger.mjs`. C: `LOG_DEBUG("source", "msg")` from `host/unified_log.h`. See `docs/LOGGING.md`.

**On-device E2E tests** (opt-in, not in CI): `tools/pytest-schwung/` is a pip-installable pytest plugin that drives a real Move end-to-end through `schwung-testd`, an opt-in test-bus daemon (TCP loopback, started manually over SSH; built into the tarball but not auto-started). Tests inject MIDI, wait for SPI frames, snapshot pad LEDs, capture MIDI_OUT, and reset to a known-empty set (`pristine_set`). Run `pytest tests/e2e` against attached hardware. Full protocol, fixtures, and hardware pitfalls in `tools/pytest-schwung/README.md`.

**OTLP span tracing** (perf profiling, off by default): `touch /data/UserData/schwung/otlp_trace_on` makes **both** the shim and the `shadow_ui` process emit realtime-safe spans as OTLP/JSONL to `/data/UserData/schwung/traces/`, one file per service (`schwung-shim-*` / `schwung-shadow-ui-*`). Shim: `spi.pre`/`spi.post` roots + `shadow.mix_audio`, `midi.process`, `param.serve` children. shadow_ui: `js.tick` + `param.get`. Spans correlate **cross-process by trace_id** — the shim's `param.serve` is emitted as a child of shadow_ui's `param.get` (context propagated through `shadow_param_t`), so Tempo/Jaeger stitch the two files into one trace. JS modules (overtake/chain, incl. ion) can add spans via `host_trace_begin(name) -> handle` / `host_trace_end(handle)` (shadow_ui context only); balance the pair within one `tick()` (handles come from a 16-entry table reset each `js.tick`). `rm` the file to stop. Zero hot-path cost when off. See `docs/tracing.md`.

**Two more UI perf diagnostics** (both off by default):

- `touch /data/UserData/schwung/param_tally_on` — wraps `shadow_get_param` /
  `shadow_set_param` and reports once a second: reads and writes per tick, the
  keys by frequency with a sampled caller, any single call over 24 ms, and each
  key's value RANGE (`MOVING synth:timbre 0.115 .. 1.000`) so you can tell
  whether a value is actually moving — which is how the idle-LFO fix was
  confirmed. Needs a `shadow_ui` restart to install (it wraps at init). Writes
  `param_tally.txt` once per window. See `src/shared/param_tally.mjs`.
- `param_pages fps: 55 draws / 55 ticks / 1004ms` in `debug.log` while the knob
  grid is up. Draws vs ticks separates the two causes of "dropped frames":
  draws << ticks means something gates the redraw; draws == ticks and both low
  means the tick is slow or its pacing is wrong.

**SPI frame tally** (`touch /data/UserData/schwung/spi_tally_on`, off by
default) reports at ~1 Hz:

```
spi-tally: 345 frames / 345 irq  tx avg 389us (max 447us)  headroom 2513us  backlog 8
spi-tally: LATE 1 irq(s) arrived while busy — frames queued, not dropped (backlog 8, worst window 1)
```

(345/s is right: the block rate is **344.5 Hz**, not 44 — 128 frames per block
at 44.1 kHz, 2.902 ms per block.)

Both numbers come from ablspi itself, which ships in Ableton's GPL source drop
(see `docs/SPI_PROTOCOL.md`). `tx` is the driver's own `spi_tx_time`, stamped
after every transfer into `struct ablspi_sys_info` at the END of the mmap'd page
— one aligned 8-byte load of memory the shim already maps, no syscall. `irq` is
`/proc/ableton/ablspi0.0/irq_count`, read on the worker because it is file I/O.

**The gap between them is the point, and it exists because ablspi's IRQ is a
counting semaphore rather than a flag** (`atomic_inc` in the ISR, `atomic_dec`
in the wait). Overrun the budget and the frame is **not dropped** — it queues,
and the next waits return immediately, replaying back-to-back. So a late frame
never appears as a gap; it appears as a **burst**, which is exactly the shape
that gets attributed to somebody else's producer misbehaving. `backlog` is that
queue. Point it at the Link Audio dropouts before blaming Move's `Link Main`.

**Measured 2026-08-26, and it corrects the budget below.** The ioctl takes
2569µs but only **389µs of that is the transfer** — the other ~2180µs is
`wait_event_interruptible` blocking for the next XMOS IRQ, i.e. frame pacing,
not work and not the wire. With ~140µs of our own compute (`pre` 97 + `post`
43), real slack is **~2370µs, not ~900µs**. See docs/REALTIME_SAFETY.md.

A corollary worth holding: **`total_us` is not a load signal.** Our work
growing shrinks the driver's wait by the same amount, so the loop total sits
near the period whatever we do. The old overrun counter compared it against
2000µs — below the 2902µs period — and so counted *every* frame: 43,986
"overruns" in two minutes on an idle device. Now `OVERRUN_THRESHOLD_US`.

Pure accumulator in `src/host/spi_tally.c` (no I/O, so `spi_tally_record` is
SPI-callback-safe and the whole thing is host-tested by
`tests/host/test_spi_tally.c`); `/proc` read and reporting in `shim_worker.c`.
**Arming gotchas, the measured table, and the two experiments still owed are in
`docs/plans/2026-08-26-spi-tally-followups.md`** — read it before re-measuring;
the tally stays silent for ~20 s after arming, which looks like a broken build.
The IRQ delta is a **32-bit** subtraction on purpose — that counter is printed
from an `int`, goes negative past 2^31 (~72 days) and wraps at 2^32, and only
modular arithmetic survives both. Widening it is the regression the test fails on.

**When the UI feels slow, check the tick rate FIRST.** The shadow UI loop is
paced to an absolute deadline (60 Hz); it previously slept a fixed 16 ms
*after* the work, making the real rate `1/(work + 16ms)` — so every parameter
read added anywhere lowered the frame rate of the whole view, and reducing
work moved the ceiling instead of steadying it. A parameter round-trip is
~2.8 ms (one SPI frame; the shim itself takes ~10 µs) while a whole page
render is 1.68 ms, so **an IPC read costs more than redrawing the entire
screen**. Spend reads like frames.

## Device Constraints

**Never write to `/tmp` on the Move device.** Root FS (`/`) is ~463MB and usually 100% full; `/tmp` lives there. **Always** use `/data/UserData/` (~49GB free) for logs, recordings, temp files, everything. The unified logger already writes to `/data/UserData/schwung/debug.log`.

## Architecture

```
Host (schwung):
  - Owns /dev/ablspi0.0 for hardware I/O
  - Embeds QuickJS for JS execution
  - Manages module discovery and lifecycle
  - Routes MIDI to JS UI and DSP plugin

Modules (src/modules/<id>/):
  module.json       # Required - metadata and capabilities
  ui.js             # JavaScript UI
  ui_chain.js       # Optional Signal Chain UI shim
  dsp.so            # Optional native DSP plugin
```

Key sources: `src/schwung_host.c` (host runtime), `src/schwung_shim.c` (LD_PRELOAD shim), `src/host/module_manager.c`, `src/host/menu_ui.js`, `src/host/plugin_api_v1.h`.

Built-in modules: `chain`, `file-browser`, `song-mode`, `wav-player`.
Source-only (not shipped): `store` (on-device store retired — see Module Install/Update below).
Source-only (not in release tarball): `controller` (superseded by catalog `control`), `tools/{ui,seq,config,splash}-test`, `text-test`.

### JS Module Lifecycle

Every UI module exports four globals:

```javascript
globalThis.init = function() { }                        // Once on load
globalThis.tick = function() { }                        // ~44x/sec (128 frames @ 44.1kHz)
globalThis.onMidiMessageInternal = function(data) { }   // Move hardware MIDI
globalThis.onMidiMessageExternal = function(data) { }   // External USB MIDI (overtake only)
```

`data` is a Uint8Array `[status, cc/note, value]`. Filter noise with `shouldFilterMessage()` from `input_filter.mjs`.

Loading styles:
- `host_load_module(id)` — full load (DSP + UI), auto-calls `init()`
- `host_load_ui_module(path)` — UI-only; caller captures globals and calls `init()` (used by Chain)
- `shadow_load_ui_module(path)` — Overtake modules; deferred `init()` after LED clearing

See `docs/API.md` for full display, LED, MIDI API.

### Module Categorization

`component_type` in module.json determines menu placement: `featured`, `sound_generator`, `audio_fx`, `midi_fx`, `utility`, `overtake`, `tool`, `system`.

### Plugin API

**v2 (recommended, multi-instance, required for Signal Chain)** — see `src/host/plugin_api_v1.h`:

```c
typedef struct plugin_api_v2 {
    uint32_t api_version;              // Must be 2
    void* (*create_instance)(const char *module_dir, const char *json_defaults);
    void (*destroy_instance)(void *instance);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*render_block)(void *instance, int16_t *out_lr, int frames);
} plugin_api_v2_t;
extern "C" plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host);
```

**v1 (deprecated, singleton)** — kept for legacy modules. Audio: 44100 Hz, 128 frames/block, stereo interleaved int16.

### JS Host Functions

Module management: `host_list_modules`, `host_load_module`, `host_load_ui_module`, `host_unload_module`, `host_return_to_menu`, `host_module_set_param/get_param/send_midi`, `host_is_module_loaded`, `host_get_current_module`, `host_rescan_modules`, `host_get_module_metadata(id)`.

Volume / settings: `host_get_volume`, `host_set_volume`, `host_get_setting/set_setting/save_settings/reload_settings` (keys: `velocity_curve`, `aftertouch_enabled`, `aftertouch_deadzone`).

Jack state (for feedback gate): `host_speaker_active()` (true = speakers, false = headphones), `host_line_in_connected()`.

Display: `host_flush_display`, `host_set_refresh_rate(hz)`, `host_get_refresh_rate`.

Filesystem: `host_file_exists`, `host_read_file`, `host_write_file`, `host_http_download`, `host_extract_tar(_strip)`, `host_ensure_dir`, `host_remove_dir`.

Tool lifecycle: `host_exit_module()`. MIDI injection: `move_midi_inject_to_move([type, status, d1, d2])`. Sampler: `host_sampler_start(path)`, `host_sampler_stop()`, `host_sampler_is_recording()`.

CC 79 is the host volume knob by default. Modules can claim it via `capabilities.claims_master_knob: true`.

### Shared JS Utilities (`src/shared/`)

`constants.mjs` (MIDI/LED), `input_filter.mjs` (touch filtering, delta decoding, LED helpers), `menu_layout/render/nav/items/stack.mjs`, `screen_reader.mjs`, `store_utils.mjs`, `filepath_browser.mjs`, `text_entry.mjs`, `sampler_overlay.mjs`, `feedback_gate.mjs`.

## Move Hardware MIDI

Pads notes 68–99. Steps notes 16–31. Tracks CCs 40–43 (**reversed**: CC43=Track1, CC40=Track4). Key CCs: 3 (jog click), 14 (jog turn), 49 (shift), 50 (menu), 51 (back), 71–78 (knobs). Notes 0–9: capacitive knob touch (filter if unused).

## SPI Protocol

`/dev/ablspi0.0`, 768-byte transfers at 20 MHz, mmap'd to 4096.

```
TX (offset 0):                       RX (offset 2048):
  0     MIDI OUT: 20 × 4 bytes        2048  MIDI IN: 31 × 8 bytes
  80    Display status + data         2296  Display status
  256   Audio OUT: 128 stereo int16   2304  Audio IN: 128 stereo int16
```

**Critical:** MIDI_IN events are **8 bytes** (4-byte USB-MIDI + 4-byte timestamp), MIDI_OUT events are 4 bytes. Injecting 4-byte events into MIDI_IN → misalignment → SIGABRT.

Cable numbers: 0 = internal hw, 2 = external USB, 14 = system, 15 = SPI protocol. See `docs/SPI_PROTOCOL.md`.

### A zeroed MIDI_IN slot is a TERMINATOR, not a hole

MIDI_IN is **31 × 8 = 248 bytes** — not `MIDI_BUFFER_SIZE` (256), which runs one
slot into the RX display-status word at +248. Bound every 8-stride walk with
`SHADOW_MIDI_IN_BYTES`.

Move's firmware reads MIDI_IN **until the first empty slot** (cable, CIN and all
three payload bytes zero — `schwung_usb_midi_msg_is_empty`; `schwung_jack_bridge.c`
breaks on it too). So suppressing an event by zeroing its slot in place does not
punch a hole, it plants an end-of-list marker and **everything behind it is
invisible to Move for that frame**.

That is a real stuck-note bug, not a theoretical one. Knob CCs 71-78, knob-touch
notes 0-9, jog, Back and Menu are filtered on every event while the shadow UI is
up; spin a knob with pads held and the pad note-off lands behind a terminator.
Move keeps the pad lit and its instrument sounding — **and so does the slot
synth, because chain slots are fed from Move's MIDI_OUT echo, not from MIDI_IN.**
One drop, two stuck consumers, with the note-off sitting present and correctly
ordered in the raw hardware mailbox the whole time. That last part is what makes
this look like it must be somewhere else; it isn't.

`shadow_midi_in_compact()` (`src/host/shadow_midi_filter.c`) closes the gaps and
runs **last** in `shim_post_transfer` — the blocking sites above it pair `sh[j]`
with `hw[j]` by index, so nothing may move while they run. Zero a slot after it
and the bug is back; `tests/host/test_midi_in_compact_call_site.sh` fails on
exactly that.

Compaction is safe because events already shift between slots across frames —
which is why the dedup rings key on content + timestamp rather than position.
The old "never compact MIDI_IN (SIGABRT)" note came from the RTP-MIDI WIP
(`271bcd0b`), which walked MIDI_IN at a **4-byte stride**, i.e. shifted events by
half an event, and never stabilised for reasons it recorded as unknown.

**Nothing may walk MIDI_IN at a 4-byte stride.** Two places still do
(`shadow_forward_external_cc_to_out` and `shadow_forward_midi`, both in
`shadow_midi.c`); they read timestamps as packets and are unfixed.

## Realtime Safety

SPI callback runs on core 3. Budget ~900µs/frame after the ~2ms transfer.

**Priority: FIFO 70, not 90.** Measured 2026-08-22 with the RT-thread audit —
nothing anywhere in the MoveOriginal process is above 70. This file said 90
here and "the shim runs in MoveOriginal's FIFO 70 threads" two lines down; the
second one is right. What the hardware runs, with no module loaded, is 23
threads of which 11 are realtime: `MoveOriginal` at 10, **`Link Main` at 35**,
three `Audio Worker` at 70, and six threads named `Audio Main/SPI` at 70 (one
at 45). Arm it and look before reasoning about priorities:
`touch /data/UserData/schwung/rt_thread_audit_on`.

**Never in the SPI callback path:** `unified_log()`, `fprintf()`, `fopen()`, any file I/O; allocation; locks held by non-RT threads.

**FIFO inheritance:** Shim runs in MoveOriginal's FIFO 70 threads. Any child process (`shadow_ui`, `host_system_cmd`) must reset to SCHED_OTHER before exec — handled by `shadow_process.c` and `shadow_ui.c`, don't bypass.

**CPU pinning:** Keep core 3 free for SPI. Pin compute-heavy procs (RNBO) to cores 0–2 (`taskset 0x7`). See `docs/REALTIME_SAFETY.md`.

**Module entry points ARE the SPI callback — and the ecosystem does not know
it.** `create_instance`, `destroy_instance`, `set_param`, `get_param`,
`on_midi`/`process_midi` and `render_block`/`tick` all run there. A 2026-08
audit of all 113 catalogued modules found ~150 confirmed violations, several
carrying comments asserting the opposite in so many words ("control-thread
only", "NEVER from process_block — so this malloc is realtime-safe"). Authors
infer a control thread because nothing contradicted them. The contract now
lives at the top of `src/host/plugin_api_v1.h`, in `docs/MODULES.md`, and as
rule 4 of `docs/REALTIME_SAFETY.md` — **keep all three in sync.**

Two consequences worth remembering: `pthread_create` from those entry points
inherits the callback's priority — **FIFO 70** (Move's own `Link Main` is FIFO
35, so it starves). A source audit put this at seven modules; measuring it
found five, and not the same five — see
`docs/plans/2026-08-22-rt-thread-audit-findings.md`. **Existence is not the
harm**: a worker that parks on a condvar starves nobody, so the number that
matters is CPU burned at realtime priority, which is what the audit reports, and a **`get_param` that scans a directory is served
once per repaint**, which makes it worse than the equivalent `set_param`.

## Deployment Layout

```
/data/UserData/schwung/
  schwung                # Host binary
  schwung-shim.so        # Shim (also /usr/lib/)
  host/menu_ui.js
  shared/
  modules/
    chain/                          # Built-in
    sound_generators/<id>/          # External (by component_type)
    audio_fx/<id>/, midi_fx/<id>/, tools/<id>/
```

Device: `ssh ableton@move.local`. Stock firmware preserved at `/opt/move/MoveOriginal`.

## Gain Staging (MFX ME-Only Bus)

Master FX processes only Schwung's internal audio (slot synths, slot FX, overtake DSP) — never Move's. Shim builds:
- `mailbox` (DAC out) = Move audio at `mv` + `me_bus × mv`
- `unity_view` (captures) = Move reconstructed at unity + `me_bus` post-MFX

Skipback, quantized sampler, and the native resample bridge read `unity_view` → captures independent of master volume. Clean-idle leaves Move's mailbox untouched (no round-trip).

Master volume is estimated from Move's on-screen volume bar (`shadow_master_volume` in `schwung_shim.c`). ±2 dB calibration error at extremes; capture degrades below ~15% (amplification clamps at `mv < 0.02`).

Under Link Audio rebuild mode (`rebuild_from_la`), mailbox is composited from per-track routed audio at unity via `shadow_chain_process_fx`, MFX runs on the mailbox, then master volume is applied for DAC out.

## Link Audio

Move's firmware publishes per-track + master audio over Ableton Link Audio (UDP/IPv6, `chnnlsv` framing). Schwung consumes this so shadow FX can process Move's tracks.

```
Move firmware → link-subscriber sidecar (C++, libs/link)
              → SHM /schwung-link-in (5-slot SPSC ring: 1-MIDI..4-MIDI, Main)
              → schwung_shim.c link_audio_read_channel_shm → shadow mixer
```

Sidecar: `src/host/link_subscriber.cpp`. SHM layout: `link_audio_in_shm_t` in `src/host/link_audio.h`. Sidecar also writes shadow-slot output back as `ME-N` Link Audio channels via `/schwung-pub-audio` → `LinkAudioSink`s.

The old `sendto()`-hook reception path was deleted when the public `abl_link` audio API landed (2026-03-30). Sidecar is now the only reception path; `src/host/shadow_link_audio.c` is just the SHM reader + capture buffer. See `docs/plans/2026-04-17-link-audio-official-api-migration.md`.

### Latency Comp

Move's per-track audio round-trips with ~5–14 ms unpredictable drift. Slot synths render locally at ~0 ms → fire ahead of Move audio on the same beat. **Latency Comp** (Global Settings → Audio, default OFF) only runs when `rebuild_from_la = 1`:

1. **Link Audio nudge** (`link_audio_read_channel_shm`): drops/dups one stereo frame every 16 reads outside a ±256-sample dead band around `LATENCY_COMP_TARGET_SAMPLES` = 1400 stereo samples (~15.9 ms). Burst mode (8 frames/period) when error > 512. Effective rate change <0.3%. The target sits at Move's *organic* ring-fill floor under load — an earlier 800-sample (~9 ms) target was below what Move actually delivers, so the nudge drained the ring continuously and underran → dropouts (and the dead band was mistakenly 32, not 256, so it nudged almost every cycle).
2. **Schwung-side delay buffer** (`shadow_latency_delay_apply`): per-slot 2048-sample ring delays `shadow_slot_deferred[s]` by `LATENCY_COMP_TARGET_SAMPLES` (1400) before combining with `move_track`. Bypassed when `rebuild_from_la = 0`. (Target must stay ≤ ring size − one block = 1792.)

Toggle mid-playback → ~16 ms artifact (audio hole on OFF→ON, dup on ON→OFF) as ring resets.

Telemetry: `touch /data/UserData/schwung/link_audio_avail_log_on` for 5 s slot avail logs; `touch /data/UserData/schwung/align_dump_trigger` for ~2.9 s of raw s16le PCM dumps in `/data/UserData/schwung/`.

## Signal Chain Module

```
[Input or MIDI Source] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

Modules declare chainability in module.json: `capabilities.chainable: true` + `component_type: sound_generator|audio_fx|midi_fx`.

### Shadow UI Parameter Hierarchy

Modules expose `ui_hierarchy` (menu structure + knob mappings) via get_param:

```json
{
  "modes": null,
  "levels": {
    "root": {
      "label": "SF2", "list_param": "preset", "count_param": "preset_count", "name_param": "preset_name",
      "knobs": ["octave_transpose", "gain"],
      "params": [
        {"key": "octave_transpose", "label": "Octave"},
        {"key": "gain", "label": "Gain"},
        {"level": "soundfont", "label": "Choose Soundfont"}
      ]
    },
    "soundfont": {"label": "Soundfont", "items_param": "soundfont_list", "select_param": "soundfont_index"}
  }
}
```

- `knobs`: array of param-key **strings** mapped to physical knobs 1–8
- `params` items: string (param key), `{key, label}` (editable), or `{level, label}` (navigation)
- Preset levels use `list_param`/`count_param`/`name_param`; selection levels use `items_param`/`select_param`
- **Use `key`, not `param`**, for editable entries. Metadata comes from `chain_params`.

### Chain Parameters

`chain_params` (get_param JSON array) is **required** for Shadow UI to know step sizes, ranges, enum options:

```c
"[{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
 "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"LP\",\"HP\",\"BP\"]}]"
```

Types: `float` (min/max/step), `int` (min/max), `enum` (options). Optional: `default`, `unit`, `display_format`.

### Chain Architecture

Chain host (`modules/chain/dsp/chain_host.c` — lifecycle/set+get_param/render; helpers split into `chain_{json,params,mod,midi,patch,reorder}.c`, shared decls in `chain_internal.h`) dlopens sub-plugins, forwards MIDI to sound generator, routes audio through FX. Patches in `/data/UserData/schwung/patches/*.json`. Built-in MIDI FX: chord, arp (up/down/up_down/random). Built-in audio FX: freeverb. MIDI sources can provide `ui_chain.js` for fullscreen chain UI.

### Chain shape edits are a PERMUTATION, never a reload

Adding, removing or reordering a position used to be expressed as a run of
`<id>:module` writes, and each of those unloads the position and dlopen()s a
fresh instance — so inserting at the head rebuilt every module behind it and
removing a mid-chain FX rebuilt everything downstream. A running arp lost its
phase; a reverb lost its tail. Three set_param verbs replace that, **1-based to
match the ids**:

```
fx:insert = "1"     midi_fx:insert = "1"    open an empty position, shift the rest along
fx:remove = "3"     midi_fx:remove = "2"    unload that position and close the gap
fx:move   = "1>3"   midi_fx:move   = "3>1"  rotate the span between two positions
```

`chain_reorder.c` shifts every per-position array together (`chain_permute.h`)
and re-aims the three tables that name a position by string — modulation targets,
the two LFOs, the knob mappings. Instances keep running, so **nothing is
carried**: state, modulation base and routing are still the originals.

**Two kinds of per-position array, and the difference is a crash.** A VALUE
array is vacated by zeroing its bytes (`PERM_FIELD`). An OWNED-BUFFER array
holds a pointer to a block allocated once per position by
`chain_alloc_position_storage` and **never null** — `fx_params`,
`midi_fx_params`, and the two `ui_hierarchy` caches (`PERM_OWNED`). Those are
**rotated**: the vacated position gets the buffer displaced off the end of the
shift and its *contents* are cleared. Zeroing the pointer instead left a NULL
that `v2_load_midi_fx_slot` parsed a param table through — SIGSEGV on the SPI
callback, loading a MIDI FX in front of an existing one — and leaked the
allocation the shift overwrote.

`tests/host/test_chain_permute.sh` pins both: a new
`[MAX_AUDIO_FX]`/`[MAX_MIDI_FX]` member not in a collector fails, and the
owned/value split is derived from `chain_alloc_position_storage` rather than
trusted. `tests/host/test_chain_midi_fx_slot.sh` drives the crashing sequence
against a real `chain_instance_t` with the real loader.

Insert only opens the hole — the caller follows with the ordinary
`<id>:module` write. Both chain walks skip a hole per position, so the frame in
between renders correctly.

**Thread safety is free**: parameter requests are serviced from
`shim_pre_transfer` on the SPI audio thread, after `shadow_mix_audio`, and
nothing else touches a chain instance — a permutation cannot interleave with a
render. (That same property is what lets module loading `dlopen()` from this
thread, which *is* a pre-existing realtime violation.)

In the shadow UI, `writeChainShape` emits these verbs. It replaced
`writeChainOrder`, whose state / modulation-base / LFO-remap carries are all
deleted. `clearLfoRoutingForComponent` stays: a picker **swap** genuinely does
destroy and create a module.

The two `+` boxes add **where they are drawn** — the MIDI one at the head of the
chain (index 0), the audio one appended. Backing out of a `+` picker, or picking
`None` in one, writes nothing at all.

### Chain editor knob feedback is a CARD

Touching a knob in the chain editor raises a bordered card
(`src/shared/param_pages/knob_card.mjs`) showing the four cells of that knob's
row, drawn with the knob grid's own widgets via `drawKnobRow` at a 29px cell
instead of the grid's 32. Touch raises it, release drops it; a turn with no
touch raises it too and decays after ~700ms, so a cap sensor that misses cannot
strand the feature. With no component selected the slot's global mappings serve
a name and a value but no type metadata, so that case gets a header-only card.

The card consumes no TURN. A jog-click while it is up is swallowed — it fires
the parameter if that parameter is a trigger, and otherwise does nothing.

It used to be dismiss-and-descend: the click fell through and opened the
focused component. That was deliberate and it was wrong. The card is a panel
over the diagram and the component behind it is only incidentally selected, so
descending acted on something the user could not see — *"when the overlay is
active clicking shouldn't take you into the module, it's a hidden element that
it's still selected"*. Releasing the knob drops the card, so there is already a
way out that does not also do something.

**The 1px black gap between the border and the header band is load-bearing.**
Both are white, so where they touch the border stops existing and the card reads
as a stripe across the diagram. **The divable brackets are load-bearing too** —
nothing dives from the card, so dropping `drawDivableMark` looks like an obvious
simplification, but `drawOpaqueBox` has no frame of its own and the brackets ARE
its frame. Both are asserted on the pixel buffer in
`tests/host/test_knob_card.sh`, with the outermost cell touched, because neither
is visible in code review.

**Every value is read on touch-down, never on the draw path** (`knobCardOpen` in
`shadow_ui.js`) — a read is ~2.8ms against a 1.68ms whole-page render. Two tests
pin it: `test_chain_knob_card_reads.sh` for the renderer, and
`test_chain_edit_read_budget.sh` for `drawChainEdit` itself. The latter LIFTS
`drawChainEdit` with `new Function` and a fixed dependency list, so the card
reaches it through a single `knobCardDrawState()` accessor — nine free
identifiers there is nine chances for a `typeof` guard to make the block
unreachable and leave the budget measured with the card switched off, which is
what happened the first time. Consequence of the read budget: a modulated
NEIGHBOUR does not animate while a knob is held; only the touched knob carries a
modulation mark, because that read is one `showKnobOverlay` already pays for.

`render_page_movy.mjs`'s cell geometry is a parameter (`GRID_GEOM`,
`drawKnobRow`'s optional `geom`), so the card and the grid share one row
renderer. `geom` is **all-or-nothing** — a partial `{cellW}` makes every cell
origin `NaN`, which reaches `line()`'s `for(;;)` and never satisfies its
equality break: a frozen `shadow_ui` tick. The default path is pinned
byte-identical against `tests/fixtures/movy-geom-baseline.txt`
(`UPDATE_GEOM_BASELINE=1` to refresh).

Preview it without deploying: `node tools/param-pages/preview_knob_card.mjs
<module-id> --knob N [--short] [--png DIR --scale 4]`.

### Every enum opens a LIST

Any enum that declares `options` is divable: hold its knob, click, pick from a
scrolling list, Back cancels. The knob still steps it one detent at a time —
the list is the other half, for a Recv Ch with seventeen options or a Braids
model with forty-seven. `VIEWS.ENUM_PICKER`, `drawEnumPicker` in
`src/shadow/shadow_ui.js`, hints `JOG SEL` / `CLK SET` / `BACK EXIT`
(`enumPickerFooterHints` in `shadow_ui_param_pages.mjs` — the hint vocabulary is
a canon, so the wording is built there and not at the draw site).

**`meta.divable` and `meta.divable_mark` are separate on purpose.** The corner
brackets key on `divable_mark`, which stays exactly where it was: the opaque
types. ~135 enums in the fleet against ~5 opaque params — bracket them all and
every cell on every page is marked, which is the same as marking none; and for
an opaque cell the brackets are STRUCTURAL, because `drawOpaqueBox` draws no
frame of its own. Net pixel change on the grid is zero. The affordance for an
enum is the footer, which flips to `CLK OPEN` off `divable` for free. An enum
with no declared options has no list, so it is not a door.

**The picker wears the movy chrome from BOTH entry points** — the knob grid, and
a jog-click on an enum row in the hierarchy list editor — and reuses the one
shared `drawMenuList`. Following the caller's chrome instead would be a
`cameFromGrid` branch inside a shared draw, which is the exact thing
`chain_editor_chrome.mjs` records the module picker doing before ("the module
select here is different than the module select in slots", reported from the
device). Entry-point chrome is that branch coming back.

**The list rect starts at y=9, not `MENU_LIST_Y`.** `MENU_LIST_Y` (10) leaves
44px, which at a 9px line is FOUR options where the old chrome showed FIVE. 9 is
safe only because this header is not inverted — the glyphs stop at row 5, so the
selected row's highlight at row 8 still has air above it. **A menu page cannot
do the same: its bank bar owns row 7.** `tests/host/test_enum_picker_chrome.sh`
pins it as `CAPACITY === OLD_CAPACITY` and `clipped() === 0`, because the device
clips silently and losing the last option to a band drawn over it is a failure
this codebase has already had.

Nothing is written on the way in or while scrolling, so Back is a real cancel
and the draw path costs no IPC. The grid path keeps its controller alive and
commits through `controller.commitEnum` — that is what makes the picker work on
Slot Settings and Master FX Settings, which are synthesised contracts with no
`ui_hierarchy` to enter, and it keeps the slot io's own mappings (Fwd's offset,
MPE's compound write) applied rather than bypassed.

### The knob grid is the DEFAULT param view, and it reflows to stay drawable

`paramViewGlobal` defaults to 1 (the grid). The hierarchy list is still there
under Global Settings → Display → Param View, and it remains the better view for
the 11 modules that publish no `ui_hierarchy` at all — a knob grid over a flat
paginated param list is worse than a list of them.

`param_view.json` is written **only by the toggle**. That is what lets the
default change at all: a device that never touched the setting has no file and
follows the new default, and one where the user explicitly chose List keeps
List. Save it anywhere else — init, a load, an autosave — and every existing
install is pinned to whatever it booted with, forever.
`tests/host/test_param_view_default.sh` asserts the call COUNT, because a
second call site *is* the whole failure.

**A graphic must sit inside ONE ROW.** Row 0's knobs draw at y=10 with their
LABELS at y=25..32 and row 1 starts at y=33, so a shape spanning both would
draw straight through the label band. That is geometry, not a tunable.

The consequence was not acceptable: 26 fleet groups were rejected for LAYOUT
alone — the ADSR on the Main page of obxd, hush1, minijv, moog, surge, rex and
osirus, plus twelve surge LFO pages. An author writing attack/decay/sustain/
release in the obvious order lands on slots 3..6 and gets four separate dials.
`planPages` now moves such a block into a row (`alignGroupsToRows`), 24 pages
across the fleet.

Three rules keep that from being vandalism:

- **it is a permutation WITHIN a page.** No knob is pushed to another page and
  no orphan page holding one control is created. Max group span is 4 and a row
  is 4 wide, so a group always fits.
- **row two is preferred, but only for a block that must move.** "Always put
  the envelope on row two" is wrong: 29 envelopes already sit inside row one
  and draw correctly, many on pages that exist FOR that envelope
  (obxd/Filter Env, hera/Envelope, tablor/Env) where row two would leave the
  top half empty. An always-rule makes 29 pages worse to fix 24. For a block
  that IS straddling, moving it DOWN leaves the head of the page alone —
  minijv keeps `macro_cutoff` on knob 1, where a nearest-fit rule pushed it
  to knob 5.
- **the real detector confirms the result**, and a move that loses a group
  that already drew is rejected.

An earlier version scored by keys covered with no cost bound and did what that
invites: schwung-filter moved cutoff from knob 1 to knob 6 — five knobs
displaced on a FILTER module — to pull one `mode` key into a group that already
drew. It was also 37ms on minijv, twelve times the rest of the plan. Driving
the search from the counterfactual "what would group if the row rule were
lifted?" is both correct and 6.5ms.

**A detector role is OPTIONAL or REQUIRED, and the difference is a whole
group.** `detectFilter` built its slot run from cutoff, resonance AND whichever
of mode/slope it found, then required the lot to be contiguous — so a Mode knob
parked at the far end of the page deleted the corroborated pair. Optionals are
now dropped when they do not fit; `detectEnvelope` takes the longest adjacent
RUN rather than demanding every role found be adjacent.

**`present` is filtered by ROLE and must never be assumed to contain any
particular one.** `drawPartialEnv` computed its attack rise unconditionally, so
surge's twelve hold/sustain/release LFO pages — no attack at all — produced NaN
coordinates, and NaN reaches `line()`'s `for(;;)` whose equality break is never
satisfied. A HANG, not a wrong picture, and unreachable until alignment made
those pages drawable.

### A turn PEEKS the list; a cell that is already big does not

Turning a divable enum raises its option list over the grid for ~700ms
(`ENUM_PEEK_MS`), header `TURNING`, footer `TURN SET`. It is the same screen
the picker draws (`enum_list.mjs`) with the opposite commit semantics: the
detent has ALREADY written, so there is nothing to confirm and nothing to
cancel. It never calls `setView` — a Back that "cancelled" it would be a lie.

Three things take it down: the timeout, turning a NEIGHBOUR (left up it would
describe a knob your hand has left), and Back. **Back closes the peek and stops
there** — it used to fall through to the view exit and throw you out of the
module, which is a wildly disproportionate answer to a panel about to vanish on
its own. It is a layer like the picker and the entered menu, and Back takes one
at a time. `dismissPeek` goes through `enumPeek()` so an EXPIRED peek is not a
layer: swallowing one press is a layer, swallowing two is a trap, and this
screen has no other way out.

**A parameter drawn across MORE THAN ONE CELL does not peek** (`drawnWide`).
The peek exists because a 30px cell cannot show a list; once the picture has
the room, a panel over the top hides the rest of the row to show nothing new.
Not hypothetical — 12 enum cells in the fleet sit inside a wide graphic, every
one a filter type or an LFO shape, where turning the knob already redraws the
curve better than a list of words can.

### The sample cell draws the file it HAS, or nothing

The envelope is the file's real peaks (`wav_peaks.mjs`, streamed and bounded,
advanced from the tick — never from the draw path). When there are none there
is no envelope, just the baseline, the cursor and the brackets.

There used to be a fallback shape, `sin(t*PI)*(0.55+0.35*sin(t*23))`, drawn
whenever the peaks were missing. It is the tri-state read rule in a different
costume — **a read that did not answer must never become a picture** — and it
cost the flagship granular module a waveform for a sample that was never
loaded. granny declares `sample_path` in its hierarchy and on NO knobs list, so
every page carrying `position` searched the page, found no file and drew the
synthetic one.

So `detectSample` resolves the file from the whole contract, not from the page,
and returns it as `extraKeys`. Those are **not** `keys`: keys claim cells, and
an off-page key has no cell to claim. The controller reads them as one extra
stop in the value rotation, the same bargain the preset-name read takes.

**`gatherGroupMembers` seats scattered members together** so the picture gets
the width its controls warrant. `alignGroupsToRows` rescues a group that is
already contiguous but straddles the row break; this is the other half, for
members that are simply not next to each other. It carries the same guarantees,
because it is the same kind of reorder behind an author's back: WHICH keys are
on the page never changes, the result stays inside ONE ROW, and the real
detector verifies the outcome. Measured over the fleet fixture **3 of 489 pages
move** — granny/root 1→2, granny/main 1→2, mrsample/sample 1→3 — and that
narrowness is the feature. A pass that re-seated every page would be a layout
engine, which is a much larger decision. `tests/host/test_viz_gather.sh` pins
the count.

Spray is claimable for that reason. The old rule — it modifies the cursor
rather than being a position, so it never takes a cell — described the
parameter correctly and the layout wrongly: the fences drew on `position`'s
cell while spray sat elsewhere with an arc that looked unrelated. Adjacency
keeps it safe; where the two are apart the run rule still gives span 1.

(A module may declare the same marker on two levels — granny declares
`position` on both `root` and `main` — and the graphic then appears on both.
That is the contract, not the detector.)

### Small ints are BIG NUMBERS, not framed ones

`shouldDrawBigNumber` / `bigNumberText` / `drawBigNumber` in
`render_page_movy.mjs`: an int with a declared range spanning ≤24 (≤48 if
bipolar) draws its value in the device 6x7 font instead of an arc, with a sign
only where the range has a negative side.

It used to draw inside the enum square's box. **The box is the ENUM
affordance** — every enum declaring options is divable, and the square plus its
corner brackets are what say a list is behind the cell. A small int has no list
and can never have one, so the frame advertised a door that does not open.

The span bound is load-bearing: an earlier version bounded at 128 and drew 1392
params big across 60 modules, including `volume [0..100]` and `tune [0..127]`,
which are sweeps where an arc is the honest picture.

### Knob ring LEDs, and giving them back

`knob_leds.mjs` paints CC 71-78 — knobs 1-4 white, 5-8 amber, brightness
tracking value, colour 0 reserved for "nothing is bound here". CC 71-78 carries
encoder rotation IN and the ring colour OUT; notes 0-7 are touch sensors, input
only.

**A ramp is one hue's `dark` → `dim` → full.** The palette header in
`constants.mjs` gives every hue those variants, and it is the authority —
picking constants by NAME produced `DarkBrown2 → Mustard → Ochre →
BrightOrange`, i.e. `#250E05 → #876700 → #491804 → #C93C00`, whose third step
is DARKER than its second: a sweep went dim, bright, dark, bright.
`tests/host/test_knob_leds.sh` parses the hex out of that header and requires
luminance to rise at every step, which is the assertion that catches it; the
older tests only checked that a sweep walks the ramp in the order it is
WRITTEN, which was true of the broken one too. Step boundaries are derived from
ramp length, never written beside it.

**Leaving the grid RESTORES the rings, it does not turn them off.** Move writes
an LED only when its value changes, so going dark left Move's own rings dark
indefinitely. `shadow_control_t.restore_knob_leds` (a JS-set edge the shim
consumes and clears) arms `led_queue_restore_move_sysex_leds()` — the same call
overtake exit makes.

**The colour is in the SYSEX, not the CC.** `move_cc_led_state[71..78]` looks
like the right cache and is not: Move drives the rings via
`F0 00 21 1D 01 01 3B <subcmd> <idx> <6 rgb bytes> F7`, and the CC packets are
latch triggers. Restoring the CC cache restored a latch or a zero and every
ring came back blank. (That sysex is also the way to drive true per-LED RGB —
brightness as `hue x value` rather than a walk through palette entries — but
the encoder `<idx>` mapping is recorded nowhere in this tree and
`led_queue_set_capture_enabled` has no caller and no dump path, so the restore
replays the whole surface instead.)

`invalidateLedCache()` is called with it: `input_filter`'s cache suppresses a
write matching what it believes the hardware shows, which is only sound while
it is the only writer — and the shim is about to repaint underneath it.
### A door you were SENT to opens; one you PAGED past stays shut

Preset browsers, items lists and menu pages are **doors**: the jog pages until
you click in. That rule is load-bearing — a preset browser auditions live, so
browsing past one must not audition every preset it goes by.

It does not apply to arrivals you asked for. **Choosing** a page enters it:
`navigate_to` after picking from a list, and naming a section in the jog-click
picker. Reported from the device both times — *"factory does dump me to
presets, but shouldn't presets be already active? I have to click into it"*, and
for airwindows, whose entire picker is Presets / Main / Jump to Category, two of
them doors. One deliberate gesture should not need a second to take effect.

The switch is `goToPage(index, { enterIfDoor: true })`, and **it belongs there,
not at the call sites**: with `remember` on, `restoreSection` can land you on a
different page of the section than the index passed in, so only `goToPage` knows
what you actually arrived at. Entering writes nothing — a browser auditions on
*turn* — so this hands over the jog without loading anything. Landing on a knob
grid is unchanged; there is nothing to enter.

`onJog` does not route through `goToPage`, which is what keeps paging inert.
`tests/host/test_param_pages_controller.sh` pins both halves, and mutating
`enterIfDoor` away in either direction fails it: dropping the picker opt-in
breaks the new case, and making *every* `goToPage` enter breaks the existing
"jog pages off an un-entered preset page".

**A `navigate_to` naming a level that plans BOTH pages means the browser.** obxd
is the case: its `banks` level names `root`, and root carries
`list_param`/`count_param` *and* `knobs`. The lookup used to filter to
`PAGE_KNOBS`, so choosing a bank landed on the sliders. Preferring the browser
rather than inventing a `navigate_to: {level, kind}` form is deliberate — only
three modules declare `navigate_to` at all, and new vocabulary repeats the
`options_as_string` lesson: documented for months, set by nobody.

### An editor returns to whoever OPENED it, through EVERY door

Diving into a parameter from the knob grid can land you in three different
places — the filepath browser, the canvas view, or the hierarchy editor with
the row opened (edit mode). Each of those has to hand the screen back to the
grid, and each has more than one way out. Miss one and the user comes back
somewhere they did not ask for, one Back away from where they were.

`closeOwnViewEditorToCaller()` is the single answer: it consults
`paramEditorOpenedFromGrid` and returns true if it handled the return. All the
exits go through it — `closeHierarchyFilepathBrowser`, `closeCanvasPreview`,
and **both** ways out of edit mode.

That last one is the trap. **Edit mode is not a view**, so it has no close
function to fix; it is the hierarchy editor with the row opened, and for a
float carrying a waveform strip that strip IS what a user calls "the wave
editor" (granny's `position`). Back out of it already returned to the grid;
the jog-click TOGGLE in `openHierarchyParamEditor` did not — so the gesture
that OPENS the editor was the one that could not close it back. Fixing the two
real views first changed nothing observable, which read as "not deployed".

`tests/host/test_editor_returns_to_caller.sh` drives all three under both flag
states. For the toggle it deliberately leaves the identifiers past the early
return undeclared, so falling through throws instead of passing quietly.

The LFO/knob-mapping target picker is **not** part of this: it is not opened
through `paramEditorOpenedFromGrid` and has its own `lfoTargetFromGrid` /
`returnToSlotGridFromLfoTarget`. Do not merge the two.

### Recording / capture

Audio capture is shim-side: the Quantized Sampler (Shift+Sample) and Skipback
(Shift+Capture) — see Shadow Mode below. (The old chain-host CC 118 recording
was deleted in the 2026-06 cleanup; it was only reachable through the
unreachable v1 plugin path.)

## Shadow Mode

Shim intercepts hardware I/O to mix shadow audio with Move's output.

### Whatever is drawn LAST must be fed FIRST

`onMidiMessageInternal` (`src/shadow/shadow_ui.js`) is a run of early-outs ahead
of the per-view switch, and the draw path is a switch with the overlays painted
after it. **The two orders are the reverse of each other**, so an overlay added
to the bottom of the draw path has to be added to the TOP of the input path, and
nothing about either site says so.

The knob grid's early-out is the one that bites, because it is first and it
claims the jog. Text entry sits ~100 lines below it. That was safe only while no
keyboard could be raised over `PARAM_PAGES` — and then User Presets became a
trailing page INSIDE the grid, `enterPresetSaveAs` opened the keyboard without
calling `setView` (its sibling `enterPresetDeleteConfirm` does), so `view` stayed
`PARAM_PAGES` and the grid ate the jog while the keyboard was drawn on top of it.

**The symptom pointed at the wrong subsystem.** Pad typing kept working, so it
read as a keyboard bug: `decodeInput` (`shared/param_pages/page_input.mjs`)
returns `null` for notes 68–99, so pads fall through, but it decodes CC 14 as
navigation and consumes it. A half-working overlay is the signature of a
dispatch-order bug, not a broken handler — check what is *upstream* of the
handler before reading the handler.

Guard the grid block (`&& !isTextEntryActive()`) rather than hoisting the
overlay to the top: the feedback-gate and canvas-steal blocks sit between the
two, and the feedback gate is a safety modal that must keep outranking
everything. Precedence among overlays is deliberate, so moving one is a change
in its own right. `tests/host/test_text_entry_outranks_grid.sh` pins the order,
and pins the `decodeInput` jog-vs-pads asymmetry separately so a future change
that starts claiming pad notes fails loudly instead of silently.

### A param read has THREE answers, not two

`shadow_get_param` (`js_shadow_get_param`, `src/shadow/shadow_ui.c`) returns:

```
JSON / text   the component answered
""            the channel served us, the key produced nothing
null          the read did not complete — claim refused, response timed out,
              or the answer belonged to somebody else
```

`null` is **not news about the module.** Collapsing it into `""` cost three
separate bugs in one day: `impressive-chords` reporting a literal `"[]"` for
`chain_params` and being believed; missing metadata making the grid invent a
`float 0..1 step 0.01` knob and write `0.058750` into an enum; and granny's
knobs reordering with `sample_path` on knob 1, because a timed-out
`ui_hierarchy` read was read as "this module has no hierarchy", paginated
`chain_params` instead — and then **latched**, because the invented metadata
looked complete enough to settle.

**Never let a failed read produce a plan, a default, or a cached verdict.**
Branch on the RAW value before parsing: `parse(null)` and `parse("")` both give
`null`, so by the time it is parsed the distinction is gone and only the caller
that saw the wire can report it. In `page_controller.mjs` that is
`contractUnresolved` — plan nothing, keep the previous page set if it is the
same component, retry on `CONTRACT_RETRY_INTERVAL_TICKS` up to
`CONTRACT_RETRY_LIMIT`, and refuse to `metaSettled` while unresolved.
`planPages({ unresolved: true })` returns no pages for any other consumer.

Wrinkle: `tests/fixtures/module-contracts.json` records `ui_hierarchy: null` for
the four modules that genuinely declare none, so at PLANNER level `null` still
means absent. The tri-state exists only where the wire is visible.

(granny's read fails because it loads a WAV synchronously inside `set_param`, on
the SPI thread that serves param requests. That realtime violation lives in its
own repo and is not fixed here.)

### Global Settings is a SYNTHESISED CONTRACT, not a screen

It runs on the same page engine as a module, a slot's settings and Master FX's
settings — one list, one chrome, one set of widgets. The declaration is
`src/shadow/shadow_ui_global_grid.mjs` (pure, no host globals, tested with no
device by `tests/host/test_global_settings_contract.sh`); the concrete backends
and the cache-var writes that cannot leave `shadow_ui.js` are `globalGridIoFor()`
there. Entry is `enterGlobalSettingsGrid()`, modelled on
`enterMasterFxSettingsGrid`.

**Seven sections are seven PAGES**, jogged through on one axis with the section
picker on click — Display, Audio, Screen Reader, Set Pages, Shortcuts, Services,
Updates. Six are knob pages; Updates is a menu page. **One section, one page** is
load-bearing: a ninth param in any section paginates silently and the bank bar
takes over a split nobody chose. Audio sits at exactly eight. The contract test
pins the per-section counts rather than trusting the shapes.

Three consequences worth knowing:

- **`[Help...]` lives on the Updates page**, one row under `[Module Store]`. It
  used to be a peer of the sections; it cannot be a page of its own (that is an
  eighth page, pinned against twice) and a one-entry menu page is the shape
  Master FX already records as a mistake. See `UPDATES_ACTIONS`.
- **`VIEWS.GLOBAL_SETTINGS` is now only the help viewer's host.** The section
  list, the in-section list, the four `globalSettings*` state vars and the three
  switch arms that drove them are gone. `runGlobalActionFromGrid` /
  `maybeReturnToGlobalGrid` are the third instance of the slot / Master FX modal
  hand-off, and reconcile the same way rather than hooking each exit.
- **The screen reader forces the LIST layout** (`paramPagesLayout()`), because
  Global Settings enters the page chrome even with TTS on — it has no hierarchy
  editor behind it, and it is the screen you go to to turn TTS off.
  `paramPagesEnabled()` still refuses the chrome for every *component*; that
  seam is unchanged.

Persistence is **three** things and conflating them loses a write silently: a
shared `saveMasterFxChainConfig()` sink (derived from the routing table, never
hand-listed), a key-specific saver welded to the assignment, or backend-owned.
Stored values are **not** indexes — `resample_bridge` stores 0 and **2**.

### A timed-out read empties NOTHING, and latches nothing

`loadChainConfigFromSlot`'s `readPosition` was `moduleId && moduleId !== ""`,
which puts `null` (the read did not complete) in the same branch as `""` (the
position is empty) — the comment there said so, having considered only the
unserved case. Loading a module blocks the SPI callback (the thread that also
serves param requests) and `applyComponentSelectionConfirmed` re-syncs
**immediately after its fire-and-forget module write**, i.e. inside that
window. So the position read `null`, was recorded as EMPTY, and
`chainConfigFresh[slot] = true` declared it authoritative — *"clean by
definition once it returns"* was true of the call, not of the answer.

An empty box in the diagram is a `+`, so the position the user had just filled
opened the **module picker** instead of the editor.

**It takes a SECOND defect to make that permanent**, and this is the part worth
remembering: the module signature is a separate set of reads taken milliseconds
later, and they straddled the end of the load. The config read stale-empty; the
signature read the real module. `applySlotModuleSignature` reloads the config
only when the signature **changes** — so the *correct* read is what did the
damage, by matching, and a correct signature never changes again. Osirus logged
a clean 124 ms load at 13:48:53.700–.824 and the editor still drew the position
empty fifteen seconds later, while slot settings — same key, different path —
said "Synth Virus".

Now: a failed read keeps the position it had, leaves the slot **un-fresh** so
the next frame re-reads, and `getSlotModuleSignature` answers **null** rather
than inventing an empty chain (`applySlotModuleSignature` refuses null). A
failed `*_count` keeps the section length — 0 from a timeout truncates the whole
section, not one position.

Falling out of it for free: the picker writes the chosen module into
`chainConfigs` **before** the DSP write, so "what we already had" during the
load window *is* the module just picked — the box shows it throughout, and
there is no loading state to maintain. `tests/host/test_chain_config_read_failure.sh`
lifts the real functions and drives that sequence, reads failing on frame 1 and
landing on frame 2.

### A component editor WAITS; it does not decide from one read

Opening a component's editor used to be one read of `<prefix>:ui_hierarchy` and
`if (!hierarchy) enterComponentEditFallback(...)` — which is the three-answers
defect one layer above the controller that solves it, and the fallback is
irreversible. For MiniJV and Osirus, the two slowest things in the fleet to
come up, that drew an editor with **nothing in it**: neither ships a
`ui_chain.js`, so the fallback lands on the bare preset browser, and the preset
reads it makes there fail for the same reason the hierarchy read did.

What made the entry the wrong place to give up is that **everything which knows
how to wait is behind it** — the grid's `Loading...` hold, its bounded contract
retry, its ten-second recovery probe, the list editor's `is_loading` re-fetch.

`src/shared/component_load_gate.mjs` answers **ENTER / HOLD / FALLBACK**, and
`openComponentEditor` (`shadow_ui.js`) is the one gate both editors — slot
chain and Master FX — enter through. HOLD raises `VIEWS.COMPONENT_LOADING`
("Loading...", `Back: exit`) and asks again: ~0.5 s apart for ~20 s, then every
ten seconds for as long as the screen is up. **There is no give-up-and-show-the
-fallback ending**, on purpose — a blank editor is the failure being fixed.

**The empty answer needs a second question.** A module that declares no
hierarchy and a position whose module has not finished arriving BOTH answer
`""`. `<prefix>_module` separates them: the chain host publishes the name only
after `create_instance` returns (`chain_host.c:504`). Named + no hierarchy
falls back **immediately**, so the well-behaved fleet never sees the hold, and
entering still costs the one read it always did (`module` and `is_loading` are
read lazily, on the ambiguous branch only).

The wait is view-agnostic — it sits in front of the destination choice, so it
works with Param View on Knobs or List and with the screen reader on — and it
is drawn and serviced on **both** draw paths, main and co-run. The probe runs
*before* the dispatch, so a probe that lands opens the editor on that frame.
`tests/host/test_component_load_hold_wiring.sh` pins all of that from source;
`test_component_load_gate.sh` unit-tests the decision, including that a named
module with no hierarchy is **not** held.

Not a regression: the old gate is byte-identical at `v0.11.6`. What changed is
how long these two modules take to answer.

### Shortcuts

Shadow UI access gated by **Global Settings → Shortcuts → Shadow UI Trigger** (`shadow_ui_trigger` in `features.json`): `Both` (default) / `Long Press` / `Shift+Vol`.

**Shift+Vol combos** (modes Both / Shift+Vol):
- **Shift+Vol+Track 1–4** — open shadow / jump to slot settings
- **Shift+Vol+Menu** — Master FX
- **Shift+Vol+Step2** — Global Settings
- **Shift+Vol+Step13** / **Shift+Vol+Jog Click** — Tools menu (overtake modules below the divider). Jog-click also exits an active overtake module.
- **Shift+Sample** — Quantized Sampler
- **Shift+Capture** — Skipback (last 30 s)

**Long-press** (modes Both / Long Press):
- Hold Track 1–4 (500ms) → slot editor
- Hold Menu (500ms) → Master FX
- Shift + hold Step 2 (500ms) → Global Settings
- Shift + Step 13 (immediate) → Tools menu
- Tap Track / Menu while shadow UI shown → dismiss

Long-press is suppressed once the volume knob is touched during a track press (so Track-hold + knob adjusts track volume without opening shadow UI). See `track_vol_touched_during_press[]` in `schwung_shim.c`.

**While shadow UI shown** (any mode):
- **Mute + Jog Click** on focused chain/MFX module — toggle bypass. Audio passes through; MIDI FX become passthrough; synth render silenced while MIDI flows (state advances, tails ring out, clean unbypass). 4-row 'B' glyph above the module box.
- **Mute + Track 1–4** — slot mute. **Shift + Mute + Track 1–4** — slot solo.

Mute (CC 88) is passed through to Move firmware (even while shadow UI is shown) so Move-native **Mute + Pad** (per-drum mute) works. `shadow_mute_held` is tracked from the hardware buffer independently, so the shadow combos above still work. Consequences: a plain Mute tap also toggles Move's selected-track mute, and Mute + Track double-mutes (shadow slot + Move track) — these stay in sync, which is intended. Shadow slot mute/solo is set **only** by these combos — there is no D-Bus screen-reader text sync. (A former `shadow_dbus.c` auto-correct matched any announcement ending in " muted"/" soloed" and applied it to the selected slot; Move utters drum kit/pad names with those suffixes — e.g. "Lay Down Kit muted" — and Schwung's own TTS loops back through the same handler, so it spuriously muted slots and persisted the state, silencing audio across all projects. Removed; a version-stamped one-time heal in `shadow_state.c` clears any already-stuck persisted mute/solo on upgrade.) Bypass persists via per-slot autosave (`slot_N.json`, `master_fx_N.json`); patch-library reloads start with bypass=0.

### Quantized Sampler

Shift+Sample. Source: resample (incl. Schwung synths) or Move Input. Duration in bars (or until stopped); uses MIDI clock, falls back to project tempo. Starts on note event or play. Saved to `Samples/Schwung/Resampler/YYYY-MM-DD/`.

### Feedback Protection

Loading a chain module / tool that consumes line-in shows "Speaker Feedback Risk" warning if speakers active AND no line-in cable. Jog-click proceeds, Back aborts.

Gate fires when: module's `capabilities.audio_in == true` AND `component_type` is NOT `audio_fx`/`midi_fx` AND `shadow_speaker_active` AND NOT `shadow_line_in_connected`.

**Continuous feedback guard (bypass-on-risk, auto-clear when safe).** The interactive gate only runs on *user* selection, so a restored line-input slot would activate hot at cold boot (jack state is unknown to the shim there) and feed back. Now there's a continuous guard:
- **Shim, at boot:** any restored line-input slot is brought up **BYPASSED** (real `synth:bypassed=1` — the visible "B" glyph) with `feedback_hold = 1` as the guard's marker (`shadow_slot_apply_boot_feedback_hold` in `shadow_chain_mgmt.c`, both boot-restore paths). Bypass is set *after* `load_file`, so it survives the per-set mute-state load (which only touches `muted`). Chain host exposes `synth:consumes_line_input` (parsed from `capabilities.audio_in` via the new `json_get_bool_in_section`, since `audio_in` is a JSON boolean) and `synth:bypassed` get/set.
- **JS, continuously** (`reconcileFeedbackHolds` in `shadow_ui.js`, throttled ~4×/sec): for each Line In slot — **risk present** (`host_speaker_active() && !host_line_in_connected()`) → bypass + announce + raise the **"Feedback Risk"** modal when the shadow UI is on screen (gated on `shadow_get_display_mode() === 1` so it can't hijack Move's jog/back while hidden); **safe** → un-bypass + auto-dismiss the modal; **manual un-bypass (Mute+JogClick) or modal "Override"** → treated as a session override (no re-bypass until safe again or reboot). This re-bypasses on mid-session headphone *unplug*, not just at boot. Param plumbing: `slot:feedback_hold` get/set on the slot struct's new `feedback_hold` field. The modal (title/lines/footer/announce) is customizable via `confirmLineInput(label, cb, opts)` in `feedback_gate.mjs` + `drawConfirmOverlay(title, lines, footer)`; `feedbackGateCancel()` dismisses it programmatically when risk clears.

**Global default always empty.** The one-time per-set migration (`shadow_batch_migrate_sets`, `shadow_set_pages.c`) no longer copies the global `slot_state` into each set — it seeds **empty** `{}` slot/master_fx files + a default chain config (`seed_empty_set_state`, mirroring the JS new-set path). A stale global slot (e.g. an old line-in + reverb autosave) can no longer propagate into every set and reload on boot. (This is migration-only and gated by `set_state/.migrated`; it does **not** scrub sets already migrated — those rely on the boot bypass or manual cleanup.)

Impl: `src/shared/feedback_gate.mjs` (predicate + modal), `src/shadow/shadow_ui.js` (call sites + continuous guard), `src/schwung_shim.c` (XMOS CC 114 line-in, CC 115 line-out). Out of scope: Move firmware's autosample / line-in monitoring; Quantized Sampler "Move Input" toggle (fullscreen menu makes JS modal inert). See `docs/plans/2026-04-30-feedback-protection-design.md` and `docs/plans/2026-06-25-boot-feedback-fix.md`.

### Skipback

Shift+Capture saves last 30 s. Same source as sampler. Output: `Samples/Schwung/Skipback/YYYY-MM-DD/`.

### USB-C Audio-Out Source

Move's Settings menu picks what a connected computer receives over USB-C (Mic or
Main Out). Move's firmware **never persists it** — there is no key in
`/data/UserData/settings/Settings.json`, and the dialog is built as
`ListViewDelegate<UsbAudioOutputSourceDelegate, NullTransactionPolicy>` — so it
reverts to Mic on every boot. Schwung remembers it instead.

Selecting a value makes Move emit a **pair** of XMOS audio-IO SysEx messages in
one SPI frame (captured on hardware 2026-08-18):

```
Main Out:  F0 00 21 1D 01 01 37 12 02 00×12 F7   +   F0 00 21 1D 01 01 37 14 01 00×12 F7
Mic:       ...37 12 00...                        +   ...37 14 00...
```

`37 12` is the shared routing/monitoring TLV — **bit0 is the USB-C *input*
select, owned by Move's sampling page**; bit1 is monitoring, which is *how* Main
Out reaches USB-C (the XMOS mutes the speakers while it's set, to prevent
feedback). `37 14` is the dedicated out-source bit. This resolves open question
Q2 in the movesniff findings doc, which listed `0x14` as unreversed.

Flow: the SPI pre-transfer callback scans MIDI_OUT via `xmos_audio_scan`; the
worker persists the value to `/data/UserData/schwung/usbc_out_state`; ~5 s after
boot the worker arms a replay, which the SPI callback emits one message per
frame. Only Main Out is replayed — Mic is Move's own boot default, so there is
nothing to correct and nothing goes on the wire.

Two behaviours worth knowing:

- **Persistence is gated for ~7 s after boot.** Move asserts its Mic default at
  ~0.6 s, and the shim observes its *own* replay too (emit runs earlier in the
  same `pre_transfer` than scan). Persisting either would clobber the stored
  preference on every reboot. Trade-off: a change made in the first ~7 s of boot
  is not persisted.
- **Move's own Settings screen keeps reading "Mic"** even when the hardware is on
  Main Out — Move doesn't adopt the replayed value into its UI state. The audio
  is correct; the screen is not. Selecting "Main Out" there is harmless;
  selecting "Mic" (believing it a no-op) actually switches it off.

**Global Settings → Audio → USB-C Persist** (`usbc_out_persist`, default On)
governs *whether Schwung restores* the value — deliberately **not** a second
Mic/Main Out picker, which could disagree with Move's. Its value column
annotates the source last seen on the wire (`On (Main Out)`), which is the only
honest read given Move's screen goes stale. Params: `master_fx:usbc_out_persist`
(get/set) and `master_fx:usbc_out_source` (get only; -1 unknown, 0 Mic, 1 Main
Out). Persisted to `shadow_config.json`, which the **shim parses at init**
(`native_resample_bridge_load_mode_from_shadow_config`) — so the flag is known
before the ~5 s replay and the restore needs no runtime propagation.

Impl: `src/host/shadow_xmos_audio.c` (pure codec — no I/O, allocation or locks,
so it is both SPI-callback-safe and host-testable; unit tests in
`tests/host/test_xmos_audio.sh`), observed and emitted in `schwung_shim.c`'s
pre-transfer callback, persisted and armed in `src/host/shim_worker.c`.

`xmos_audio_emit` is also the only sanctioned way to put SysEx into MIDI_OUT: it
requires a **contiguous** run of free slots, refuses while any cable-0 SysEx is
mid-flight, and never partial-writes. The `spi_sysex_inject` debug trigger was
rerouted through it — the old path blind-wrote `out[0..31]` regardless of what
Move had queued, and a stuck injection like that hard-powered-off the device
twice.

### Shadow Architecture

`src/schwung_shim.c` (LD_PRELOAD, intercepts ioctl, mixes audio), `src/shadow/shadow_ui.js` (slot/patch UI), `src/host/shadow_constants.h` (SHM structs).

SHM segments: `/schwung-audio` (mixed shadow output), `/schwung-control` (`shadow_control_t`), `/schwung-param` (param requests, `shadow_param_t`), `/schwung-ui` (`shadow_ui_state_t`).

`shadow_control_t.ui_flags`: `JUMP_TO_SLOT (0x01)`, `JUMP_TO_MASTER_FX (0x02)`, `JUMP_TO_OVERTAKE (0x04)`.

### Shadow Slot Features

Each of the 4 slots has:
- **Receive channel**: 1–4 (default) or All (−1)
- **Forward channel**: 1–16 or −1 (auto: remap to receive ch, or passthrough if receive=All) or −2 (THRU: preserve original ch). Modules can declare `default_forward_channel` in capabilities.
- **Volume**, **state persistence** (synth + FX + MIDI FX).

**MPE controllers** (LinnStrument, Roli, Sensel): set Receive=All, Forward=THRU, enable MPE in the synth. Otherwise channel remap destroys per-note bend/pressure/slide.

### User Presets

Per-component preset snapshots for any chain module (synth, audio FX, or MIDI FX). Reached from a component's module-swap list in the shadow UI — an indented `[User Presets]` row tucked under the loaded module, or the component's own knob-grid "My Presets" page's `Load…` action. A preset captures that component's opaque `<prefix>:state` blob (`synth` / `fx1`..`fx4` / `midi_fx1`) — the same string slot autosave and chain patches use — saved to `/data/UserData/schwung/presets/<module-id>/<name>.json`. Keyed by **module id**, so a preset saved on a module in one slot is offered wherever that module is loaded (cross-slot reuse).

**The browser is exactly ONE thing: choose a preset.** Picking a row LOADS it
immediately and commits — there is no per-preset Load/Delete detail screen.
Save, Save As and Delete are not offered here at all; they live on the
component's own "My Presets" grid page (see below). This was three separate
hardware reports, one cause: the verbs had moved to the grid page but the
browser still offered its own copies — *"loading a preset shouldnt show
load/delete, it should just load it (delete is on the main menu)"*, *"after
deleting i get to a menu of [save current] not the preset (none) page"*,
*"i also see [save current] if i load without saving"*. Scrolling the list
**auditions live** (debounced) **when Global Settings → Audition is on**;
Back reverts to the slot's original state. That gate (`browser_preview`,
shared with the file browser's WAV preview) **defaults to OFF**: auditioning
applies state to the live slot, and this list stopped being hard to reach the
moment it became reachable from a page at the end of every component. Off
disables the audition, not the list — a pick still loads, and with it off the
browser pays no `:state` read on entry. Autosave is suppressed while
auditioning (`isPresetPreviewActive()`) so an uncommitted preview is never
persisted into `slot_N.json`. Impl: `src/shadow/shadow_ui_presets.mjs` (view
module). Developer state-contract notes in `docs/MODULES.md`.

A committed Load, or a completed Delete (still reached exclusively from the
grid's My Presets page, via `enterPresetDeleteConfirm` — the SAME
confirm-delete screen as before, just with no detail screen left in front of
it), both exit through `VIEWS.CHAIN_EDIT`. `maybeReturnToComponentGrid` (see
below) is what routes a grid-driven arrival back onto the My Presets page
specifically, by NAME; a `[User Presets]`-row arrival (no grid open) lands
plainly on the chain editor, as it always did.

### Every component's knob grid ends with two pages it never declared

Load a synth, audio FX or MIDI FX in one of the 4 slots and its knob-grid jog
sequence ends with two pages neither the module nor its author put there:
**My Presets** (row 1 a readout — `Preset` / `(none)` or `Name` / `* Name` —
then `Load…`, `Save` and `Delete` only with a preset loaded, `Save As`
always) and **Module** (`Swap Module`, `Remove Module`). Both are doors: a
`PAGE_MENU` must be entered before an entry fires, so jogging past the end
cannot fire Remove Module by accident.

**Named "My Presets", not "User Presets"** — the header's right side is a
MEASURED share against a `HEADER_MIN_LEFT` floor (70px), and "USER PRESETS"
(56px) is past it and truncates to "USER PRESE". "My Presets" (46px) fits.
"Presets" alone would be worse: 27 modules in the fleet already plan a page
called that (obxd, sfz, hush1, minijv, sf2, hera, tablor, noisemaker, …), so
`claimName` would dedupe this one to "Presets - 2". Reported from hardware —
rendered PNGs, not text art, are what actually showed the truncation.

**The `*` follows a knob write within one settle, not just a page
re-entry.** Turning a knob on any OTHER page of the same component changes
the live `<prefix>:state` blob the mark compares against, and nothing used to
notice until the page was re-entered — *"changed a knob and * didnt appear
until i exited and re entered the module"*. Fixed without adding a
draw-path read: `componentParamPagesIo`'s `setParam` marks the write pending
(`markComponentParamWrite`); `tickUserPresetStale`, driven from the main
tick (never a draw function) alongside `tickParamPages`, waits out
`CONTRACT_SETTLE_MS` and then asks ONCE — via `paramPagesRefreshTrailing()`,
the same call Save/Load/Delete already use — and only when the grid is still
open on the exact `(slot, component)` that wrote. One read per settle, never
per detent, none once the user has moved on.

**The header shows the loaded USER preset, with the same mark, on every
page of the component** — `S1 > tst` clean, `S1 > * tst` dirty — falling
back to the module's own patch name and then its abbreviation exactly as
before when no user preset is loaded. Asked for on hardware and shipped:
*"should we change the preset in the header from the system preset to the
user preset? (Init -> tst) and then show the * there too?"*. Reads a CACHE
(`userPresetLiveBlobCache`, keyed per slot+prefix), never the DSP —
`userPresetHeaderMark` in `shadow_ui.js`, wired through `ctx` to
`headerTitle()` in `shadow_ui_param_pages.mjs` — so this costs nothing beyond
the read the My Presets page already pays for, and it answers `null`
harmlessly for a synthesised contract (Slot/Master FX/Global Settings) or a
Master FX component, none of which populate a record for their key.

**They are appended by the PLANNER, after the whole walk — not injected into
a level's hierarchy — because injection cannot work for this fleet.** A
level's own `menu:` field (the same PAGE_MENU kind) lands right after that
level's OWN grids, not last: Slot Settings dodges that by giving its menu a
level of its own, which only works because it synthesises its whole
hierarchy end to end. We do not own a module's. And three fleet shapes rule
out injection outright: 11 of the 95 modules in
`tests/fixtures/module-contracts.json` publish no `levels` object at all
(chain_params pagination fallback), minijv has `levels` but no `root`, and
with `modes` present the walk root is whichever mode is active. There is no
level guaranteed to exist that "append to the end" could target.
`planPages({ trailingMenus })` in `src/shared/param_pages/page_plan.mjs`
appends after the walk instead — see `buildTrailingPages`/`appendTrailing`
there and `io.trailingMenus()`/`refreshTrailing()` in
`src/shared/param_pages/page_controller.mjs`.

**A failed contract read cannot manufacture them.** `planPages` returns no
pages at all when `unresolved`, so the append only ever lands on a resolved
plan — the same rule as "a plan is a statement about what a module declares"
above.

**Scope is exactly the 4 chain slots' real components.** Master FX chain
components are excluded — `__user_presets__` is injected in
`enterComponentSelect` only, so Master FX has no user presets today and this
inherits that gap rather than widening it. Slot Settings and Master FX
Settings are excluded because they are settings, not modules: no module id to
key a preset folder on, nothing to swap. The exclusion lives in ONE helper,
`componentParamPagesIo` in `src/shadow/shadow_ui.js`, called from every
component `enterParamPages` site, so a new call site cannot silently opt
Master FX in. Grid view only — the list view (`param_view = 0`) is a separate
code path with no pages to jog through and keeps its existing Shift+Click
route.

**The `*` leads the name**, e.g. `* Fat Brass` not `Fat Brass *`, because the
list renderer truncates the TAIL: rendered on obxd, `"Fat Brass *"` drew as
`"Fat Br…"` and the one character carrying the information was the first
thing lost. See `presetRowValue` in `src/shared/param_pages/current_preset.mjs`.
It costs no draw-path read — it compares the live `<prefix>:state` blob
against a stored hash at PLAN time and on explicit refresh, never per frame
(`trailingMenus()` has exactly 4 call sites, none inside `render()`).

**Save overwrites; Save As does not.** `overwriteUserPreset` refuses when the
`:state` read returns `null` — a FAILED read, not empty state — because
writing it would replace a good preset with nothing. **Remove Module IS the
picker's `None`**: it goes through `applyChainComponentPick`, the same
function the picker uses, because removal is not one write — it closes the
gap and renumbers everything downstream via a `remove` verb that permutes the
DSP arrays rather than reloading modules (see "Chain shape edits are a
PERMUTATION" above).

### MIDI Cable Filtering

MIDI_IN (offset 2048): cable 0 = Move hw controls, cable 2 = external USB MIDI.
MIDI_OUT (offset 0): cable 0 = Move internal out, cable 2 = external USB out.

Normal shadow: only cable 0 processed. Overtake: all cables forwarded; cable 2 → `onMidiMessageExternal`. If Move tracks listen+output on same channel as external device, MIDI echoes back — use different channels.

### Cable-2 Channel Remap (Overtake)

Overtake modules can rewrite cable-2 MIDI_IN channel before Move firmware sees it (solves live-external ↔ Move-native routing without the JS-reinject cascade in `docs/MIDI_INJECTION.md`).

JS API: `host_ext_midi_remap_set(in_ch, out_ch)` (0–15; out_ch >= 16 or < 0 = passthrough), `host_ext_midi_remap_clear()`, `host_ext_midi_remap_enable(on)`.

Shim reads the table every SPI frame (post-transfer), rewrites channel byte in-place in both hw mailbox and shadow buffer. System messages (`0xF*`) skipped. **Bypassed globally** if any chain slot is forward=THRU (MPE preservation). **Force-reset** to all-passthrough + disabled on overtake exit. Gated by `ext_midi_remap_enabled` in `features.json` (default `true`).

SHM: `/schwung-ext-midi-remap`, 64 bytes, `schwung_ext_midi_remap_t` in `src/host/shadow_constants.h`. v1.

### Co-run Input Ownership

Co-run lets an **overtake tool share Move's control surface with a second UI** for one user-driven session — e.g. a sequencer keeps the pads/steps/transport while the Schwung chain editor (`CORUN_TARGET_CHAIN_EDIT`) takes the OLED + jog, or Move's native preset/synth editor (`CORUN_TARGET_MOVE_NATIVE`) takes its knobs. **Cede-by-default**: the tool keeps the whole surface and lists only the groups it *cedes* to the peer. Cedeable surface: pads, encoders, transport (Play/Rec/Sample/Loop), edit (Copy/Delete/Undo/Capture), and the 4 nav arrows, with an optional separate LED-keep mask and a canvas sub-view. JS API: `shadow_corun_begin_cede(target, id, cede_mask, flags)`, `shadow_corun_set_cede_mask`, `shadow_corun_set_led_cede_mask`, `shadow_corun_event_owner`; masks use `CORUN_GRP_*` bits (see `src/host/shadow_constants.h`). Full ownership model, group bits, and the cede↔keep complement in `docs/CORUN.md`.

### Master FX Chain

8-slot Master FX processes mixed shadow output. Access: Shift+Vol+Menu.

The cap lives in **two** places that must move together — `MASTER_FX_SLOTS` in
`src/host/shadow_chain_mgmt.h` and in `src/shadow/shadow_ui.js` —, and
`tests/host/test_master_fx_slots_js.sh` fails on drift between them. Key routing
is cap-derived through `src/host/master_fx_key.h`
(`master_fx_route_param_key` / `master_fx_route_target`), pinned by
`test_master_fx_slot_routing`, which widens with the cap rather than quietly
covering half the range.

**The diagram is `chain_diagram.mjs`, the same one the slot editor uses.** It
replaced a fixed `TOTAL_W = 5 * BOX_W + 4 * GAP` row that filled 118 of 128
pixels: nine boxes would have been 214px, drawn off-screen with no clipping and
no error, taking the bypass `B` and the LFO `~` marks with them. A fixed row
cannot report that it overflowed, which is why
`tests/host/test_master_fx_diagram_fit.sh` asserts `clipped() === 0` at the cap
and one past it. Per-box reads are bounded by the ~5 boxes DRAWN, not by the
cap, so raising 4 → 8 cost one read per frame rather than four.

Master FX still has **no insert, remove or move** — removal is picking `None`,
which unloads in place and leaves a hole. Adding those (and the permutation
that must come with them) is residual 2.2 Step 4, and it is a new feature, not
a port of `chain_reorder.c`.

### A STEP button is not a note, and audio FX were told it was

Audio FX are fed from **three** places, and the only guard any of them had was
`d1 >= 10` — which exists solely to drop the capacitive knob-touch notes 0–9,
and was never a claim about what counts as musical input:

```
src/schwung_shim.c   MIDI_IN cable 0 (Move's own surface)   notes, d1 >= 10
src/host/shadow_midi.c   shadow_chain_dispatch_midi_to_slots    ALL voice msgs, no guard
src/host/shadow_midi.c   shadow_dispatch_direct_external_midi   cable-2 THRU, d1 >= 10
```

So on Move's own surface the **step buttons (16–31) and track buttons (40–43)
reached every loaded audio FX as played notes.** Found with an FX whose note
handler fires a one-shot action (capicola's forced re-slice): in Master FX it
fired on essentially any button press. The ducker had the identical exposure
and merely read as "sensitive". Both `shadow_master_fx_forward_midi` and the
slot `FX_BROADCAST` were affected — the asymmetry is **not** master-vs-slot, it
is broadcast-vs-dispatch: `chain_midi.c:720` handles `FX_BROADCAST` by
forwarding to every audio FX and returning *before* any channel logic, so only
the non-broadcast dispatch was ever channel-matched.

Two guards fix it, in `src/host/fx_midi_filter.h`, and **the split is the
point**:

- `move_surface_note_is_pad(d1)` — cable-0 sites ONLY, where a note number is a
  physical control identity. Replaces `d1 >= 10` at both shim broadcasts.
- `fx_midi_channel_accepts(ch, status)` — applied **inside**
  `shadow_master_fx_forward_midi`, not at its callers, so all three feeds are
  gated by construction and a fourth cannot be added ungated.

Never apply the note-range guard to the external sites: there a note number is
a **pitch**, and clamping to 68–99 silences five octaves of a keyboard.
`tests/host/test_fx_midi_filter_call_sites.sh` asserts that as an *absence* —
a test that only checked "the guard exists" would pass with it wrongly applied.

**Master FX → Settings → MIDI Ch** (`master_fx_midi_channel` in
`shadow_config.json`; param `master_fx:midi_channel`, −1 = All) selects the
listen channel. It lives on `MASTER_FX_SETTINGS_ITEMS_BASE` and in
`MASTER_GRID_PARAMS`, **not** in Global Settings — the first cut put it under
Global → Audio beside the other `master_fx:*` shim settings, which is where the
*plumbing* lives but not where a Master FX setting is looked for, and it was
reported missing from the device. Note the two representations: the wire
(`master_fx:midi_channel`, the config key, and the shim's variable) carries the
REAL channel (−1 = All, 0–15), while an enum cell is addressed by OPTION INDEX
(0–16). They are off by one and disagree about All, so the conversion is pinned
to `createMasterGridIo`'s `getParam`/`setParam` in `shadow_ui_slot_grid.mjs`
(`mfxMidiChannelToIndex` / `…FromIndex`) rather than repeated per call site.

**Default All**, deliberately: Master FX heard everything
before this existed, so any other default silently kills every sidechain in
the field — and a user whose ducker stopped after an update cannot connect
that to a setting they never saw. Note that the channel setting **cannot**
substitute for the pad guard: pads and steps share one cable-0 surface, so no
channel value separates them. Persisted like `usbc_out_persist` and parsed by
the shim at init (`shadow_resample.c`), so the filter is in force before the
first SPI frame. An out-of-range stored value fails **open** (All) rather than
muting every FX with no visible cause.

### Overtake Modules

Take full UI control in shadow mode. Listed in Tools menu below "Overtake" divider. Set `component_type: "overtake"` to keep the overtake lifecycle (LED clear, ~500 ms init delay, Shift+Vol+Jog-Click exit).

Requirements: handle all MIDI via `onMidiMessageInternal/External`; use progressive LED init (output buffer holds ~64 packets, >60/frame overflows):

```javascript
let ledInitPending = true;
let ledInitIndex = 0;
globalThis.tick = function() {
    if (ledInitPending) setupLedBatch();  // 8 LEDs/frame
    drawUI();
};
```

Lifecycle: host clears LEDs ("Loading...") → ~500 ms → `init()` → run → Shift+Vol+Jog Click → host clears LEDs ("Exiting...") → return to Move.

Reference: `src/modules/controller/ui.js`.

**External device handshakes** (e.g. M8 Launchpad Pro): be proactive — send your init in `init()` immediately; device may have sent its request during the ~500 ms delay. Optionally retry in `tick()` until any valid response confirms connection.

## Module Install / Update

**schwung-manager (web UI at `http://move.local:7700`) is the single
install/update path** for the host and all modules. On-device, the shadow UI
keeps exactly two store surfaces: update *detection* (Settings → Updates →
Check Updates shows what's outdated and points at the web manager) and
pointer screens ([Get more...] / [Module Store]). The old on-device store
module is retired (source kept for the standalone/sim host; not shipped).

Catalog: `https://raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json`.

**Shim mirror + stuck-shim repair (web update).** The manager runs as `ableton`
and can't write `/usr/lib`, so a web update mirrors the new shim via the
setuid-root `schwung-heal` helper (synchronously in `post-update.sh` + the
manager), then **verifies `/data` shim == `/usr/lib` shim before stamping
`version.txt`** — a failed mirror leaves the version old so it stays retryable
instead of self-concealing ("already up to date"). This auto-fixes the shim only
for devices whose `heal` is **blessed** (root-owned + setuid — i.e. they ran
`install.sh`/GUI installer since heal landed in v0.9.10). Never-blessed devices
(old pre-heal installs, web-only) can't be fixed over the web (no root foothold)
— they need one `install.sh`/GUI installer, after which it self-maintains.
`repair_status.go` detects the stuck state (`shimStale` md5 mismatch OR
`healUnblessed` OR non-heal-aware entrypoint) and shows a **repair banner** +
`/system/repair` page (SSH `chown root + chmod 4755 + heal --reboot`, or the GUI
installer). See memory `web-update-shim-bootstrap-gap`.

The manager also serves a **file browser** (`/files`, under `/data/UserData/`) and per-slot module **Remote UIs** (auto-discovers `web_ui.html` per module, served in a sandboxed iframe). The file browser is keyboard- and screen-reader-accessible: rows are `tabindex=0` with spoken `aria-label`s, **Enter opens** (dir → in, file → download), **Space selects**, with a checkbox column for multi-select. Source: `schwung-manager/templates/files.html`, `remote_ui.go`.

### Catalog Format (v2)

```json
{
  "catalog_version": 2,
  "host": {"name": "Schwung", "github_repo": "charlesvestal/schwung",
           "asset_name": "schwung.tar.gz", "latest_version": "0.3.11",
           "download_url": "https://.../schwung.tar.gz", "min_host_version": "0.1.0"},
  "modules": [{
    "id": "mymodule", "name": "My Module", "description": "...", "author": "...",
    "component_type": "sound_generator",
    "github_repo": "username/move-anything-mymodule", "default_branch": "main",
    "asset_name": "mymodule-module.tar.gz", "min_host_version": "0.1.0",
    "requires": "Optional: external assets (ROM, .sf2, etc.)"
  }]
}
```

### Flow

1. Fetch `module-catalog.json` → `modules[]`
2. For each module, fetch `release.json` from its repo on `default_branch`
3. Compare version to installed
4. Download tarball from `release.json.download_url`
5. Extract to category subdir (`modules/<component_type>s/<id>/`)

### release.json (on module's main branch)

```json
{"version": "0.2.0",
 "download_url": "https://github.com/user/move-anything-mymodule/releases/download/v0.2.0/mymodule-module.tar.gz"}
```

Repositories that publish multiple catalog modules may key each release by
catalog ID. Schwung Manager and the shared store utilities select the matching
entry before downloading:

```json
{"modules": {
  "module-a": {"version": "0.2.0", "download_url": "https://.../module-a-module.tar.gz"},
  "module-b": {"version": "0.2.0", "download_url": "https://.../module-b-module.tar.gz"}
}}
```

Optional: `install_path`, `name`, `description`, `requires`, `post_install`, `repo_url`. Release workflow should auto-update this file on each tagged release.

### Catalog Entry

Required: `id`, `name`, `description`, `author`, `component_type`, `github_repo`, `default_branch`, `asset_name`, `min_host_version`. Optional: `requires` (user-facing note about external assets like ROMs).

## External Module Development

External modules live in separate repos (e.g. `move-anything-sf2`, `move-anything-obxd`). Each has:

```
src/{module.json, ui.js, dsp/}
scripts/{build.sh, install.sh, Dockerfile}
.github/workflows/release.yml   # Triggers on v* tags, runs build.sh in Docker, attaches dist/<id>-module.tar.gz
```

`build.sh` must cross-compile DSP for ARM64 (Docker), package to `dist/<id>/`, and produce `dist/<id>-module.tar.gz`.

Release: bump `src/module.json` version → commit → `git tag v0.2.0 && git push --tags`. schwung-manager sees it within minutes. See `BUILDING.md`.

## Documentation Index

- `docs/API.md` — JS API reference (display, MIDI, host fns, LED colors)
- `docs/MODULES.md` — Module development guide (module.json, capabilities, tool_config, DSP API, Signal Chain integration, Remote UI `web_ui.html` + `schwungRemote` postMessage)
- `docs/LOGGING.md` — Unified logging
- `docs/SPI_PROTOCOL.md` — Full SPI reference
- `docs/REALTIME_SAFETY.md` — RT rules and JACK glitch root causes
- `docs/MIDI_INJECTION.md` — Cable-2 injection / echo filter history
- `docs/ADDRESSING_MOVE_SYNTHS.md` — Sending MIDI to Move tracks/slot synths from tools, overtake modules, chain MIDI FX. Ref: `src/modules/tools/seq-test/`.
- `../schwung-catalog-site/manual.html` — User-facing manual (canonical, lives in the catalog-site repo)
- `BUILDING.md` — Build system, cross-compilation

## Release Checklist

1. **Build**: `./scripts/build.sh` succeeds
2. **Deploy + test**: `./scripts/install.sh local --skip-modules --skip-confirmation`, verify on hardware
3. **Version**: bump `src/host/version.txt` and `module-catalog.json` (host `latest_version` + download URL)
4. **Docs**: update `CLAUDE.md`, `docs/API.md`, `docs/MODULES.md`, `src/shared/help_content.json`, and `../schwung-catalog-site/manual.html` for new features / changed behavior
5. **Help files**: update `help.json` in modified tool modules
6. **Module catalog**: bump `min_host_version` for modules depending on new host features
7. **Commit + tag**: `git tag v0.X.0 && git push --tags`
8. **Release notes**: `gh release edit` with concise bullets

## Dependencies

QuickJS (`libs/quickjs/`), stb_image.h (`src/lib/`), curl (`libs/curl/`, download backend for catalog detection + manual refresh).
