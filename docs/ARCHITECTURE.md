# Architecture: How Schwung Works

This document explains how Schwung loads custom code onto Ableton Move
hardware. Read it together with `CLAUDE.md` (working notes) and the
specific subsystem docs (`SPI_PROTOCOL.md`, `REALTIME_SAFETY.md`,
`LINK_AUDIO_WIRE_FORMAT.md`, `GAIN_STAGING_ANALYSIS.md`,
`MODULES.md`, `API.md`).

## Big picture (current shipping state)

```
┌─────────────────────────────────────────────────────────────┐
│ install.sh                                                  │
│   Deploys files to /data/UserData/schwung/.                 │
│   Configures Move's launcher to LD_PRELOAD schwung-shim.so  │
│   into MoveOriginal.                                        │
└─────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────┐
│ Stock Move firmware (MoveOriginal)                          │
│   The Ableton-supplied binary keeps running normally.       │
│   It owns the SPI mailbox, audio engine, and stock UI.      │
└─────────────────────────────────────────────────────────────┘
                              ↕ ioctl/mmap on /dev/ablspi0.0
┌─────────────────────────────────────────────────────────────┐
│ schwung-shim.so (LD_PRELOAD, in-process)                    │
│   Hooks ioctl, mmap, sendto, sd_bus, send.                  │
│   Filters MIDI, mixes shadow audio, draws overlays,         │
│   forks shadow_ui as a sibling process,                     │
│   runs slot DSP (chain_host) and master FX in-process,      │
│   intercepts screen-reader D-Bus signals for TTS.           │
└─────────────────────────────────────────────────────────────┘
                              ↕ POSIX shared memory
┌─────────────────────────────────────────────────────────────┐
│ shadow_ui (separate QuickJS process)                        │
│   Runs src/shadow/shadow_ui.js on QuickJS.                  │
│   Renders the Shadow UI overlay, drives slot/Master FX      │
│   menus, hosts overtake modules, handles the Tools menu     │
│   and the Quantized Sampler / Skipback overlays.            │
└─────────────────────────────────────────────────────────────┘
                              ↕ /schwung-* SHM, /schwung-link-in
┌─────────────────────────────────────────────────────────────┐
│ link-subscriber (sidecar)                                   │
│   Standalone C++ binary using Ableton's public abl_link     │
│   audio C API to receive Move's per-track audio and to      │
│   publish shadow output back as ME-1..ME-4 channels.        │
└─────────────────────────────────────────────────────────────┘
```

`schwung_host.c` (the standalone `schwung` binary) is built and
shipped, but is currently **not invoked on device** — every JS module
runs inside the shadow_ui process. Bindings registered in
`schwung_host.c` are listed in `docs/API.md` for completeness but are
not reachable from a running module today.

## Layer 1 — Installation Bootstrap

`scripts/install.sh` connects to Move via SSH at `move.local` and:

1. Deploys files to `/data/UserData/schwung/`:
   - `schwung-shim.so` (LD_PRELOAD library, with setuid bit so secure
     exec mode can still preload it)
   - `shim-entrypoint.sh`
   - `shared/`, `host/`, `modules/`, `scripts/` directories
   - `link-subscriber` sidecar binary
2. Writes `config/features.json` (preserving prior settings if any).
3. Configures Move's launcher to run the shim entrypoint, which sets
   `LD_PRELOAD=schwung-shim.so` before exec'ing MoveOriginal.

After installation, every reboot loads the shim alongside Move's
normal firmware. There is no separate "host takeover" — Schwung is
always sidecar, always shadow.

## Layer 2 — The shim (`src/schwung_shim.c`)

The shim is the heart of Schwung. It runs inside the MoveOriginal
process via LD_PRELOAD and intercepts a small set of system calls:

| Hook | Purpose |
|------|---------|
| `mmap` | Capture the SPI mailbox base pointer |
| `ioctl` (SPI) | Run pre/post processing around each SPI exchange — MIDI filtering, audio mix, display overlay, LED queue flush |
| `sendto` | (Historical, removed.) Used to intercept Move's chnnlsv Link Audio packets. Reception is now via `link-subscriber`. |
| `sd_bus_*` / `send` | Capture Move's D-Bus connections for screen-reader announcements |

The shim owns these subsystems:

- **MIDI routing** — filter cable-0 hardware controls into the shadow
  buffer, forward cable-2 to the chain, inject shadow-generated MIDI
  back into Move's MIDI_IN.
- **Long-press / Shift+Vol shortcuts** — gated by the
  `shadow_ui_trigger` setting (Long Press / Shift+Vol / Both); see
  `MANUAL.md` and the dispatch in `schwung_shim.c`.
