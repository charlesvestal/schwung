# schwung-probe

Loads a Schwung module **off the device** — no SPI, no QuickJS, no hardware —
and answers the two questions the preview pipeline needs:

    probe --contract <module-dir>              what does it publish?
    probe --render   <module-dir> --score S    what does it sound like?

Built for ARM64 Linux by the repo's existing `Dockerfile` (`debian:bookworm`,
arm64), which is what makes it able to load modules at all: same glibc, same
`libdbus`/`libsystemd`, same everything the release tarballs were built
against. On an Apple-silicon Mac that container runs natively.

## Why this exists

`tools/param-pages/` can already draw any module's knob grid — it just needed
a contract to draw *from*, and the only source was a hand-run device dump
(`tests/fixtures/module-contracts.json`) that goes stale silently. The probe
reads what is actually shipping: it caught obxd at v0.4.9 while the committed
fixture still described 0.4.7.

## THREE plugin contracts, not one

Dispatch is on **which entry symbol resolves**, never on the file name or the
catalog's `component_type`:

| | synth | audio FX | MIDI FX |
|---|---|---|---|
| entry symbol | `move_plugin_init_v2` | `move_audio_fx_init_v2` | (as chain_host resolves) |
| render | `render_block` | `process_block` | — |
| `.so` | `dsp.so` | `<id>.so` | varies |

Getting this wrong fails **quietly** — the wrong symbol gives a clean load and
no sound — so an object exporting no known entry symbol is an ERROR here, never
an empty result.

## Things that are true and surprising

- **`chain_params` is unserved for an audio FX.** Its metadata is declared in
  `module.json` (`capabilities.chain_params` / `capabilities.ui_hierarchy`) and
  read from the file by the chain host. Measured: cloudseed returns -1 for the
  key and 251 bytes for `ui_hierarchy`.
- **A read has three answers** — JSON, `""` (served, empty), and unserved. The
  output keeps them apart. Collapsing the last two would make 40 audio FX look
  like they publish an empty contract.
- **The host API is passed as all-zero.** Every module guards `if (host->fn)`,
  so this is safe, and it is what lets the probe run with no host at all.
- **This is the plugin in isolation**, not under the chain host: no bus, no
  send, no master FX, no LFO overlay. Output records `harness: "probe-isolated"`
  so a later full-host harness can publish the same artifacts at higher
  fidelity without any consumer changing.
- **It is not realtime and does not pretend to be.** It calls entry points from
  an ordinary thread as fast as the CPU allows, so it cannot see the realtime
  violations a 2026-08 audit found ~150 of. Nothing here is an RT check.
