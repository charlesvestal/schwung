# CLAUDE.md

Instructions for Claude Code when working with this repository.

Schwung is a framework for custom JavaScript and native DSP modules on Ableton Move hardware (pads, encoders, buttons, 128x64 1-bit display, audio I/O, MIDI via USB-A).

Keep this file, `docs/API.md`, `docs/MODULES.md`, and the user manual in `../schwung-catalog-site/manual.html` in sync with code changes (see Release Checklist).

**Four subsystems live in their own files and are NOT summarised here** — the knob
grid (`docs/PARAM_PAGES.md`), the shadow UI (`docs/SHADOW_UI.md`), the chain
contract (`docs/CHAIN.md`) and device measurement (`docs/DIAGNOSTICS.md`). What is
left behind for each is a short list of its most surprising rules, so that you know
to go and read the file **before** touching that code, not after. See Documentation
Index.

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

### install.sh REWRITES features.json, and a key it does not list was reset

Not merged — rewritten from a literal in the script, so **a key absent from that
literal reverted to its default on every single deploy.** Seven did:
`set_pages_enabled`, `midi_indicator_enabled`, `skipback_require_volume`,
`skipback_seconds`, `recall_quantize`, `metronome_mode`, `metronome_level` —
every one of them written to this file by `features_json_set()` in
`shadow_ui.c`, none of them listed. The symptom is a settings page that has
quietly gone back to its defaults after an update, which reads as *"the update
reset my settings"* rather than as an installer bug, and so was never filed as
one.

Enumerating the missing keys fixes it once and re-breaks it the next time
somebody adds a setting — which is exactly how it reached seven. **The rule is
inverted instead**: the script owns the six keys the INSTALLER decides (a CLI
flag can override them, or they migrate from a legacy name) and carries every
other line across verbatim, so a new setting is preserved from the day it is
first written. `tests/host/test_features_json_preserved.sh` lifts the merge out
of `install.sh` and runs it, and derives its key list from `shadow_ui.c` rather
than restating one.

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
**Device diagnostics — `docs/DIAGNOSTICS.md`.** On-device E2E
(`tools/pytest-schwung/`), OTLP span tracing, the param tally and the SPI frame
tally, each with its arming file. Read it before measuring anything on hardware:

- A late SPI frame is **never a gap — it is a BURST.** ablspi's IRQ is a counting
  semaphore, so an overrun queues and the next waits return immediately, replaying
  back-to-back. That shape gets attributed to somebody else's producer misbehaving.
- **`total_us` is not a load signal** — our work shrinks the driver's wait by the
  same amount. The old overrun counter fired on *every* frame: 43,986 in two
  minutes on an idle device.
- The tally stays **silent for ~20 s after arming**, which looks like a broken build.
- The **CPU usage page** (`/system/cpu`, schwung-manager, `/schwung-perf`) is the
  one diagnostic that is **always on** — its timing was already being collected
  unconditionally, so only its 1 Hz polling is armed, by a button, not a file.
  It shows two numbers (frame budget vs process CPU) that must never be added —
  modules are not processes, so a module's cost already sits inside
  `MoveOriginal`'s `/proc` percentage. MIDI FX are not separable and land in
  `proc_midi`.
- A **fork-parallel module** (JP-8000) hides from the frame budget — its DSP
  runs in child processes the CPU page can't find by name (`comm` is
  inherited; one child reported as `Audio Main/SPI`, same as six of
  MoveOriginal's own threads). Attribution walks the process **tree** minus
  the four shim helpers; ownership is `capabilities.forks_processes`, else a
  marked inference, else unattributed. Module identity is read from disk
  (`active_set.txt` → `chain.synth.module`), not the param channel.
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

### `host_api_v1_t` ends in a run of NULLs, and that is not padding

Every module guards its host calls as `if (host->fn) host->fn()`. That is only
sound while a read *inside* the struct is the only read that can happen — and
it isn't. **A module's copy of `plugin_api_v1.h` can declare a field we do not
have**, and the guard then tests memory belonging to somebody else.