- **Slot DSP and Master FX** — `chain_host` is dlopen'd in-process;
  per-slot synth/FX state lives here.
- **Master volume + ME bus mix** — ME-only-bus refactor (2026-04):
  Move's mailbox stays at `mv` level untouched; shadow output is built
  on a separate ME bus at unity, optionally run through Master FX,
  then summed: `mailbox = mv + me_post_fx × mv`. See
  `docs/GAIN_STAGING_ANALYSIS.md`.
- **Capture path (`unity_view`)** — skipback, quantized sampler, and
  the native resample bridge read a reconstructed buffer at unity, so
  captures are independent of master volume.
- **Overlay drawing** — the shim composites volume bars, the sampler
  overlay, and the shadow UI display chunk into Move's display before
  each ioctl returns to MoveOriginal.
- **Screen reader / TTS** — D-Bus filter captures Move's
  `com.ableton.move.ScreenReader.text` signals, debounces them, and
  hands off to the TTS engine (`tts_engine_dispatch.c`,
  espeak/Flite). See `docs/tts-architecture.md`.
- **Realtime safety** — every code path on the SPI callback is
  non-allocating and non-blocking; logging uses a snapshot drained by
  a background thread. See `docs/REALTIME_SAFETY.md`.

## Layer 3 — The shadow_ui process

When the user presses a configured shortcut, the shim forks a child
that resets to `SCHED_OTHER` (so it doesn't inherit MoveOriginal's
FIFO-70 priority) and execs the shadow_ui binary. shadow_ui is a
QuickJS host that loads `src/shadow/shadow_ui.js`.

Responsibilities:

- Render the Shadow UI to a dedicated display SHM that the shim
  composites onto Move's display.
- Drive slot and Master FX menus, the Tools menu, Global Settings,
  the Module Store.
- Host overtake modules (modules with `component_type: "overtake"`):
  the host clears LEDs progressively, waits ~500ms, then calls the
  module's `init()`. Shift+Vol+Jog-Click is handled at host level for
  reliable escape.
- Issue parameter requests to slot DSP via `/schwung-param`.
- Send/receive UI MIDI via `/schwung-ui-midi`, `/schwung-midi-out`,
  `/schwung-midi-dsp`, `/schwung-midi-inject`.
- Drive Quantized Sampler / Skipback overlays via `/schwung-overlay`.

The full set of JS bindings is enumerated in `docs/API.md`.

## Layer 4 — link-subscriber sidecar

`src/host/link_subscriber.cpp` is a small C++17 binary linked against
`libs/link/`. It uses Ableton's public `abl_link` audio C API to:

- Subscribe to Move's per-track Link Audio channels
  (`1-MIDI`/`2-MIDI`/`3-MIDI`/`4-Audio`/`Main`) and write raw int16
  audio into the `/schwung-link-in` SHM ring (5 slots, SPSC).
- Publish shadow slot output back to Live as `ME-1..ME-4` channels via
  `LinkAudioSink`s.

The shim reads `/schwung-link-in` from the SPI callback (no
allocation, no locks) and routes each Move track through the matching
shadow slot's audio FX chain when `Move->Schwung` routing is enabled.
See `docs/LINK_AUDIO_WIRE_FORMAT.md` for the wire-format reference and
`docs/plans/2026-04-17-link-audio-official-api-migration.md` for the
migration plan.

## The SPI mailbox

All hardware traffic flows through a 4 KB region mmap'd from
`/dev/ablspi0.0`. The shim mirrors this region into a "shadow"
buffer (also 4 KB) where it composes the version Move actually sees.

```
Output (TX) — offset 0:
  0       80     MIDI OUT: 20 × 4-byte USB-MIDI packets
  80       4     Display status word
  84     172     Display data chunk
  256    512     Audio OUT: 128 stereo int16 frames

Input (RX) — offset 2048:
  2048   248     MIDI IN: 31 × 8-byte AblSpiMidiEvent (4 USB-MIDI + 4 timestamp)
  2296     4     Display status word
  2304   512     Audio IN: 128 stereo int16 frames
```

Authoritative offsets live in `src/lib/schwung_spi_lib.h`. See
`docs/SPI_PROTOCOL.md` for full event formats and ioctl details.

### MIDI cables

