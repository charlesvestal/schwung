# CLAP Host Module Design

**Goal:** Add a Move Anything module that hosts arbitrary CLAP plugins in-process and can be used standalone or as a chain component.

## Summary

We will create a new external module repo (e.g. `move-anything-clap`) that ships a `clap` module. The module exposes a QuickJS UI for plugin selection and parameter control, and a `dsp.so` implementing `plugin_api_v1` that embeds a CLAP host. Plugins are discovered from `/data/UserData/move-anything/modules/clap/plugins/` and loaded in-process. The module is chainable and can be used as a sound generator, audio FX, or MIDI FX based on the loaded plugin's port configuration.

## Architecture

- **Module package**: `module.json`, `ui.js`, `dsp.so` in `/data/UserData/move-anything/modules/clap/`.
- **CLAP discovery**: Scan `modules/clap/plugins/` for `.clap` bundles at `on_load` and build a list of plugin metadata (name, vendor, ID, audio/MIDI port summary).
- **Selection**: UI sets `selected_plugin` via `set_param`; module unloads current plugin instance and loads the new one.
- **Signal Chain integration**:
  - The module declares `chainable: true` and reports a runtime mode based on plugin ports.
  - Chain patches reference the module plus the chosen plugin ID.
- **Audio/MIDI**:
  - `on_midi` converts to CLAP events and forwards to the plugin.
  - `render_block` calls CLAP process on 128-frame blocks at 44.1 kHz and converts float output to int16 interleaved.
  - If plugin has audio input, Move input mailboxes are converted to float and passed in.
  - If plugin emits MIDI, forward to host via `host_api_v1.midi_send_internal/external`.

## Parameters and State

- Enumerate CLAP parameters on load and expose as stringly-typed `set_param`/`get_param` keys (e.g. `param_<index>` or `param_<id>`).
- UI paginates parameter banks for encoder control.
- Persist selection and parameter values in module defaults or a small state file inside the module directory.

## Performance and Safety

- In-process host for MVP due to CM4 constraints.
- Single-threaded processing; no worker threads in v1.
- Simple error handling: log failures and fall back to a safe "no plugin" state.

## Testing

- Add unit tests for CLAP discovery and port classification (host-side library functions).
- Add a smoke test that loads a minimal CLAP stub plugin and processes a block.

## Risks

- Plugin compatibility and parameter mapping edge cases.
- CPU cost for heavier CLAP plugins; may need a whitelist or warning UI.

## Rollout

- Ship as a separate module repo alongside `move-anything-obxd`.
- Optional host changes only if chain metadata or UI hooks require it.
