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
(~95 shell tests: source-invariant pins, compiled C units, node-run .mjs units —
not wired into CI; ~20 stale failures pin since-moved code, see the cleanup
review doc). On-hardware behavior is verified manually. Enable the unified logger:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
```

JS: `console.log()` (auto-routed) or import `shared/logger.mjs`. C: `LOG_DEBUG("source", "msg")` from `host/unified_log.h`. See `docs/LOGGING.md`.

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

## Realtime Safety

SPI callback runs SCHED_FIFO 90 on core 3. Budget ~900µs/frame after the ~2ms transfer.

**Never in the SPI callback path:** `unified_log()`, `fprintf()`, `fopen()`, any file I/O; allocation; locks held by non-RT threads.

**FIFO inheritance:** Shim runs in MoveOriginal's FIFO 70 threads. Any child process (`shadow_ui`, `host_system_cmd`) must reset to SCHED_OTHER before exec — handled by `shadow_process.c` and `shadow_ui.c`, don't bypass.

**CPU pinning:** Keep core 3 free for SPI. Pin compute-heavy procs (RNBO) to cores 0–2 (`taskset 0x7`). See `docs/REALTIME_SAFETY.md`.

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

1. **Link Audio nudge** (`link_audio_read_channel_shm`): drops/dups one stereo frame every 16 reads outside a ±32-sample dead band around 800 stereo samples (~9.07 ms). Burst mode (8 frames/period) when error > 512. Effective rate change <0.3%.
2. **Schwung-side delay buffer** (`shadow_latency_delay_apply`): per-slot 2048-sample ring delays `shadow_slot_deferred[s]` by 800 samples before combining with `move_track`. Bypassed when `rebuild_from_la = 0`.

Toggle mid-playback → ~9 ms artifact (audio hole on OFF→ON, dup on ON→OFF) as ring resets.

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

Chain host (`modules/chain/dsp/chain_host.c` — lifecycle/set+get_param/render; helpers split into `chain_{json,params,mod,midi,patch}.c`, shared decls in `chain_internal.h`) dlopens sub-plugins, forwards MIDI to sound generator, routes audio through FX. Patches in `/data/UserData/schwung/patches/*.json`. Built-in MIDI FX: chord, arp (up/down/up_down/random). Built-in audio FX: freeverb. MIDI sources can provide `ui_chain.js` for fullscreen chain UI.

### Recording / capture

Audio capture is shim-side: the Quantized Sampler (Shift+Sample) and Skipback
(Shift+Capture) — see Shadow Mode below. (The old chain-host CC 118 recording
was deleted in the 2026-06 cleanup; it was only reachable through the
unreachable v1 plugin path.)

## Shadow Mode

Shim intercepts hardware I/O to mix shadow audio with Move's output.

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

Plain Mute is blocked from reaching Move firmware while shadow UI is shown. Bypass persists via per-slot autosave (`slot_N.json`, `master_fx_N.json`); patch-library reloads start with bypass=0.

### Quantized Sampler

Shift+Sample. Source: resample (incl. Schwung synths) or Move Input. Duration in bars (or until stopped); uses MIDI clock, falls back to project tempo. Starts on note event or play. Saved to `Samples/Schwung/Resampler/YYYY-MM-DD/`.

### Feedback Protection

Loading a chain module / tool that consumes line-in shows "Speaker Feedback Risk" warning if speakers active AND no line-in cable. Jog-click proceeds, Back aborts.

Gate fires when: module's `capabilities.audio_in == true` AND `component_type` is NOT `audio_fx`/`midi_fx` AND `shadow_speaker_active` AND NOT `shadow_line_in_connected`.

Impl: `src/shared/feedback_gate.mjs` (predicate + modal), `src/shadow/shadow_ui.js` (call sites), `src/schwung_shim.c` (XMOS CC 114 line-in, CC 115 line-out). Out of scope: Move firmware's autosample / line-in monitoring; Quantized Sampler "Move Input" toggle (fullscreen menu makes JS modal inert). See `docs/plans/2026-04-30-feedback-protection-design.md`.

### Skipback

Shift+Capture saves last 30 s. Same source as sampler. Output: `Samples/Schwung/Skipback/YYYY-MM-DD/`.

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

### MIDI Cable Filtering

MIDI_IN (offset 2048): cable 0 = Move hw controls, cable 2 = external USB MIDI.
MIDI_OUT (offset 0): cable 0 = Move internal out, cable 2 = external USB out.

Normal shadow: only cable 0 processed. Overtake: all cables forwarded; cable 2 → `onMidiMessageExternal`. If Move tracks listen+output on same channel as external device, MIDI echoes back — use different channels.

### Cable-2 Channel Remap (Overtake)

Overtake modules can rewrite cable-2 MIDI_IN channel before Move firmware sees it (solves live-external ↔ Move-native routing without the JS-reinject cascade in `docs/MIDI_INJECTION.md`).

JS API: `host_ext_midi_remap_set(in_ch, out_ch)` (0–15; out_ch >= 16 or < 0 = passthrough), `host_ext_midi_remap_clear()`, `host_ext_midi_remap_enable(on)`.

Shim reads the table every SPI frame (post-transfer), rewrites channel byte in-place in both hw mailbox and shadow buffer. System messages (`0xF*`) skipped. **Bypassed globally** if any chain slot is forward=THRU (MPE preservation). **Force-reset** to all-passthrough + disabled on overtake exit. Gated by `ext_midi_remap_enabled` in `features.json` (default `true`).

SHM: `/schwung-ext-midi-remap`, 64 bytes, `schwung_ext_midi_remap_t` in `src/host/shadow_constants.h`. v1.

### Master FX Chain

4-slot Master FX processes mixed shadow output. Access: Shift+Vol+Menu.

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