breakbeat is the case. Its header appends `float (*get_project_bpm)(void)`
after `get_beat_position` — a callback **no Schwung has ever provided** — so it
resolves to **+120**, one past our last field, while the same binary calls the
real `get_bpm()` at **+88**. Its own comment reads *"Appended host callbacks.
Keep these at the end for ABI compatibility"*, which is the right instinct in
the wrong direction: appending lets a module be **older** than the host, never
newer. A module cannot extend this struct from its side. The fix is upstream
([mestela/schwung-breakbeat#3](https://github.com/mestela/schwung-breakbeat/pull/3));
this is the backstop. The struct a chain sub-plugin receives is
`chain_instance_t::subplugin_host_api`, so +120 read **the next member of that
heap instance** — non-NULL, so the module's own guard passed, and the `blr`
jumped into a `rw-p` page. SIGSEGV on the SPI callback at load, which takes
MoveOriginal with it and **boot-loops the device**, because the slot is restored
every boot and crashes before the UI can be used to remove it.

`void *reserved[8]` at the end absorbs 64 bytes of that drift, so an over-read
finds NULL and the caller's existing guard does what it was written to do. Every
instance is zeroed by construction (`mm_init` memsets, `shadow_host_api` is BSS,
`overtake_host_api` is a static, `chain_host` memcpy's `sizeof()`).

**It does not make the ABI extensible** — appending a real field still requires
rebuilds. It buys a safe failure instead of a crash. So **consume `reserved`
from the front** when adding a field and never reduce the total;
`tests/host/test_host_api_reserved_tail.c` fails on a shrunken tail, on a field
appended *after* `reserved`, and on +120 specifically.

**The diagnosis needed the load base.** The shim's SIGSEGV handler prints `pc`,
`lr` and `sp` plus a `/proc/self/maps` dump to
`/data/UserData/schwung/crash_maps.txt` (async-signal-safe `open`/`read`/`write`
— the unified logger buffers, so a line logged *before* the crash is lost with
it). `v2_load_synth` logs the dlopen'd module's `dlinfo` base, which is what
turns a raw `lr` into `lr - base` and an `addr2line` offset. Without both halves
the address is unattributable under ASLR.

### JS Host Functions

Module management: `host_list_modules`, `host_load_module`, `host_load_ui_module`, `host_unload_module`, `host_return_to_menu`, `host_module_set_param/get_param/send_midi`, `host_is_module_loaded`, `host_get_current_module`, `host_rescan_modules`, `host_get_module_metadata(id)`.

Volume / settings: `host_get_volume`, `host_set_volume`, `host_get_setting/set_setting/save_settings/reload_settings` (keys: `velocity_curve`, `aftertouch_enabled`, `aftertouch_deadzone`).

Jack state (for feedback gate): `host_speaker_active()` (true = speakers, false = headphones), `host_line_in_connected()`.

Display: `host_flush_display`, `host_set_refresh_rate(hz)`, `host_get_refresh_rate`.

Filesystem: `host_file_exists`, `host_read_file`, `host_write_file`, `host_http_download`, `host_extract_tar(_strip)`, `host_ensure_dir`, `host_remove_dir`.

Tool lifecycle: `host_exit_module()`. MIDI injection: `move_midi_inject_to_move([type, status, d1, d2])`. **Three producers inject into Move's MIDI_IN and each owns its own queue, because ownership follows WHO PUSHED and never what mode the surface is in** — the overtake test bus keeps `/schwung-midi-inject` (during overtake the shim pops it *onto the module*, as if a control had been pressed), an overtake DSP's `midi_inject_to_move` has an in-shim ring (`shadow_overtake_move_inject_active()` detects it), and the shadow UI's JS binding has `/schwung-midi-inject-ui`. Sharing the first of those with JS is what broke song-mode for a month: its injected Play CC never reached Move and came back into its own `onMidiMessageInternal`, toggling playback and re-injecting, so it fired pads as fast as the queue drained. See `docs/ADDRESSING_MOVE_SYNTHS.md`. Sampler: `host_sampler_start(path)`, `host_sampler_stop()`, `host_sampler_is_recording()`.

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

### An SHM buffer sized to `sizeof` reads as FULL, and is not

`CONTROL_BUFFER_SIZE` was 84 with a `sizeof(...) == BUFFER` assert — not a
designed size, just wherever the struct happened to end after the corun masks
were widened. Adding a byte failed the build, which looks like a hard limit,
and is why Recall Quantize was first squeezed into two spare bits of
`ui_flags_ext` — costing a mask, a shift and one real bug (a flat mask applied
to ext space, which reads zero always).

**It costs nothing.** `/dev/shm` is tmpfs and allocates by page: measured on the
device, an 84-byte segment occupied 4096 — the same 8 blocks as a 512-byte one.
Both buffers are containers with headroom now (256 / 512) under a `<=` assert
plus a floor, so adding a field is free and only SHRINKING fails the build.

**And a resize is not a SIGBUS.** `shadow_shm_map()` fstats on attach and
refuses a segment shorter than requested, logging "restart the shim so it is
recreated" — the feature goes quiet rather than crashing. Two earlier resizes
(#358, #361) used exactly that procedure. Deploy the shim and shadow_ui
together, as `install.sh` does; growing is also safe for an old consumer, which
asks for less than the segment holds.

### Blocking an event means silencing TWO buffers, and eleven sites silenced one

`shadow` (`= global_mmap_addr`) is **what Move sees**. `hardware_mmap_addr` is
the real mailbox, which **Move never reads** — Schwung's own post-ioctl scans
do. Twelve sites in `shim_post_transfer` meant "block this from reaching Move"
and **eleven zeroed only the hardware mailbox**: no block at all, and worse than
a no-op, because a zeroed slot is a terminator, so they hid every event *behind*
them from our own readers. Exactly backwards, at every one.

It surfaced only when **Shift+Delete reached Move and deleted a clip**. The
other ten leaked into Capture, Sample, Back, Jog Click and the arrows — none
destructive, which is why they went unnoticed and why the broken form read as
the house style. `midi_in_swallow(shadow + MIDI_IN_OFFSET, src, j)` is now the
only sanctioned way; `tests/host/test_midi_in_swallow_pairs_buffers.sh` fails on
any hand-rolled hardware-mailbox zeroing.

**Swallowing a button needs BOTH EDGES, latched.** Press-only leaves Move a lone
button-up for a key it never saw go down, and Move acts on it. The release
cannot be gated on `shadow_shift_held` either — Shift is usually let go *before*
the button.

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

Telemetry: `touch /data/UserData/schwung/link_audio_avail_log_on` for 5 s slot avail logs. For raw audio, `echo 30 > /data/UserData/schwung/align_dump_trigger` writes that many seconds of s16le stereo to `slot0_move_track.pcm` (Move's Link Audio) and `slot0_synth_src.pcm` (the module's own output) — the two summands of a slot's mix, captured separately at the point they are combined. Score them with `tools/link-audio/analyze_capture.py`.

**The capture is RT-safe and the dump length is not cosmetic.** It used to `fopen`/`fwrite` on the SPI callback for the whole capture, so the instrument could perturb the timing fault it was measuring; `src/host/align_capture.{c,h}` now memcpys into a preallocated buffer and the worker writes it. And 2.9 s was too short to tell signal from variance — the same configuration measured 5.61x, 1.84x, 1.01x, 2.58x and 2.94x across five consecutive snapshots.

**A starved frame is captured as SILENCE, not skipped.** Skipping spliced the file across the gap, so a starve read as a waveform discontinuity indistinguishable from a real one.

### The IN ring is sized for Move's jitter, not for symmetry

`LINK_AUDIO_IN_RING_BLOCKS` was `LINK_AUDIO_PUB_SHM_BLOCKS` (16 blocks = 4096 stereo samples = **46 ms**), inherited from the publish side because the two sit next to each other in `link_audio.h`. The directions do not have the same problem: we write the pub ring on a metronome, one block per SPI frame; **Move writes this one in bursts.**

Measured 2026-08-27 with the sidecar's `cb slot=N max_gap=... max_burst_run=...` telemetry, under a load that provoked it:

```
max_gap       = 92 ms       Move publishes nothing at all
max_burst_run = 30 blocks   then 30 callbacks back-to-back (~85 ms)
avail max     = 13128 stereo samples = 149 ms
```

All four channels stalled within 5 ms of each other — one shared stall in Move's publisher, not per-channel jitter. A 46 ms ring is empty less than halfway through a 92 ms stall and cannot hold the burst that follows, so the producer lapped the consumer. **~16% of frames had no Move audio at all.**

Worse, catch-up was **fighting** the burst: the threshold was a bare `need * 12` at the call site (3072 samples ≈ 35 ms), chosen when "observed bursts were consistently <30 ms". Every refill that could have covered the next stall was discarded — ~76,000 stereo samples per 5 s window, ~17% of the audio — guaranteeing the next stall starved too. Starve, burst, discard, starve.

Now 64 blocks (**186 ms**), with `LINK_AUDIO_IN_CATCHUP_SAMPLES` **derived** from the ring (3/4 of it) rather than written beside it, so a resize cannot leave the two disagreeing. This adds no steady-state latency — the consumer sets the pace, taking one block per SPI frame whatever the ring holds, so mean `avail` stays where Move's delivery puts it (~16 ms). It only changes what happens during a burst: absorbed instead of discarded. After: starve, catchup, would_overrun and la_starve_fallback all **zero**, and Move's own `max_gap` fell to 14–32 ms.

**`LINK_AUDIO_IN_SHM_VERSION` must be bumped with any resize** (now 3). The struct grew 32 KB → 128 KB, so a segment left by an older sidecar is not merely stale, it is *too short* for the new mapping — and touching the tail of an undersized mapping is SIGBUS. `magic` and `version` must stay the **first two fields** so the version check itself can be read safely off a short segment. `tests/host/test_link_audio_ring_sizing.sh` pins the margins against the measured numbers, not the constants.

### build.sh used to skip the sidecar silently

`libs/link` is a submodule. Uninitialised, `build.sh` printed a warning, **exited 0**, `package.sh` added the sidecar only "if it was built", and `install.sh` only ever *kills* `link-subscriber` — it never installs one. So the copy on the device never changed, and rode through weeks of deploys and a three-host-version bisect of a Link Audio bug as the one component nobody was varying. It is a hard build failure now (`SCHWUNG_ALLOW_NO_LINK_SDK=1` to opt out), CI verifies the binary and the tarball entry rather than warning, and the moral is general: **a build step that can be skipped silently defeats every bisect that follows.**

## Signal Chain Module

```
[Input or MIDI Source] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

Modules declare chainability in module.json: `capabilities.chainable: true` + `component_type: sound_generator|audio_fx|midi_fx`.
### Chain contract and architecture — `docs/CHAIN.md`

The `ui_hierarchy` and `chain_params` declarations (what a module must publish for
the shadow UI to know its steps, ranges and enum options), the chain host's file
layout, and the shape-edit verbs. Read it before touching `modules/chain/dsp/`.

- **Use `key`, not `param`**, for editable `params` entries — metadata comes from
  `chain_params`, and a module missing it gets an invented `float 0..1 step 0.01`
  knob writing `0.058750` into an enum.
- **A knob drives up to four destinations, each through its own window** (a
  slice of that parameter's range, stored as fractions so it survives
  re-pointing). ⚠ **One destination keeps its parameter's own step and enum
  feel; only SEVERAL are driven by the knob's 0..1 position** — driving a lone
  enum from a position needs ~95 detents per option instead of one. The line is
  "several destinations", not "has a range".
- **A plain read of a modulated key answers the BASE**, never the plugin's value
  — the plugin holds the effective value the overlay keeps writing into it. The
  driven value is asked for as `<key>:effective` (#276).
- **A chain shape edit is a PERMUTATION, never a reload.** `fx:insert` /
  `fx:remove` / `fx:move` keep instances running; a run of `<id>:module` writes
  rebuilds every position behind it, losing arp phase and reverb tails.
- Per-position arrays split into VALUE and **OWNED-BUFFER**. Zeroing an owned
  pointer instead of rotating it is a SIGSEGV on the SPI callback.
- **`synth:last_note` is recorded at BOTH synth-feed paths**, via
  `chain_record_synth_note`. `v2_tick_midi_fx` is the one that looks optional
  and is not: an ARPEGGIATOR emits from `tick()`, not `process_midi()`, so
  instrumenting only `v2_on_midi` means it never updates with an arp in the
  slot. It reports a NOTE, not a voice index — the canonical voice order lives
  in `voices.mjs` and a C copy of it would fail silently as "the grid follows
  the wrong pad".

### The knob grid / param pages — `docs/PARAM_PAGES.md`

~1000 lines on the page planner, every widget and the rule that selects it, the
peek / flip / dive gestures, animation, and the read budget. **Read it before
touching `src/shared/param_pages/`, `shadow_ui_param_pages.mjs`, or any draw path
in `src/shadow/shadow_ui.js`.** The load-bearing claims, so you know when to look:

- **An IPC read is ~2.8 ms; a whole page render is 1.68 ms.** Nothing reads on the
  draw path — values arrive on touch-down, on the rotation, or in the entry warm.
- **A read that did not answer must never become a picture.** Placeholder frames
  animated the first page of 46 of 95 fleet modules in.
- **`render()` is not the whole draw.** Nothing in `src/shared/param_pages/`
  clears the screen — that is what lets `render(ctx, {rect, bands})` place a
  page inside a caller's chrome — so anything full-screen is the frame owner's
  second call: `controller.renderOverlays(ctx, { clearScreen })`. Today that is
  the enum peek. It lived in `shadow_ui_param_pages.mjs`, where it was
  invisible to every module binding the controller from its own `ui_chain.js`:
  the peek was tracked on each detent, `applyInput` swallowed the Back that
  dismissed it, and it was painted nowhere. CW-78 and 6W6 both shipped that
  way.
- **Two-option enums: the GRID flips on click, a LIST focuses instead** — and on
  the KNOB they split BY WIDGET, not by semantics: a **switch** has a track, so
  its form names a direction and it is direction-absolute (clockwise on,
  idempotent, no latch); the **boxed** enum square shows a state and no
  direction, so it TOGGLES either way, latched to one flick. The turn partition
  must EQUAL the draw partition or a shape promises what the knob won't do.
  `flipsOnClick` defines "is a two-way", not "flip".
- **Corner brackets and the chevron box do NOT both mean divable.** 967 divable
  cells on knob pages, 953 of them wearing no mark. Divability is a FOOTER fact.
- **`access: "read"` is a STROKE, not a widget** — dotted, ONCE per cell,
  wherever that cell's stroke already lives: a frameless dial gains a frame, the
  enum square dots the one it has, the opaque box is left alone. The input layer
  honoured readOnly for a long time while the DRAW layer did not, so a readout
  was pixel-identical to a control.
- A momentary fires from the knob too, **latched per gesture** — a rate limit
  still fires eight times across a two-second spin.
- **A module's OTHER draw surface is a CARD, and it FLOATS.** `drawCell` gives it
  one cell; `card_script` gives it the page — a bordered picture raised while a
  knob is held, gone on release. It is centred in the page's **FRAME**, not on
  the panel (`render()` takes a `rect`, so a full-screen centre painted over an
  embedded host's own chrome), and it blanks only its own rect, which is why it
  needs no `clearScreen` while the enum peek does. Same `frameCtx` contract as a
  widget, for a second reason: `card_w`/`card_h` are declared **per parameter**,
  so coordinates authored against one card are wrong on the next.
- **A graphic must sit inside ONE ROW**; `alignGroupsToRows` reflows 24 fleet
  pages to keep it there, as a permutation *within* a page.
- Every scrolling list draws a scrollbar, and no list draws arrows.
- **A module-supplied widget draws into a FRAME and cannot name a screen pixel** —
  the rect varies sixteen ways (`cellW` is caller-dependent, `rowH` is dynamic and
  picks the render mode, a right-edge span is clamped). An unknown `custom:` kind
  falls through by **not claiming its keys**, one branch covering a typo, a failed
  load, an older host and a one-strike disable. Guarding in the shared walk
  instead of the singles branch silently yields a THREE-cell envelope with a key
  orphaned.
- **`level_walk.mjs` is the walk, and the LFO target picker is its second
  consumer.** Names must not be copied — nothing shows a grid page title beside
  the picker's row for the same level.
- **A module DECLARES whether it is a rack or a keyboard; it is never
  inferred.** `pad_layout` at the top of `ui_hierarchy` is `drums` | `chromatic`,
  and **absent is a third state** — all 100 captured fleet modules are in it.
  The tempting shortcut, "it has notes on its pages so it is drums", is wrong:
  key zones, multitimbral parts and chord modules all carry notes on melodic
  pages. Voices are described separately, and the two axes never imply each
  other. **ROOT ITSELF CAN BE THE RACK** — mrdrums declares its 16 pads *on*
  `root`, not in a sibling level, and skipping root made an earlier build a
  complete no-op for the flagship drum module while every fixture passed.
- **THE MODULE OWNS THE FOCUS; nothing infers it from what is PLAYED.**
  `child_index_param`, else `focus_param`, else the grid does not follow — and
  declaring neither is a valid choice, not a gap. A `synth:last_note` fallback
  was tried and deleted: **a sequencer plays notes**, so a running pattern
  changed the page on every hit in the bar, and a pad press cannot be told from
  a clip anyway (both arrive through the same MIDI_OUT echo). `last_note` is
  still served as a diagnostic and the test asserts it is never READ, because a
  read is what someone later starts navigating on again.
- **A focus answer may carry a CHANGE TOKEN — `"<count>:<level>"`.** The follow
  acts on a change, so a repeat does nothing — correct while a value is
  re-reported, wrong when it marks a second hit on the pad you are already
  editing. Hit kick, jog to Reverb, hit kick: a bare name leaves you on Reverb.
  9W9 published a counter for this before the contract existed; reading the
  fleet before designing at it is the whole lesson. `focusToken`, `voices.mjs`.
- **The header pad minimap is a PHYSICAL map**, gated on `pad_layout: "drums"`:
  the lit cell is the voice's note minus 36, so it shows where the pad is under
  your hand. A map matching the page order would be a second bank bar. Move's
  rack counts up from the BOTTOM-LEFT; off-rack draws the empty box rather than
  the nearest cell.
- **The voice-follow path writes no pad LEDs.** Move owns the pads while the
  shadow UI is up; `tests/host/test_voice_follow_no_leds.sh` fails on a MIDI or
  LED write in `syncVoiceFromModule` or `voices.mjs`.
### Recording / capture

Audio capture is shim-side: the Quantized Sampler (Shift+Sample) and Skipback
(Shift+Capture) — see Shadow Mode below. (The old chain-host CC 118 recording
was deleted in the 2026-06 cleanup; it was only reachable through the
unreachable v1 plugin path.)

**Save Stems — `docs/SHADOW_UI.md`.** Global Settings → Audio → **Save**
(Master / Stems / Both), honoured by all three recorders — the sampler,
Skipback and **Song Mode's Record button, which needed no new recording code
because it already went through the same sampler**.

- **A stem is a SLOT, and that is forced, not chosen.** Under Move→Schwung the
  shim builds a slot as `move_track[s] + synth[s]` and *then* runs the slot FX
  on the SUM, so Move's track and Schwung's synth are inseparable after that
  point — the four slot stems ARE the four tracks, and they sum to the master
  exactly. A fifth **Move** stem carries the mailbox mix for the case
  Move→Schwung is OFF and there is no split to be had; under Move→Schwung it is
  left INVALID on purpose, or a stem sum would double every instrument.
- **Stems are PRE-Master-FX.** MFX runs on the summed bus, so with a chain
  loaded the stems do not add up to the master file.
- **The capture gate opens on the RT ARM, not in the worker** — `sampler_state`
  is RECORDING the moment the arm returns and the worker runs ~200 ms later, so
  opening it there put a few hundred ms in the master and not the stems, offset
  for the whole take by the master's preroll trim. The mode is latched there
  too. **And stems are captured BEFORE the master**, because both apply the
  fade-in ramp and the master is what consumes the counter.
- **A silent stem's file is DELETED at finalize, never opened lazily** — a lazy
  open would start the file at the first sound rather than at t=0.
- Skipback stems are capped at **60 s** against the master's 5 minutes (five
  rings at the maximum is ~265 MB) and are a SUFFIX of it.

## Shadow Mode

Shim intercepts hardware I/O to mix shadow audio with Move's output.
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
### Shadow UI internals — `docs/SHADOW_UI.md`

The synthesised contracts (Global Settings, Slot Settings, Master FX Settings), the
component load gate, and the input-dispatch order. Read it before editing
`src/shadow/shadow_ui.js` or its `.mjs` siblings.

- **Whatever is drawn LAST must be fed FIRST.** The draw path is a switch with the
  overlays painted after it; the input path is a run of early-outs *before* it. The
  two orders are the reverse of each other, and nothing at either site says so.
- **A timed-out read empties NOTHING, and latches nothing.** A `null` recorded as
  "this position is empty" made a filled chain position open the module picker —
  and the *correct* read milliseconds later is what made it permanent, by matching.
- **A component editor WAITS; it does not decide from one read.** Everything that
  knows how to wait sits behind the entry, and the fallback is irreversible.
- Global Settings is seven sections = seven PAGES. **One section, one page** is
  load-bearing — but the rule is "never SPLIT", not "never exceed eight".
  **Eight is the number of physical KNOBS**, and this screen is pinned to the
  LIST (`layout: LAYOUT_LIST`), which draws five rows and scrolls the rest.
  The planner was chunking it as a grid anyway, so a ninth param silently
  became a `<Section> - 2` page holding one row; it is handed `paginate: false`
  now and a section is one list however long. Audio holds nine. The flag is a
  property of the CONTRACT, never inferred from the layout — the layout is
  also `LAYOUT_LIST` with the screen reader on or Param View set to List, and
  a module's pages are authored groupings that must keep their shape.
- **The LFO target picker groups by level, and the grouping is LOSSLESS** — an
  orphan sweep into "Other", asserted over all 95 modules. It was one flat list
  of 418 rows for minijv. The group step is SKIPPED, not emptied, and Back
  branches on that. **A child level lists TEMPLATES** — resolve them through
  `child_key.mjs` or a drum module files 200+ keys under "Other".
- **A Track tap switches SLOT (`Keep Schwung`, default ON — a reversal); off it
  dismisses** — enforced in the SHIM, on the PRESS, *outside* the long-press block
  (that block is gated on the trigger mode, so a jump inside it works on
  `Both`/`Hold` and silently not on `Shift+Vol`). Shift+Track still exits. It is
  a **bool**, not a two-option enum: same click path, but only `off|on|…` words
  draw the SWITCH — anything else is the enum square, i.e. a menu.
- **Module lists file a module into Favorites or your own lists, and
  `drawFooter` DROPS a hint pair that does not fit** — silently, with every
  pair after it, so the membership screen's PRIMARY action was the one word
  missing from its own footer. A footer names the verb of the row under the
  CURSOR. The swap picker's row 0 filters by list; the filter persists across
  pickers but is re-resolved per picker, and its cursor SCANS rather than
  counting, because the move rows sit under the loaded module.
### Shortcuts

Shadow UI access gated by **Global Settings → Shortcuts → Shadow UI Trigger** (`shadow_ui_trigger` in `features.json`): `Both` (default) / `Long Press` / `Shift+Vol`.

**Shift+Vol combos** (modes Both / Shift+Vol):
- **Shift+Vol+Track 1–4** — open shadow / jump to slot settings
- **Shift+Vol+Menu** — Master FX
- **Shift+Vol+Step2** — Global Settings
- **Shift+Vol+Step13** / **Shift+Vol+Jog Click** — Tools menu (overtake modules below the divider). Jog-click also exits an active overtake module.
- **Shift+Sample** — Quantized Sampler
- **Shift+Capture** — Skipback (last 30 s)

Both write the mixed master by default, or per-track stems, or both — Global
Settings → Audio → **Save** (see Recording / capture).

**Anywhere** (independent of the trigger mode, and whether or not the shadow UI
is on screen):
- **Shift+Copy** — snapshot every slot + Master FX
- **Shift+Delete** — put the snapshot back

**Long-press** (modes Both / Long Press):
- **Hold Track 1–4 (500ms) → TOGGLE between the two worlds.** From Move it opens
  that slot's editor; from the shadow UI it dismisses and leaves you on that
  Move track. Its own inverse, so you long-press back and forth. The Move-track
  tap is injected on BOTH directions — it is what made Move's selected track
  follow the slot, and it is why the dismiss lands somewhere useful rather than
  on whatever track Move was on.
- Hold Menu (500ms) → Master FX
- Shift + hold Step 2 (500ms) → Global Settings
- Shift + Step 13 (immediate) → Tools menu
- Tap Menu while shadow UI shown → dismiss. **Tap Track → switch to that slot**
  (**Global Settings → Display → Keep Schwung**, default ON; off restores the
  old dismiss). Shift+Track dismisses either way.

Long-press is suppressed once the volume knob is touched during a track press (so Track-hold + knob adjusts track volume without opening shadow UI). See `track_vol_touched_during_press[]` in `schwung_shim.c`.

**While shadow UI shown** (any mode):
- **Mute + Jog Click** on focused chain/MFX module — toggle bypass. Audio passes through; MIDI FX become passthrough; synth render silenced while MIDI flows (state advances, tails ring out, clean unbypass). 4-row 'B' glyph above the module box.
- **Mute + Track 1–4** — slot mute. **Shift + Mute + Track 1–4** — slot solo.

Mute (CC 88) is passed through to Move firmware (even while shadow UI is shown) so Move-native **Mute + Pad** (per-drum mute) works. `shadow_mute_held` is tracked from the hardware buffer independently, so the shadow combos above still work. Consequences: a plain Mute tap also toggles Move's selected-track mute, and Mute + Track double-mutes (shadow slot + Move track) — these stay in sync, which is intended. Shadow slot mute/solo is set **only** by these combos — there is no D-Bus screen-reader text sync. (A former `shadow_dbus.c` auto-correct matched any announcement ending in " muted"/" soloed" and applied it to the selected slot; Move utters drum kit/pad names with those suffixes — e.g. "Lay Down Kit muted" — and Schwung's own TTS loops back through the same handler, so it spuriously muted slots and persisted the state, silencing audio across all projects. Removed; a version-stamped one-time heal in `shadow_state.c` clears any already-stuck persisted mute/solo on upgrade.) Bypass persists via per-slot autosave (`slot_N.json`, `master_fx_N.json`); patch-library reloads start with bypass=0.

### A master-bus metronome is gone under Move→Schwung by CONSTRUCTION

- Not by a bug: `rebuild_from_la` composites only the four per-track Link Audio
  slots, and Move mixes its click at master. Schwung plays its own, detected
  from Move's `"Metronome On"` / `"Metronome Off"` announcement — **exact
  equality on the whole normalised string**, which is what separates it from
  the removed mute auto-correct that matched a suffix and fired on Move's own
  drum-kit names. **Never persisted, because Move does not persist it either**
  — that is what makes off-at-boot the truth rather than a guess. The click
  mixes **between the `unity_view` snapshot and the master-volume scaling**, so
  it is on the DAC and in no recording. `Main − Σ(tracks)` is NOT the
  metronome (`returnTracks`, `masterTrack`).
- **A click that is "early, worse at low tempo" is a PHASE error, not a
  latency.** `shadow_transport_pulses` is zeroed on MIDI Start and incremented
  by the first clock — which IS the downbeat — so beats sit at **24N+1**, and
  firing at 24N was one pulse early (125 ms at 20 BPM, 20.8 at 120). Measured
  at two tempos to separate it from the 19.6 ms Link Audio transit stacked on
  top; **one tempo cannot separate two terms.** `recall_quantize` had the
  identical off-by-one and is fixed with it — the grid now lives once, in
  `src/host/transport_grid.h`, because one fact with two consumers written
  down nowhere is how both got it wrong. See `docs/SHADOW_UI.md`.

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
### Snapshot / recall — `docs/SHADOW_UI.md`

Shift+Copy snapshots all 4 slots + 8 Master FX, Shift+Delete puts it back.

- **A recall writes STATE, never SHAPE.** `load_file` is what restores module
  identity and it REINSTANTIATES — cutting reverb tails, resetting arp phase,
  which is the opposite of an A/B. A position whose module was swapped since is
  skipped and **counted**; the count is the whole feature, because a partial
  restore that reports nothing is indistinguishable from a working one.
- **Recall Quantize** (Global Settings → Shortcuts, default Off) makes
  Shift+Delete wait for the next beat / bar / 2 bars. A SETTING, not a second
  gesture. Ignored while the transport is stopped — a queue with no clock never
  fires. The division is a field in `shadow_control_t`;
  `load_feature_config()` runs once at init, so a setting parsed there would
  need a reboot. `sampler_clock_count` is NOT a beat
  counter — it only advances while the sampler is RECORDING — so the queue uses
  `shadow_transport_pulses`. The boundary maths is in `recall_quantize.h` so
  `tests/host/` can run it: the next boundary is never the current one, and the
  lead is clamped below the division or a fast tempo degrades it to instant.
- **The snapshot is re-seeded from the set on every set load**, so it means one
  sentence and is never older than the session. It lives in
  `set_state/<uuid>/snapshot/` — a global dir would be the one piece of chain
  state that does not travel with the set.
- **There is no second serializer.** A take is `autosaveAllSlots()` +
  `saveMasterFxChainConfig()` and a file copy; those writers already carry every
  guard (bail-if-empty, skip-if-unchanged, shim-reports-empty).
- **`ui_flags` is FULL and cannot be widened** — `ui_patch_index` sits at +8
  with no padding, so a uint16 moves every field behind it and `sizeof` is a
  contract between two binaries. Flags 0x0100+ live in `ui_flags_ext` (was
  `reserved16`); the JS binding presents one flat word.
### USB-C Audio-Out Source

Move's Settings menu picks what a connected computer receives over USB-C (Mic or
Main Out), and Move's firmware **never persists it**. Schwung remembers it instead
— **the SysEx wire format and the boot arbitration are in `docs/SPI_PROTOCOL.md`.**
**Global Settings → Audio → USB-C Persist** (`usbc_out_persist`, default On)
governs whether Schwung restores it.

- Selecting a value emits a **pair** of `37 12` / `37 14` messages — but Move's
  *sampling* page emits a **lone `37 12`** that clears the monitoring bit, silently
  reverting USB-C out to Mic while `37 14` still reads Main Out.
- Persistence is gated **CAUSALLY, not on a deadline.** It was a ~7 s deadline, and
  that deadline was the bug: a slow boot put Move's own Mic assert on the trusting
  side of the line and clobbered the stored preference.
- **Move's own Settings screen keeps reading "Mic"** even when the hardware is on
  Main Out. Selecting "Mic" there, believing it a no-op, actually switches it off.
- `xmos_audio_emit` is the **only** sanctioned way to put SysEx into MIDI_OUT.
### Shadow Architecture

`src/schwung_shim.c` (LD_PRELOAD, intercepts ioctl, mixes audio), `src/shadow/shadow_ui.js` (slot/patch UI), `src/host/shadow_constants.h` (SHM structs).

SHM segments: `/schwung-audio` (mixed shadow output), `/schwung-control` (`shadow_control_t`), `/schwung-param` (param requests, `shadow_param_t`), `/schwung-ui` (`shadow_ui_state_t`).

`shadow_control_t.ui_flags`: `JUMP_TO_SLOT (0x01)`, `JUMP_TO_MASTER_FX (0x02)`, `JUMP_TO_OVERTAKE (0x04)`. **Flags 0x0100+ live in `ui_flags_ext`, not here** — the 8-bit field is full and widening it moves every field behind it.

### Shadow Slot Features

Each of the 4 slots has:
- **Receive channel**: 1–4 (default) or All (−1)
- **Forward channel**: 1–16 or −1 (auto: remap to receive ch, or passthrough if receive=All) or −2 (THRU: preserve original ch). Modules can declare `default_forward_channel` in capabilities.
- **Volume**, **state persistence** (synth + FX + MIDI FX).

**MPE controllers** (LinnStrument, Roli, Sensel): set Receive=All, Forward=THRU, enable MPE in the synth. Otherwise channel remap destroys per-note bend/pressure/slide.
### User Presets, and the two trailing pages — `docs/SHADOW_UI.md`

Per-component preset snapshots (the opaque `<prefix>:state` blob, saved under
`/data/UserData/schwung/presets/<module-id>/`, keyed by **module id** so they are
offered wherever that module is loaded), plus the **My Presets** and **Module**
pages the PLANNER appends to the end of every component's knob grid.

- The browser is exactly ONE thing: **choose a preset.** Picking a row loads it;
  Save / Save As / Delete live on the My Presets grid page.
- Auditioning is gated by `browser_preview`, **default OFF** — it applies state to
  the live slot, and autosave is suppressed while it is active.
- The trailing pages are appended **after the whole walk**, never injected into a
  level: 11 of 95 fleet modules publish no `levels` at all and minijv has no
  `root`, so there is no level an injection could target.
- Scope is the 4 chain slots' real components. **Master FX is excluded**, in one
  helper, so a new call site cannot silently opt it in.
- **`drawScrollbar` is EXPORTED from `menu_layout.mjs`** — a list is not the only
  thing that scrolls, and `scrollable_text.mjs` kept drawing the arrows it
  replaced, so the help list wore a bar and the help text wore arrows on the same
  jog. The help footer now names where Back *actually* goes (`helpBackTarget`):
  a detail returns to the list it was opened from, not that list's parent, and
  the **module name is reserved for the Back that LEAVES** — one level in reads
  `Back: List`, because frame 0 is titled with the module and the two adjacent
  screens meant different destinations by the same word. That frame's header is
  **`Help: <module>`**, and its 18-char cap is gone — `drawHeader` fits in
  PIXELS, and the cap cut 12 of 133 fleet names that all fit. The
  help text also had its **own 10px row pitch** against the list's 9 in the same
  rect — four lines where the list drew five — so it uses `LIST_LINE_HEIGHT` and
  `visibleLinesFor(topY, bottomY)` now, never a counted row total.
- The Module page's **`Module Help`** is the ONE component action that does not
  come back through `VIEWS.CHAIN_EDIT` — the help viewer is hosted by
  `VIEWS.GLOBAL_SETTINGS` — so it carries its own return pair and reconciler.
  It seeds **exactly one** help frame so Back lands on the MODULE rather than
  climbing the Help tree, and the row is hidden when the module ships no
  `help.json`.
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
### Master FX persistence, and what audio FX are fed — `docs/SHADOW_UI.md`

- **The SHIM says what is loaded, and it says it ONCE.** `master_fx:modules` is one
  GET returning the whole chain, positional and never compacted. The in-file mirror
  never saw anything written straight to the shim (an overtake tool, a Remote UI
  client) and wrote `{}` over it — the whole master chain gone on the next boot.
- **A STEP button is not a note, and audio FX were told it was.** The only guard was
  `d1 >= 10`, which exists solely to drop knob-touch notes 0–9, so steps (16–31) and
  tracks (40–43) reached every loaded audio FX as played notes. `fx_midi_filter.h`
  splits the cable-0 pad-range guard from the channel guard — **never apply the
  note-range guard to the external sites**, where a note number is a pitch.
- Master FX → Settings → **MIDI Ch** defaults to All, deliberately: any other
  default silently kills every sidechain in the field.
### Overtake Modules

Take full UI control in shadow mode. Listed in Tools menu below "Overtake" divider. Set `component_type: "overtake"` to keep the overtake lifecycle (LED clear, ~500 ms init delay, Shift+Vol+Jog-Click exit).

Requirements: handle all MIDI via `onMidiMessageInternal/External`; use progressive LED init (the shadow-UI MIDI-out buffer holds **128 packets per flush**, and a write past that is refused rather than silently wrapped):

```javascript
let ledInitPending = true;
let ledInitIndex = 0;
globalThis.tick = function() {
    if (ledInitPending) setupLedBatch();  // 8 LEDs/frame
    drawUI();
};
```

Lifecycle: host clears LEDs ("Loading...") → ~500 ms → `init()` → run → Shift+Vol+Jog Click → host clears LEDs ("Exiting...") → return to Move.

**The old figure here was "~64 packets, >60/frame overflows", and it was a
symptom, not a limit.** `shadow_midi_out_t.write_idx` is a byte offset into a
512-byte buffer and was declared `uint8_t`, so it saturated at 255: only the
first 63 packets were addressable, `write_idx = write_offset + 4` wrapped
252 -> 0 and silently rewound the buffer mid-flush, and the
`write_offset + 4 <= SHADOW_MIDI_OUT_BUFFER_SIZE` bounds check could never fire
because a `uint8_t` cannot reach 512 — it read as a working overflow guard and
was dead code.

Widening it to `uint16_t` costs nothing (it takes a reserved byte, `sizeof` is
unchanged, and both mappers use `sizeof`). A `_Static_assert` beside the field
now requires it to be able to address its own buffer.

Two consequences worth holding on to. **A dropped LED was permanent, not a
flicker**: `input_filter`'s `setLED` recorded the colour it believed it had
sent and suppressed the next identical repaint, so a lost packet was never
retried. It now caches only on a successful send. And **`move_midi_internal_send`
returned true either way** — the write discarded and reported success, which is
the same defect class as the tri-state read rule above. It now returns false and
the drop is counted and logged (rate-limited, from `shadow_ui`, which is
SCHED_OTHER and may log). Pinned by `tests/host/test_shadow_midi_out_capacity.c`.

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

**`CLAUDE.md` is an INDEX for the four files below, not a summary of them.** Each
was split out because it is ~1000 lines of subsystem detail that matters on the
sessions touching that subsystem and costs context on every other one. The bullets
left behind in this file are deliberately the *surprising* claims rather than the
topics — enough that you know a rule exists and go read it before you break it. A
new war story goes in the subsystem file with one bullet added here; adding it
inline is how this file got to 151 KB.

- `docs/PARAM_PAGES.md` — **The knob grid.** Page planner, every widget and the
  rule that selects it, peek / flip / dive gestures, animation, the read budget.
  Read before touching `src/shared/param_pages/` or any draw path.
- `docs/SHADOW_UI.md` — **Shadow UI internals.** Input dispatch order, the
  synthesised contracts, the component load gate, User Presets and the trailing
  pages, Master FX persistence, what audio FX are fed.
- `docs/CHAIN.md` — **The chain contract.** `ui_hierarchy`, `chain_params`, the
  chain host's file layout, and the shape-edit permutation verbs.
- `docs/DIAGNOSTICS.md` — **Measuring the device.** On-device E2E, OTLP tracing,
  the param tally, the SPI frame tally. Every switch is off by default.

- `docs/API.md` — JS API reference (display, MIDI, host fns, LED colors)
- `docs/MODULES.md` — Module development guide (module.json, capabilities, tool_config, DSP API, Signal Chain integration, Remote UI `web_ui.html` + `schwungRemote` postMessage). Its **widget reference** — every widget's picture beside the rule that selects it, plus chrome and motion — is GENERATED between markers by `node tools/param-pages/widget_sheet.mjs --manual` and pinned by `tests/host/test_widget_sheet.sh` (which also fails on an ORPHANED image). There is no separate WIDGETS.md: a second user-facing widget page in the same voice as the manual's was one document too many, and the pictures belong next to the rules. `--manual` additionally writes a 14-image subset into `../schwung-catalog-site/manual.html`, sized from each image's own natural width — `width: 100%` rendered a one-cell switch four times the size of a cell. Not the SCH-50 catalog (`tools/param-pages/catalog.mjs`), which renders ten *alternatives* per widget and is gitignored.
- `docs/LOGGING.md` — Unified logging
- `docs/SPI_PROTOCOL.md` — Full SPI reference
- `docs/REALTIME_SAFETY.md` — RT rules and JACK glitch root causes
- `docs/SYSEX.md` — **SysEx, both directions**, and they fail for unrelated reasons. Test rig is a Mac on USB-C (Standalone Port = cable 2, no external gear). **A chain slot is WRITE-ONLY for SysEx** — an editor built as one waits forever. **The inbound ceiling is the sender's BURST RATE, not the message size**: 400/512/632 B all truncate at 381 B, yet two 316 B messages 100 ms apart both arrive whole.
- `docs/MIDI_INJECTION.md` — Cable-2 injection / echo filter history
- `docs/ADDRESSING_MOVE_SYNTHS.md` — Sending MIDI to Move tracks/slot synths from tools, overtake modules, chain MIDI FX. Ref: `src/modules/tools/seq-test/`.
- `../schwung-catalog-site/manual.html` — User-facing manual (canonical, lives in the catalog-site repo)
- `BUILDING.md` — Build system, cross-compilation

## Release Checklist

1. **Build**: `./scripts/build.sh` succeeds
2. **Deploy + test**: `./scripts/install.sh local --skip-modules --skip-confirmation`, verify on hardware
3. **Version**: bump `src/host/version.txt` and `module-catalog.json` (host `latest_version` + download URL)
4. **Docs**: update the subsystem file (`docs/PARAM_PAGES.md`, `docs/SHADOW_UI.md`,
   `docs/CHAIN.md`, `docs/DIAGNOSTICS.md`) and add a bullet to `CLAUDE.md`'s hook
   for it — **not** the prose itself. Then `docs/API.md`, `docs/MODULES.md`, `src/shared/help_content.json`, and `../schwung-catalog-site/manual.html` for new features / changed behavior. If a knob-grid widget changed, regenerate the sheet with `node tools/param-pages/widget_sheet.mjs --manual` — `tests/host/test_widget_sheet.sh` fails until the `docs/` half is current, and `--manual` also rewrites the manual's generated widget section and its images (skipped silently when the sibling repo is not checked out, so it is safe on any machine).
5. **Help files**: update `help.json` in modified tool modules
6. **Module catalog**: bump `min_host_version` for modules depending on new host features
7. **Commit + tag**: `git tag v0.X.0 && git push --tags`
8. **Release notes**: `gh release edit` with concise bullets

## Dependencies

QuickJS (`libs/quickjs/`), stb_image.h (`src/lib/`), curl (`libs/curl/`, download backend for catalog detection + manual refresh).