| Cable | Direction | Purpose |
|-------|-----------|---------|
| 0 | IN/OUT | Internal Move hardware controls (pads, knobs, buttons, LEDs) |
| 2 | IN/OUT | External USB-A MIDI (devices on Move's USB host port). Also used by the shim to inject chain-generated MIDI back into Move's input stream. |
| 14 | OUT | System-level events |
| 15 | OUT | SPI protocol-bound events |

In overtake mode the shim forwards external (cable-2) MIDI to the
overtake module's `onMidiMessageExternal`. In normal shadow mode the
shim filters cable 2 unless a slot's MIDI router is configured to
listen on it.

### Display

128×64 1-bit. Each frame is split into 6 chunks of 172 bytes; the
device drives chunk index via the RX status word and the shim/host
echoes the corresponding chunk back. The shim composites the shadow
UI's display SHM, sampler/skipback overlays, and any toast banners
onto Move's chunk before the ioctl returns.

### Audio

44 100 Hz stereo, 128 frames per block, interleaved int16, little
endian. The shim's mix path is described above; the realtime budget
is documented in `docs/REALTIME_SAFETY.md`.

## Shadow chain slots

Each of the 4 shadow slots holds:

- A loaded patch (synth + audio FX + MIDI FX + MIDI router config).
- Per-slot volume, mute, solo, and capture rules.
- Per-set state (saved/restored as the user switches Move sets).

`src/modules/chain/dsp/chain_host.c` is dlopen'd into the shim and
hosts the full chain DSP graph. Slot focus, parameter edits, and
preset loads are driven by shadow_ui via the `/schwung-param` SHM.

For details on chain modules, capabilities, and `ui_hierarchy`, see
`docs/MODULES.md`.

## Shared memory segment names

| Region | Purpose |
|--------|---------|
| `/schwung-control` | Control flags, slot selection, request IDs (`shadow_control_t`, exactly 64 B) |
| `/schwung-audio` | Shadow's mixed audio output |
| `/schwung-midi` | MIDI to shadow DSP |
| `/schwung-ui-midi` | MIDI to shadow UI |
| `/schwung-display` | Shadow display buffer |
| `/schwung-display-live` | Live display mirror for the web viewer |
| `/schwung-movein` | Move's audio for shadow processing |
| `/schwung-ui` | Slot state (names, channels, active flags) |
| `/schwung-param` | Parameter read/write requests |
| `/schwung-midi-out` | MIDI output from shadow UI |
| `/schwung-midi-dsp` | MIDI from shadow UI to DSP slots |
| `/schwung-midi-inject` | MIDI inject into Move's MIDI_IN |
| `/schwung-screenreader` | Screen reader announcements |
| `/schwung-overlay` | Sampler/skipback overlay state |
| `/schwung-link-in` | Per-channel audio from `link-subscriber` |
| `/schwung-pub-audio` | Shadow audio for `link-subscriber` to publish |
| `/schwung-web-param-set` | Web UI → shim param-set ring |
| `/schwung-web-param-notify` | Shim → web UI param-change ring |

Authoritative names and structures live in
`src/host/shadow_constants.h`.

## Shadow Mode access

User-level details (shortcuts, Master FX, tools, recording, etc.)
live in `MANUAL.md`. The trigger gestures themselves are gated by the
`shadow_ui_trigger` setting (Long Press / Shift+Vol / Both) and by the
volume-tweak suppression rule in the shim (touching the volume knob
during a track hold suppresses that track's long-press for the rest of
the press).

## Module loading

Modules are extracted to category subdirectories under
`/data/UserData/schwung/modules/`:

- `chain/`, `controller/`, `store/`, `file-browser/`, `song-mode/`,
  `wav-player/` — built-in
- `sound_generators/<id>/`, `audio_fx/<id>/`, `midi_fx/<id>/`,
  `tools/<id>/`, `overtake/<id>/`, `utilities/<id>/` — installed via
  Module Store

Each module ships a `module.json`, optional `ui.js` / `ui_chain.js`,
optional `dsp.so`. Native plugins are dlopen'd by `chain_host` (for
chainable plugins) or by the shadow UI overtake loader (for
overtakes). The plugin C API is `host_api_v1_t` + `plugin_api_v2_t`
(v2 supports multiple instances; v1 is the deprecated singleton API).
See `docs/MODULES.md` for the full module developer guide.

## Returning to stock Move

There is no full takeover today, so there is also no full "exit". Use
the Tools menu's "Return to Move" action (or the standard back-out
gesture) to dismiss the Shadow UI; MoveOriginal continues running
without interruption.

`scripts/uninstall.sh` removes Schwung entirely, restoring the stock
launcher.

## Security and trust

- The shim runs with the setuid bit set so LD_PRELOAD survives
  MoveOriginal's secure-exec mode.
- Modules are native ARM code with full system access; only install
  modules from sources you trust. Module Store fetches are
  authenticated through GitHub releases declared in
  `module-catalog.json` plus each module's own `release.json`.
- The web Schwung Manager (`move.local:7700`) is unauthenticated by
  default; treat it as a trusted-network tool.
