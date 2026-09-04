# Module Development Guide

Modules are self-contained packages that extend Schwung with new functionality.

> **Contributing back to Schwung?** Read the
> [contribution provenance](../CONTRIBUTING.md#contribution-provenance) policy
> first: please disclose which AI coding tools you used, and note that
> contributions developed with Grok aren't accepted.

## Module Structure

```
src/modules/your-module/
  module.json              # Required: module metadata
  ui.js                    # Optional: JavaScript UI
  ui_chain.js              # Optional: Signal Chain UI shim
  dsp.so                   # Optional: native DSP plugin
  dsp/                     # Optional: DSP source code
    your_plugin.c
  settings-schema.json     # Optional: per-module settings (web UI)
  config.json              # Mutable: written by schwung-manager (do not ship)
  secrets/                 # Mutable: 0600 secret files (do not ship)
    <key>.txt
```

## module.json

```json
{
    "id": "your-module",
    "name": "Your Module Name",
    "version": "1.0.0",
    "abbrev": "MOD",
    "description": "What your module does",
    "author": "Your Name",
    "ui": "ui.js",
    "ui_chain": "ui_chain.js",
    "dsp": "dsp.so",
    "api_version": 2
}
```

Required fields: `id`, `name`, `version`, `api_version`
Optional fields: `description`, `author`, `ui`, `ui_chain`, `dsp`, `defaults`, `capabilities`, `abbrev`

**Notes:**
- `api_version`: Use `2` for new modules (supports multiple instances, required for Signal Chain)
- `abbrev`: Short display name (3-6 chars) for Shadow UI slot display (e.g., "SF2", "Dexed", "CLAP")
- `module.json` is parsed by a minimal JSON reader. Use double quotes for keys, lowercase `true`/`false`, and avoid comments.
- Keep `module.json` reasonably small (the loader caps it at 8KB).
- `dsp`: any filename inside the module directory. The host loads whatever path you specify here, so `dsp.so`, `<module-id>.so`, or anything else is fine for standalone modules. **Exception — `audio_fx` modules used inside Signal Chain:** the chain host loads the FX directly as `modules/audio_fx/<id>/<id>.so` (it does not consult the FX's `module.json`), so audio FX shared libraries **must** be named `<module-id>.so`. Sound generators and MIDI sources loaded by the chain are hardcoded to `dsp.so`.

### Capabilities

Add capability flags to enable special module behaviors. You can group them under
`capabilities` (recommended) or place them at the top level (the host searches
for keys anywhere in `module.json`).

```json
{
    "id": "your-module",
    "name": "Your Module",
    "version": "1.0.0",
    "api_version": 1,
    "capabilities": {
        "audio_out": true,
        "midi_in": true,
        "claims_master_knob": true
    }
}
```

| Capability | Description |
|------------|-------------|
| `audio_out` | Module produces audio |
| `audio_in` | Module uses audio input |
| `midi_in` | Module processes MIDI input |
| `midi_out` | Module sends MIDI output (chain MIDI FX, generator tools) |
| `aftertouch` | Module uses aftertouch |
| `claims_master_knob` | Module handles volume knob (CC 79) instead of host |
| `raw_midi` | Skip host MIDI transforms (velocity curve, aftertouch filter); module may also bypass internal MIDI filters when set |
| `raw_ui` | Module owns UI input handling; host won't intercept Back to return to menu (use `host_return_to_menu()` to exit) |
| `chainable` | Marks a module as usable inside Signal Chain patches (metadata) |
| `requires_continuous_processing` | Audio FX that owns non-trivial internal time (rolling loopers, granular, modulated delays/reverb). Keeps the slot's DSP rendering through silence instead of idle-parking it after the silence threshold, so the plugin's internal clock doesn't freeze and resume at a wrong offset. Read by `chain_host.c`; null-checked so older chain DSPs still load. |
| `skip_led_clear` | Host skips clearing LEDs on module load/unload — preserves Move's native pad colors (useful for modules that overlay highlights on existing clip colors) |
| `default_forward_channel` | Default Forward Channel for shadow slots loading this module. `-2` = passthrough (preserve original MIDI channel, required for MPE), `1`–`16` = remap to a specific channel. |
| `button_passthrough` | Array of CC numbers the module wants Move to keep handling (e.g. `[85]` to let Play reach Move while the module is active). |
| `suspend_keeps_js` | Tool/overtake modules: pressing Back suspends the UI but the DSP keeps ticking; full exit requires Shift+Back. Useful for sequencers that should keep playing while you browse Move. |
| `component_type` | Module category: `sound_generator`, `audio_fx`, `midi_fx`, `utility`, `system`, `featured`, `overtake`, or `tool` |
| `forks_processes` | Module creates child **processes** (not threads) to do its DSP, as JE-8086 does. See below. |

> **Where these are read.** `src/host/module_manager.c` (used by the
> standalone host runtime) currently parses only `claims_master_knob`,
> `raw_midi`, and `raw_ui`. The remaining flags are honored in the
> shim and shadow UI code paths that actually run on device — search
> for the flag name in `src/schwung_shim.c`, `src/shadow/shadow_ui.{c,js}`,
> and `src/modules/chain/dsp/chain_host.c` to find the consumer.

### `forks_processes`

Set `capabilities.forks_processes: true` when your module's DSP runs in
child **processes** it forks (`fork()`), not threads. It is metadata
only — nothing about how your module runs changes because of this flag.

It exists for the CPU page (`/system/cpu` in schwung-manager, see
`docs/DIAGNOSTICS.md`), which cannot work out ownership from the forked
processes' names: a fork inherits its parent's `comm`, so your children
report as `MoveOriginal` — or worse, as `Audio Main/SPI`, the same name
six of Move's own realtime threads use. The flag tells the page these
processes are yours to attribute.

**Omitting it does not hide your module's cost.** The forked processes
still show up on the page with their real CPU time, just as
"Unattributed" instead of under your module's name.

### Tool Config

Tool modules (`"component_type": "tool"`) appear in the Tools menu and support additional options via `tool_config`:

```json
{
    "tool_config": {
        "interactive": true,
        "skip_file_browser": true
    }
}
```

| Field | Description |
|-------|-------------|
| `interactive` | Tool takes over the UI (like an overtake module) rather than running headlessly |
| `skip_file_browser` | Tool does not use the file browser on launch (goes straight to its own UI) |
| `input_extensions` | Array of file extensions the tool accepts (e.g., `[".wav"]`) |
| `allow_new_file` | Show a "+ New File" action in the file browser |
| `command` | Shell command to run for non-interactive tools |
| `overtake` | `true` to use overtake display mode (full LED clear, ~500ms init delay, Shift+Vol+Jog-Click exit). Default is `false`. |
| `stems` | Array of stem names a separation tool produces, used for the progress readout |
| `processing_ratio` | Wall time as a fraction of the input's duration, used only for the "about N remaining" estimate. `0.5` means a 4-minute file takes about 2 minutes. Default `0.5`. |

Interactive tools use `host_exit_module()` to return to the tools menu when the user presses Back.

**`processing_ratio` is a measurement, not an aspiration, and it should err
LONG.** An estimate that runs out while the tool is still working reads as a
hang; one that finishes early reads as a pleasant surprise.

A tool that ships several engines of different speeds puts a `processing_ratio`
on each entry of `tool_config.engines[]` instead; the per-engine value wins over
the module-level one.

The module-level field went unread for the life of the field — `getToolProcessingRatio()`
consulted only the per-engine value and otherwise returned a hardcoded `0.5`.
It stayed invisible because the single module declaring it declared `0.5`, the
same number as the default, so a working declaration and an ignored one were
indistinguishable. It surfaced only when that module corrected itself to a
measured figure and the on-screen estimate did not move.
`tests/host/test_tool_processing_ratio.sh` now runs both copies of the function
— `shadow_ui.js` drives the confirm screen, `shadow_ui_tools.mjs` the processing
screen — against the same cases and fails if they disagree.

### Defaults

Use `defaults` to pass initial parameters to DSP plugins at load time:

```json
{
    "defaults": {
        "preset": 0,
        "output_level": 50
    }
}
```

## Per-Module Settings

A module that wants user-configurable settings exposed in the
Schwung Manager web UI ships a `settings-schema.json` next to its
`module.json`. schwung-manager auto-discovers it and renders a
Settings section inline on the module's detail page
(`http://move.local:7700/modules/<id>`). Saved values land in
`<module_dir>/config.json`; password-typed fields land in
`<module_dir>/secrets/<key>.txt`. The module reads its own values
at runtime via `host_read_file`.

Available since host **0.9.8** — set `min_host_version` accordingly
in your `module.json` and catalog entry if your module depends on
this system.

### Schema fragment

```json
{
    "id": "your-module",
    "label": "Your Module",
    "items": [
        { "key": "enabled", "label": "Enabled", "type": "bool", "default": true },
        { "key": "mode", "label": "Mode", "type": "enum",
          "options": ["Off", "Low", "High"], "values": ["off", "low", "high"], "default": "off" },
        { "key": "ratio", "label": "Ratio", "type": "float", "min": 0, "max": 1, "step": 0.01, "default": 0.5 },
        { "key": "label", "label": "Label", "type": "string", "default": "" },
        { "key": "system_prompt", "label": "System Prompt", "type": "textarea",
          "rows": 12, "default_source": "default_system_prompt.txt" },
        { "key": "api_key", "label": "API Key", "type": "password",
          "help": "Get a free key at", "help_url": "https://example.com/" }
    ]
}
```

**Required:** the schema's `id` MUST equal the parent directory
name (matches the module's catalog id). Mismatched fragments are
silently dropped at discovery time — this prevents a tampered
schema from impersonating a neighbor module.

**Two layouts are accepted.** The flat shorthand above (single
implicit section), or explicit sections:

```json
{
    "id": "your-module",
    "sections": [
        { "id": "general",  "label": "General",  "items": [...] },
        { "id": "advanced", "label": "Advanced", "items": [...] }
    ]
}
```

### Field types

| Type | Storage | Notes |
|------|---------|-------|
| `bool` | `config.json` | Renders as a toggle. |
| `enum` | `config.json` | Pair `options[]` (display labels) with `values[]` (saved values). |
| `int` | `config.json` | Supports `min`, `max`, `step`. |
| `float` | `config.json` | Supports `min`, `max`, `step`. |
| `string` | `config.json` | Single-line text input. |
| `textarea` | `config.json` | Multi-line. Set `rows` for height. Use `default_source` to load a default from a file shipped with the module. |
| `password` | `<module>/secrets/<key>.txt` (0600) | Never round-tripped through the web UI. Field shows `••••••••` when set, with an `(×)` button to clear. Optional `help` + `help_url` render under the input. |

Common item fields: `key` (snake_case, required), `label`
(required), `type` (required), `default` (any JSON value matching
the type), `default_source` (textarea only, path relative to the
module dir, must stay inside it).

### Reading values at runtime

The module owns its own defaults — schwung-manager only writes
saved values, never schema defaults. Read your config from
`<module_dir>/config.json`:

```javascript
const MODULE_DIR = "/data/UserData/schwung/modules/tools/your-module";

const cfg = JSON.parse(host_read_file(MODULE_DIR + "/config.json") || "{}");
const enabled = cfg.enabled === undefined ? true : !!cfg.enabled;  // schema default: true
const mode    = cfg.mode || "off";
const ratio   = cfg.ratio === undefined ? 0.5 : cfg.ratio;

const apiKey = (host_read_file(MODULE_DIR + "/secrets/api_key.txt") || "").trim();
```

For a more robust pattern that reads `default` and `default_source`
straight out of the schema and merges them under saved values, see
`src/modules/tools/config-test/ui.js`.

To pick up changes the user makes in the web UI without a reload,
re-read `config.json` periodically from `tick()` (e.g. once a
second).

### Upgrade and uninstall behavior

- `settings-schema.json` is **overwritten** by the new tarball on
  every install/upgrade — it ships with the module and reflects
  the version's feature set.
- `config.json` is **preserved** across upgrades. The user's saved
  values survive.
- `secrets/*.txt` are **preserved** across upgrades. API keys
  survive.
- `default_source` files (e.g. `default_system_prompt.txt`) are
  **overwritten** — they're part of the shipped module.

Uninstall removes the entire module directory atomically — schema,
config, and secrets all go away together.

**Do not ship `config.json` or `secrets/` in your release tarball.**
schwung-manager defends against this by snapshotting and restoring
both before/after extraction, but the cleanest module tarball
contains neither.

### File ownership

schwung-manager runs as root on the device but the module's
`ui.js` runs as `ableton`. schwung-manager `Lchown`s every file
it writes inside a module directory (`config.json`, `secrets/`,
each secret file) to `ableton:users` so the module can read its
own values.

### Build script

If you have files referenced by `default_source`, make sure your
build/packaging includes them. The main repo's `scripts/build.sh`
copies `*.{js,mjs,json,sh,py,txt}` from `src/modules/`; module
repos typically mirror that pattern.

## Remote UI Custom HTML (web_ui.html)

A module that wants a fully custom browser-based UI ships a
`web_ui.html` next to its `module.json`. schwung-manager
auto-discovers it and loads it in a sandboxed iframe on the Remote
UI page (`http://move.local:7700/remote-ui`) whenever that module
is loaded in a shadow slot. The iframe replaces the auto-generated
knob/slider UI for that component.

**Any chain component can supply one** — the synth, an audio FX
(`fx1`/`fx2`), or a MIDI FX (`midi_fx1`). Where it appears differs:

- A **synth** panel takes over the slot view, with the other
  components' sections rendered below it.
- An **audio/MIDI FX** panel renders inside that component's own
  collapsible section, with its own pop-out button, so a slot can
  show a custom synth panel and a custom FX panel at once.

The slot's **Interface: Default** toggle still shows the
auto-generated controls for every component, so a module's own
parameters stay reachable whatever its panel does.

A **Master FX** position works the same way as an audio FX: the panel
renders inside that position's own section, with its own pop-out, and
the Master FX tab has its own Interface toggle. The component there is
`master_fx:fx1` … `master_fx:fx4`, so a panel that builds its keys from
`schwungRemote.component` needs no change to work on the master bus —
one that hardcodes `fx1:` will address a chain slot instead.

**Know which component you are driving.** The manager appends
`?component=<comp>` to the iframe URL and exposes it as
`schwungRemote.component`. Use it to build your parameter keys —
an FX panel must write `fx1:mix`, not `synth:mix`:

```js
var comp = schwungRemote.component || "synth";   // "synth", "fx1", …
schwungRemote.setParam(comp + ":mix", "0.5");
```

`getHierarchy()` and `getChainParams()` already return the metadata
for *your* component, so they need no prefix. A page that ignores
the flag defaults to `synth` and behaves exactly as before.

### File layout

```
modules/<category>/<your-module>/
  module.json
  web_ui.html         # entry point loaded into the iframe
  assets/             # optional: any sibling files (js, css, png, …)
    app.js
    style.css
```

Anything under the module directory is served at
`/api/remote-ui/module-assets/<module-id>/<path>` (see
`handleModuleWebUIAsset` in `schwung-manager/main.go`). Relative
URLs in `web_ui.html` (e.g. `<link href="assets/style.css">`)
resolve against that prefix, so a single module folder is
self-contained.

The asset endpoint rejects `..`, absolute paths, and any
`module.json`/`config.json`/`secrets/` request — those stay
private to the device.

### schwungRemote JavaScript API

schwung-manager exposes a small postMessage bridge to the iframe.
Include the bundled helper to get a Promise-based wrapper:

```html
<script src="/static/schwung-remote-api.js"></script>
<script>
  // Subscribe to parameter changes (hardware knobs, other clients).
  schwungRemote.onParamChange(function (params) {
    // params is { "synth:cutoff": "0.42", ... }
  });

  // Read a single value (resolves with the current cached string).
  schwungRemote.getParam("synth:cutoff").then(function (val) { ... });

  // Write a value. Fire-and-forget; goes straight to the device.
  schwungRemote.setParam("synth:cutoff", "0.75");

  // Read structural metadata once on load.
  schwungRemote.getHierarchy().then(function (hier) { ... });   // ui_hierarchy
  schwungRemote.getChainParams().then(function (params) { ... }); // chain_params array
</script>
```

| Method | Direction | Notes |
|--------|-----------|-------|
| `onParamChange(cb)` | device → iframe | Subscribes the iframe to all `param_update` messages for the active slot. Includes hardware knob changes and updates from other browser clients. |
| `getParam(key)` | local cache | Returns the **last value the iframe has seen** for `key`, not a fresh device read. If you need a value before any update has arrived, call `onParamChange` first and seed from the initial burst. |
| `setParam(key, value)` | iframe → device | Goes through the WebSocket `set_param` path. Value is coerced to a string. |
| `getHierarchy()` | local cache | Returns the parsed `ui_hierarchy` object for **your** component (or `null` if the module didn't expose one). |
| `getChainParams()` | local cache | Returns the parsed `chain_params` array for **your** component (or `null`). |
| `component` | property | The chain component this page drives — `"synth"`, `"fx1"`, `"fx2"`, `"midi_fx1"`. Use it to build parameter keys. |

**Param keys are component-prefixed.** Use `"synth:cutoff"`, not
`"cutoff"` — and build the prefix from `schwungRemote.component`
rather than hardcoding `synth`, or your page will only work in a
synth slot. Slot-level keys like `slot:volume` and `knob_1_value`
also flow through `onParamChange` if you want to mirror the
slot/knob state.

### Iframe sandbox

The iframe is created with `sandbox="allow-scripts allow-same-origin"`.
That means:
- `<script>` runs and `fetch`/`XHR` to same-origin URLs works.
- No top-level navigation, popups, form submission, or pointer
  lock — design the UI to stay in-frame.
- No access to the parent window beyond `window.parent.postMessage`,
  which is exactly what `schwungRemote` uses.

### Minimal example

```html
<!doctype html>
<html><head><meta charset="utf-8"><title>My Module</title>
<style>
  body { font-family: system-ui; margin: 1em; }
  input[type=range] { width: 100%; }
</style>
</head><body>
  <label>Cutoff <input id="cutoff" type="range" min="0" max="1" step="0.01"></label>

  <script src="/static/schwung-remote-api.js"></script>
  <script>
    // Works in a synth slot or an FX slot: the host says which.
    const comp = schwungRemote.component || "synth";
    const key = comp + ":cutoff";

    const cutoff = document.getElementById("cutoff");
    cutoff.addEventListener("input", () => {
      schwungRemote.setParam(key, cutoff.value);
    });
    schwungRemote.onParamChange((params) => {
      if (params[key] !== undefined) {
        cutoff.value = params[key];
      }
    });
  </script>
</body></html>
```

### Notes

- The custom UI **adds to**, not replaces, the hardware UI on the
  Move display. Modules still need a working `ui.js` (or a chain
  shim) — the iframe only affects the browser view.
- `web_ui.html` is reloaded whenever the slot's synth module
  changes. Persist any iframe-side UI state via `setParam` or
  your own storage; don't rely on the iframe surviving slot swaps.
- Available since schwung-manager landed the Remote UI custom
  HTML support (see `docs/plans/2026-04-08-remote-ui-plan.md`
  Task 5). Bump `min_host_version` in your catalog entry if your
  module depends on it.

### Remote UI for overtake tools (the Tool tab)

Overtake tools (dsp.so loaded by the shim as `overtake_dsp`, not a chain slot)
get their own browser view: schwung-manager serves the tool's `web_ui.html`
under the **Tool tab**, addressed via the `overtake_dsp:<key>` param prefix.

- **Opt in** by answering `get_param("module_id")` with your module id. The
  manager probes it to discover the active tool, announces arrival/departure
  to open Tool tabs, and serves `web_ui.html` from your module folder.
- **Reads**: the manager seeds and refreshes the browser from
  `get_param("state")` — return a **flat JSON object of delimited string
  values** (nested arrays/objects are dropped by the param explosion).
- **Writes**: browser `setParam` calls with values under 256 bytes arrive as
  ordinary `set_param` dispatched from the shim's **web-set ring drain**
  (lossless, ~one SPI frame, immune to param-mailbox contention); larger
  values take the shadow_param mailbox (serialized round-trip, up to 64 KB).
  **No per-buffer coalescing** on either path (unlike the on-device JS
  channel). Ordering across the two paths is not guaranteed — keep any
  op-sequencing within one size class.
- **Off-audio-thread snapshots (optional)**: answer
  `get_param("remote_snapshot_rt_safe")` with `"1"` and the host serializes
  your `"state"` snapshot on a low-priority worker thread into a cached,
  rev-stamped double buffer; the audio thread then serves browser pulls with
  a single memcpy instead of running your serializer in the SPI frame budget.
  **Contract:** every byte of instance memory reachable by your `"state"` /
  `"rui_poll"` get_param must stay valid for the LIFETIME of the instance —
  never freed or realloc'd by `render_block` OR `set_param` (frees only in
  `destroy_instance`; use grow-only / clear-and-keep pools). Torn or
  one-rev-stale snapshots are acceptable (the manager re-pulls until the
  snapshot's own rev matches the digest); a use-after-free is not. Include
  your current rev in the `"state"` JSON so the manager can tell what it
  received. Note: a BULK_GET (request_type 3) of `"state"` bypasses this
  cache and runs your serializer on the audio thread — don't put `"state"`
  in bulk reads if you opt in.
- **Live sync (optional but recommended)**: expose a monotonic edit counter
  and a cheap digest, and the host pushes changes to the browser on-change:
  - `get_param("rui_poll")` → `rev:on:tick:bpm[:devms]` — `rev` bumps on every
    snapshot-visible edit; `on/tick/bpm` describe transport; `devms` (playing
    only — the stopped digest must stay byte-stable) is a free-running
    device-clock ms that lets the browser time-base its playhead independent
    of delivery latency. The shim probes this in-process every few frames and
    pushes changes through the notify ring; without it the manager falls back
    to polling.
  - Key-naming convention: keys suffixed `_ruisel` / `_cc_focus` and the key
    `transport` are treated as selection/transport (the manager echoes
    snapshots to their sender immediately instead of applying the editing
    quiet-window).

## Drop-In Modules

Modules are discovered at runtime from `/data/UserData/schwung/modules`.
To add a new module, copy a folder with `module.json` (plus `ui.js` and `dsp.so`
if needed) and either restart Schwung or call `host_rescan_modules()` in
your UI. No host recompile is required for new modules or UI updates.

## JavaScript UI (ui.js)

Module UIs are loaded as ES modules, so you can import shared utilities:

```javascript
import {
    MoveMainKnob, MoveShift, MoveMenu,
    MovePad1, MovePad32,
    MidiNoteOn, MidiCC
} from '../../shared/constants.mjs';

/* Module state */
let counter = 0;

/* Called once when module loads */
globalThis.init = function() {
    console.log("Module starting...");
    clear_screen();
    print(2, 2, "Hello Move!", 2);
}

/* Called every frame (~60fps) */
globalThis.tick = function() {
    // Update display here
}

/* Handle MIDI from external USB devices */
globalThis.onMidiMessageExternal = function(data) {
    // data = [status, data1, data2]
}

/* Handle MIDI from Move hardware */
globalThis.onMidiMessageInternal = function(data) {
    const isNoteOn = data[0] === 0x90;
    const note = data[1];
    const velocity = data[2];

    // Ignore capacitive touch from knobs
    if (note < 10) return;

    // Handle pad press
    if (isNoteOn && note >= 68 && note <= 99) {
        console.log("Pad pressed: " + note);
    }
}
```

### Signal Chain UI Shims

Modules can expose a full-screen UI when used as a Signal Chain MIDI source by
adding `ui_chain.js` (or setting `"ui_chain"` in `module.json` to a different
filename). The file should set `globalThis.chain_ui`:

```javascript
globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
};
```

Do not override `globalThis.init` or `globalThis.tick` in `ui_chain.js`.
Make sure to ship `ui_chain.js` in your build/install step if you use it.
The host itself ignores `ui_chain`; it is consumed by the Signal Chain UI when
loading a MIDI source module.

Example `ui_chain.js` wrapper:

```javascript
import {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
} from './ui_core.mjs';

globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal,
    onMidiMessageExternal
};
```

#### If your `ui_chain.js` draws the knob grid (`param_pages`)

A module may bind Schwung's own page engine instead of drawing its own screens
— import `page_controller.mjs` from `/data/UserData/schwung/shared/param_pages/`
and you get the whole knob grid, the widgets, the viz graphics, the section
picker and the gestures for free.

**Then two calls are yours, not one:**

```javascript
clear_screen();                                   /* the frame is YOURS */
controller.render(ctx, { title: title() });
controller.renderOverlays(ctx, { clearScreen: clear_screen });
```

The library never clears the screen — that is what lets `render()` place a page
inside a rect you own — so anything full-screen is handed back to you.
Today that is the enum peek: turn a multi-option enum and its option list
should rise over the grid for ~700 ms. Skip `renderOverlays` and the controller
still tracks the peek and still swallows the Back that dismisses it; it is
simply never painted, with no error anywhere. Two shipped modules had exactly
that bug.

#### Back-button handling (`handleBack`)

A chain module's `ui_chain.js` may export `handleBack()`. When the shadow UI is in
COMPONENT_EDIT with your module's chain UI loaded, a Back press calls your
`handleBack()` first:

- return a **truthy** value to **consume** Back (you handled internal navigation
  — e.g. popped your own sub-view);
- return **falsy** / omit the method to let the host handle Back (unload the module
  UI and return to the chain editor).

Only consume Back while you actually have somewhere to go back *to*. If `handleBack()`
always returns truthy the user can never leave your module via Back — it is the only
host-processed exit in this screen, so the sole remaining way out is to exit shadow mode.

#### Copy / Delete / Undo (`ui_chain.js`)

While the shadow UI is on screen, CC 56 (Undo), CC 60 (Copy), and CC 119
(Delete) are delivered exclusively to the loaded module's `ui_chain.js` via
`onMidiMessageInternal` — they are blocked from reaching Move firmware for
the duration, so a press can never double-fire into Move's own undo/copy/
delete while you're editing a module (e.g. a Delete press won't also delete
a Move clip in the background).

This makes the three buttons safe to repurpose for module-specific gestures
— e.g. hold Copy/Delete + tap a pad to target it, tap Undo to revert the
last such operation. They join the existing forwarded set (jog wheel/click,
Back, track buttons, knobs, Mute) that a chain module already receives in
this screen.

Outside shadow display (or outside COMPONENT_EDIT), these CCs behave as
normal Move hardware buttons and are not intercepted.

### Menu Layout Helpers

For list-based screens (title/list/footer), use the shared menu layout helpers:

```javascript
import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    menuLayoutDefaults
} from '../../shared/menu_layout.mjs';

const items = [
    { label: "Velocity", value: "Hard" },
    { label: "Aftertouch", value: "On" }
];

drawMenuHeader("Settings");
drawMenuList({
    items,
    selectedIndex,
    listArea: {
        topY: menuLayoutDefaults.listTopY,
        bottomY: menuLayoutDefaults.listBottomWithFooter
    },
    valueAlignRight: true,
    getLabel: (item) => `${item.label}:`,
    getValue: (item) => item.value
});
drawMenuFooter("Back:back  </>:change");
```

`drawMenuList` will derive row count from the list area and scroll automatically. When `valueAlignRight` is enabled, labels are truncated with `...` if they would overlap the value.

## Menu System

For modules that need hierarchical settings menus, the shared menu system provides a complete solution for navigation, input handling, and rendering.

### Menu Item Types

Import factory functions from `menu_items.mjs`:

```javascript
import {
    MenuItemType,
    createSubmenu,
    createValue,
    createEnum,
    createToggle,
    createAction,
    createBack,
    formatItemValue,
    isEditable
} from '../../shared/menu_items.mjs';
```

| Type | Factory | Description |
|------|---------|-------------|
| `SUBMENU` | `createSubmenu(label, getMenu)` | Navigate to child menu |
| `VALUE` | `createValue(label, {get, set, min, max, step, fineStep, format})` | Numeric value with range |
| `ENUM` | `createEnum(label, {get, set, options, format})` | Cycle through string options |
| `TOGGLE` | `createToggle(label, {get, set, onLabel, offLabel})` | Boolean on/off |
| `ACTION` | `createAction(label, onAction)` | Execute callback on click |
| `BACK` | `createBack(label)` | Return to parent menu |

Example menu definition:

```javascript
function getSettingsMenu() {
    return [
        createEnum('Velocity', {
            get: () => host_get_setting('velocity_curve'),
            set: (v) => { host_set_setting('velocity_curve', v); host_save_settings(); },
            options: ['linear', 'soft', 'hard', 'full']
        }),
        createValue('AT Deadzone', {
            get: () => host_get_setting('aftertouch_deadzone'),
            set: (v) => { host_set_setting('aftertouch_deadzone', v); host_save_settings(); },
            min: 0, max: 50, step: 5, fineStep: 1
        }),
        createToggle('Aftertouch', {
            get: () => host_get_setting('aftertouch_enabled') === 1,
            set: (v) => { host_set_setting('aftertouch_enabled', v ? 1 : 0); host_save_settings(); }
        }),
        createSubmenu('Advanced', () => getAdvancedMenu()),
        createBack()
    ];
}
```

### Menu Navigation

The `menu_nav.mjs` module handles all input for menu navigation:

```javascript
import { createMenuState, handleMenuInput } from '../../shared/menu_nav.mjs';
import { createMenuStack } from '../../shared/menu_stack.mjs';

const menuState = createMenuState();
const menuStack = createMenuStack();

// Initialize with root menu
menuStack.push({ title: 'Settings', items: getSettingsMenu() });

// In onMidiMessageInternal:
function onMidiMessageInternal(data) {
    if ((data[0] & 0xF0) === 0xB0) {  // CC message
        const cc = data[1];
        const value = data[2];
        const current = menuStack.current();

        const result = handleMenuInput({
            cc, value,
            items: current.items,
            state: menuState,
            stack: menuStack,
            shiftHeld: isShiftHeld,
            onBack: () => host_return_to_menu()
        });

        if (result.needsRedraw) {
            redraw();
        }
    }
}
```

**Navigation behavior:**
- **Jog wheel**: Scroll list (navigation) or adjust value (editing)
- **Jog click**: Enter submenu, start/confirm edit, execute action
- **Up/Down arrows**: Scroll list
- **Left/Right arrows**: Quick-adjust values without entering edit mode
- **Back button**: Cancel edit or go back in menu stack

### Encoder Acceleration

When editing numeric values with the jog wheel, acceleration provides smooth control:

```javascript
import { decodeDelta, decodeAcceleratedDelta } from '../../shared/input_filter.mjs';

// Simple delta (±1) for navigation
const delta = decodeDelta(ccValue);

// Accelerated delta for value editing
// Slow turns = step 1, fast turns = step up to 10
const accelDelta = decodeAcceleratedDelta(ccValue, 'my_encoder');
```

- Slow turns (<150ms between events): step = 1 (fine control)
- Fast turns (<25ms between events): step = 10 (coarse control)
- In between: interpolated step size
- Hold **Shift** for fine control (always step 1)

### Text Scrolling

Long labels automatically scroll after a delay:

```javascript
import { createTextScroller, getMenuLabelScroller } from '../../shared/text_scroll.mjs';

// Use singleton for menu labels
const scroller = getMenuLabelScroller();

// In tick():
scroller.setSelected(selectedIndex);  // Reset scroll on selection change
if (scroller.tick()) {
    redraw();  // Scroll position changed
}

// When rendering:
const displayText = scroller.getScrolledText(fullLabel, maxChars);
```

**Scroll behavior:**
- 2 second delay before scrolling starts
- ~100ms between scroll steps
- 2 second pause at end, then reset

### Menu Stack

For hierarchical menus with back navigation:

```javascript
import { createMenuStack } from '../../shared/menu_stack.mjs';

const stack = createMenuStack();

// Push root menu
stack.push({ title: 'Main', items: mainMenuItems });

// Navigate to submenu
stack.push({ title: 'Settings', items: settingsItems, selectedIndex: 0 });

// Go back
stack.pop();

// Get current menu
const current = stack.current();  // { title, items, selectedIndex }

// Get breadcrumb path
const path = stack.getPath();  // ['Main', 'Settings']
```

## Native DSP Plugin

For audio synthesis/processing, create a native plugin implementing the C API.

### Threading: there is no control thread

**Read this before writing a line of DSP.** Every plugin entry point runs on the
SPI audio callback — SCHED_FIFO 90, core 3, ~900 µs of budget per block:

| Entry point | Runs on the SPI callback? |
|---|---|
| `create_instance` / `destroy_instance` | **yes** |
| `set_param` | **yes** |
| `get_param` | **yes** |
| `on_midi` / `process_midi` | **yes** |
| `render_block` / `process_block` / `tick` | **yes** |

There is no separate control thread, no UI thread, and no "MIDI thread". A
2026-08 audit of all 113 catalogued modules found ~150 confirmed realtime
violations, and several came with comments asserting the opposite — *"control
thread only (blocking dir + file I/O)"*, *"run on the control thread, NEVER from
process_block — so this malloc is realtime-safe"*, *"safe because it runs in the
MIDI callback (not the RT render thread)"*. Each of those produced a
multi-megabyte blocking operation on the audio thread.

**Never, in any of the calls above:**

- file I/O — `fopen`/`fread`/`fwrite`/`open`/`stat`/`opendir`/`readdir`/`mkdir`
- allocation or free — `malloc`/`calloc`/`realloc`/`free`/`new`/`delete`
- locks held by a non-realtime thread
- `fork`/`exec`/`system`/`popen`/`dlopen`
- logging, including `host->log` **and a bare `fprintf(stderr, …)`** — stderr is
  unbuffered, so that is a `write()` syscall even when your debug flag is off
- unbounded work: an FFT, a whole-buffer `memset`, a directory sort

The symptom is not a glitch in your module: it is a **device-wide** audio
dropout, because you are holding the thread that services every other module's
audio and Move's own.

#### Threads inherit SCHED_FIFO 90

`pthread_create()` from any of those entry points gives your worker the audio
callback's realtime priority. Move's own `Link Main` publisher runs at **FIFO
35**, so an inherited-priority worker starves Move's audio and causes the very
dropouts you went off-thread to avoid. Demote as the worker's first action:

```c
static void *worker(void *arg) {
    struct sched_param sp = { .sched_priority = 0 };
    sched_setscheduler(0, SCHED_OTHER, &sp);      /* MUST be first */

    cpu_set_t set; CPU_ZERO(&set);                 /* keep core 3 free for SPI */
    CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
    sched_setaffinity(0, sizeof(set), &set);
    ...
}
```

References that do this correctly: `schwung-keydetect`
(`keyfinder_wrapper.cpp`) and `schwung-airwindows` (`clap_fx.cpp`,
`loader_thread_fn`). The audit found at least 14 modules that do not.

#### What to do instead

Load files, allocate buffers and build lists on your own `SCHED_OTHER` worker,
and publish the result to the audio path as a pointer swap. Keep `get_param`
cheap in particular — a `get_param` that rescans a directory is served on the
audio thread **once per repaint**, not once per click, so it is worse than the
equivalent `set_param`. Seven modules in the audit had exactly that bug.

See `docs/REALTIME_SAFETY.md` for the measurements.

### Plugin API v2 (Recommended)

V2 supports multiple instances and is **required for Signal Chain integration**:

```c
#include "host/plugin_api_v1.h"  /* v2 API is defined in this file */

typedef struct my_instance {
    // Your synth state here
    float sample_rate;
    int preset;
} my_instance_t;

static void* create_instance(const char *module_dir, const char *json_defaults) {
    my_instance_t *inst = calloc(1, sizeof(my_instance_t));
    inst->sample_rate = 44100.0f;
    // Parse json_defaults if needed
    return inst;
}

static void destroy_instance(void *instance) {
    free(instance);
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    my_instance_t *inst = (my_instance_t*)instance;
    // source: 0 = internal (Move), 1 = external (USB)
}

static void set_param(void *instance, const char *key, const char *val) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Handle parameter changes
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Return parameter value, ui_hierarchy, chain_params, etc.
    return -1;
}

static void render_block(void *instance, int16_t *out_lr, int frames) {
    my_instance_t *inst = (my_instance_t*)instance;
    // Generate 'frames' stereo samples
    // Output format: [L0, R0, L1, R1, ...]
}

/* Export the plugin API */
static plugin_api_v2_t api = {
    .api_version = 2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .render_block = render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t* host) {
    return &api;
}
```

### Runtime Modulation Callbacks (Chain Host)

When a plugin runs inside Signal Chain, `host_api_v1_t` may provide optional modulation callbacks:

```c
int (*mod_emit_value)(void *ctx,
                      const char *source_id,
                      const char *target,
                      const char *param,
                      float signal,
                      float depth,
                      float offset,
                      int bipolar,
                      int enabled);
void (*mod_clear_source)(void *ctx, const char *source_id);
void *mod_host_ctx;
```

Use these to publish temporary modulation overlays without overwriting a target parameter's saved base value.

Guidelines:
- `source_id`: stable ID for the modulation source instance/lane.
- `target`: `"synth"`, `"fx1"`, `"fx2"`, `"midi_fx1"`, or `"midi_fx2"`.
- `enabled=0` or `mod_clear_source(...)`: clears that source's contribution.
- Missing/stale targets should fail silently (do not crash or spam logs).
- Multiple sources can target the same parameter; the host sums contributions and clamps to target range.

### Plugin API v1 (Deprecated)

V1 is a singleton API - only one instance can exist. **Do not use for new modules:**

```c
#include "plugin_api_v1.h"

static int on_load(const char *module_dir, const char *json_defaults) {
    return 0;  // 0 = success
}

static void on_unload(void) { }

static void on_midi(const uint8_t *msg, int len, int source) { }

static void set_param(const char *key, const char *val) { }

static int get_param(const char *key, char *buf, int buf_len) {
    return -1;
}

static void render_block(int16_t *out_lr, int frames) { }

static plugin_api_v1_t api = {
    .api_version = 1,
    .on_load = on_load,
    .on_unload = on_unload,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .render_block = render_block,
};

const plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t* host) {
    return &api;
}
```

### Building DSP Plugins

Add to `scripts/build.sh`:

```bash
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/your-module/dsp/your_plugin.c \
    -o build/modules/your-module/dsp.so \
    -Isrc -Isrc/modules/your-module/dsp \
    -lm
```

## JS ↔ DSP Communication

Use `host_module_set_param()` and `host_module_get_param()` in your UI:

```javascript
// In ui.js
host_module_set_param("preset", "5");
let current = host_module_get_param("preset");
```

The DSP plugin receives these in `set_param()` and `get_param()`.

## Shadow UI Parameter Hierarchy

Modules expose a navigable parameter hierarchy to the Shadow UI via `ui_hierarchy` in module.json or `get_param("ui_hierarchy")`. The hierarchy uses a **levels dictionary** format with named levels:

```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "name": "My Synth",
        "params": [
          {"key": "cutoff", "name": "Cutoff", "type": "int", "min": 0, "max": 127},
          {"key": "mode", "name": "Mode", "type": "enum", "options": ["LP", "HP", "BP"]},
          {"level": "advanced", "label": "Advanced Settings"}
        ],
        "knobs": ["cutoff", "mode"]
      },
      "advanced": {
        "name": "Advanced",
        "params": [
          {"key": "drive", "name": "Drive", "type": "float", "min": 0, "max": 1}
        ],
        "knobs": ["drive"]
      }
    }
  }
}
```

### Level Fields

| Field | Description |
|-------|-------------|
| `name` / `label` | Display name for the level |
| `params` | Array of parameter items (see below) |
| `knobs` | Array of parameter keys mapped to physical knobs 1-8 |
| `list_param` / `count_param` / `name_param` | For preset browser levels |
| `items_param` / `select_param` | For dynamic item selection levels |
| `child_prefix` / `child_count` / `child_label` | For repeated elements (see below) |
| `navigate_to` | Where to land after choosing from this level's list (see below) |
| `visible_if` | Optional conditional visibility rule for this level |
| `menu` | Array of `{label, action?, level?, value?}` rendered as a plain list page (see below) |

#### `menu` — a level's own action list, and where it lands

A level can declare its own list of actions/jumps that have no value to turn:

```json
"menu": [
  { "label": "Save", "action": "save" },
  { "label": "LFO 1", "level": "lfo1" },
  { "label": "Mode", "action": "mode", "value": "Poly" }
]
```

`value` is optional and right-aligned, so a settings-style menu reads like a
list. **It lands right after THAT LEVEL's own grid pages — not at the end of
the whole plan.** A `menu` on `root` is the SECOND page a user sees, not the
last, because the tree walk emits it before descending into any level `root`
navigates to. If you want your menu to read as a true finale, put it on its
own level and reference that level last — that is how Slot Settings does it,
and it only works there because that screen synthesises its entire hierarchy
end to end. A module author does not own the order the rest of the walk
takes, so there is no way to make an ordinary level's `menu` "last" from
inside `ui_hierarchy` itself.

**You never need this to get User Presets / Module actions.** Every loaded
chain component already gets a "My Presets" and a "Module" page appended
after its whole jog sequence, for free — declare nothing. See CLAUDE.md,
"Every component's knob grid ends with two pages it never declared", for why
that append happens in the planner rather than through this field.

#### Selector keys must not appear in `knobs`

The keys named by `list_param`, `count_param`, `name_param`, `items_param` and
`select_param` get their own page — a preset browser or an items list — which is
a better control than a knob and is where the host expects you to choose from.
**Listing one in `knobs` as well is now ignored.**

Two modules did it (`impressive-chords` `preset_index`, `breakbeat` `preset`) and
in both the `knobs` array was byte-identical to `params` — everything listed
rather than eight chosen — so the selector happened to land on knob 1. It could
not have worked anyway: `impressive-chords` declares `preset_index` as
`int 0..500` against 52 presets, so ~90% of that knob was dead travel landing on
nothing.

You lose nothing by leaving it out. Reported from the device as *"why is preset a
knob on impressive chords?"*.

#### `navigate_to` — where choosing leaves you

A level with `items_param` may declare `navigate_to: "<level>"`, meaning *"having
chosen from this list, the user wants to be there"*. Without it, choosing lands
on the first grid page.

Two behaviours worth knowing, both of which changed in 2026-08:

- **If the named level plans BOTH a preset browser and a knob grid, you get the
  browser.** `obxd` declares `banks -> navigate_to: "root"`, and its `root`
  carries `list_param`/`count_param` *and* `knobs`. Naming the level never said
  which, and the lookup used to filter to knob pages only — so choosing a bank
  landed on the sliders instead of in that bank's presets. A chooser that filters
  a list means "now show me the list".
- **You arrive with the page already open.** The jog is normally inert on a
  preset browser or items list until you click in (so that *paging past* one
  cannot audition every preset it goes by). A page you were **sent** to opens:
  you did not page there, you chose your way there, and one deliberate gesture
  should not need a second to take effect. Naming a level that plans no preset
  page still lands on its grid, as before.

There is deliberately no `navigate_to: {level, kind}` form. Only three modules in
the fleet declare `navigate_to` at all, and new vocabulary nobody adopts is how
`options_as_string` sat documented and unused for months.

### Parameter Item Types

Each entry in `params` is either:

- **Editable param**: `{"key": "cutoff", "name": "Cutoff", "type": "int", "min": 0, "max": 127}`
- **Navigation link**: `{"level": "advanced", "label": "Advanced Settings"}`

**Important:** Use `key` (not `param`) for editable parameter objects. Metadata (type, min, max) can come from either the hierarchy or `chain_params`.

### Naming a parameter: the knob grid has ~5 characters

A `name` (or a hierarchy item's `label`) is drawn into a 32px-wide cell on the
knob grid, and the label band budgets **the width of five `M`s**, which is
about 5-7 characters depending on the letters. Everything longer is squeezed,
and knowing how it is squeezed is the difference between a readable page and a
row of stumps.

Three passes, in order:

1. **Per-word mnemonics.** A shared table maps common audio vocabulary to a
   fixed short form — `attack`→`ATK`, `envelope`→`ENV`, `resonance`→`RES`,
   `rotation`→`ROT`, `polyphony`→`POLY`. Applied word by word, so "Filter
   Envelope" becomes `FLT ENV`.
2. **Squeeze.** Whatever is left is fitted to the cell: initials for the
   leading words, the last word kept longest, trailing indices preserved
   ("Osc 1 Octave" keeps its 1).
3. **Truncate**, if it still does not fit.

**The failure mode worth designing against is step 2 on an unknown word: it
drops vowels.** A word the table does not know becomes a non-word rather than
an abbreviation — `Rotation` used to draw `ROTATN`, `Wave Group` drew `WGROU`,
`Polyphony` drew `PLYPHN`. That is worse than truncation, and it is not fixed
by a wider cell.

So:

- **Prefer a short `name`.** "Fdbk Src" beats "Feedback Source"; you know the
  abbreviation you want, and the renderer is guessing.
- **Check what it actually draws** rather than counting characters — the
  budget is in pixels, so `I` and `M` are not the same:

  ```bash
  node -e 'import("./src/shared/param_pages/render_page_movy.mjs")
    .then(R => console.log(R.labelForCell("Your Param Name")))'
  ```

  Or see the whole page: `node tools/param-pages/preview_knob_card.mjs <module-id>`.
- **If a common word is missing from the table, add it there** rather than
  pre-abbreviating in your module — an entry fixes that word for every module
  that uses it. The table is `WORD_ABBREV` in
  `src/shared/param_pages/render_page_movy.mjs`, with the rules and a test in
  `tests/host/test_label_abbrev.sh`. Do not add a word that already fits: a
  mnemonic is a consolation for not fitting, never an improvement on the real
  word.

**Enum `options` are tighter still.** An enum value is drawn in a ~16px square,
roughly 3-4 characters, so long option strings are heavily cut. `["In", "Out"]`
survives where `["Input Env", "Output Env"]` does not — and the parameter's own
name is already carrying the context, so the option only has to disambiguate.

### Parameter Types

| Type | Fields | Description |
|------|--------|-------------|
| `int` | `min`, `max` | Integer value with knob control |
| `float` | `min`, `max`, `step` | Float value (0.0-1.0 typical) |
| `enum` | `options` | List of string options |
| `filepath` | `root`, `start_path`, `filter` | Opens Shadow UI file browser and stores selected path |
| `module_picker` | `allow_none`, `allow_self`, `allowed_targets`, `param_key` | Dynamic enum from loaded chain components |
| `parameter_picker` | `target_key`, `numeric_only`, `allow_none` | Dynamic enum from selected target's exposed params |
| `mode` | `options` | Mode selector (like enum, triggers mode switch) |
| `note` | `mode`, `min_note`, `max_note` | Generated note selector (`single` uses note names only, `multi` includes octaves) |
| `rate` | `include_bars`, `bars_mode`, `include_triplets` | Generated musical rate list (divisions, triplets, bars) |
| `wav_position` | `display_unit`, `mode`, `filepath_param`, `min`, `max`, `step`, `shift_increment_multiplier` | Numeric position/trim param with waveform preview and marker |
| `string` | none (or `default`/`value`) | Opens on-screen text entry keyboard on edit |
| `canvas` | `display_value_type`, `canvas_script`, `canvas_overlay`, `show_footer`, `show_value` | Opens fullscreen module-defined canvas UI when clicked |

`rate.bars_mode` values:
- `bars-every` (default): every bar count from `16` down to `1`
- `bars-simple`: `16, 8, 4, 2, 1`
- Legacy aliases are still accepted: `pow2` -> `bars-simple`, `all` -> `bars-every`

Rate options are emitted from slowest to fastest timing, for example:
`16 bars, ... 2 bars, 1 bar, 1/1T, 1/2, 1/2T, 1/4, ...`

`wav_position` and `canvas` behavior details are documented in dedicated subsections below:
- `wav_position` in module.json
- `canvas` in module.json

`visible_if` can be attached to level entries and param entries:

```json
{
  "param": "sync",
  "equals": true
}
```

Supported condition fields:
- `equals`, `not_equals`
- `gt`/`greater_than`/`greater`
- `lt`/`smaller_than`/`smaller`
- `truthy`, `falsey`/`falsy`

Visibility is evaluated dynamically; hidden entries are removed from list navigation and knob mappings for that level.

### Parameter visualisations (`viz`)

A knob page can draw a parameter *group* as a picture instead of separate
controls — an ADSR as an envelope, cutoff+resonance as a filter response, three
band gains as an EQ curve. Declare the grouping and the UI draws it; leave it out
and the UI falls back to detecting the group from names and ranges, which works
but is a guess.

**Declare it.** Detection exists so that modules which say nothing still get
graphics; it is a fallback, not the contract. A declaration is also the only way
to correct a detector that guesses wrong.

Add an optional `viz` object to a `chain_params` entry:

```json
{ "key": "attack",  "name": "Attack",  "type": "float", "viz": { "group": "amp", "role": "attack"  } },
{ "key": "decay",   "name": "Decay",   "type": "float", "viz": { "group": "amp", "role": "decay"   } },
{ "key": "sustain", "name": "Sustain", "type": "float", "viz": { "group": "amp", "role": "sustain" } },
{ "key": "release", "name": "Release", "type": "float", "viz": { "group": "amp", "role": "release" } }
```

| Field | Meaning |
|-------|---------|
| `group` | An id shared by every param in one graphic. Any string; scoped to the module. Omit for single-param graphics. |
| `role` | This param's part in the group. Required when `group` is set. |
| `kind` | The graphic type. Optional — derived from the roles present when omitted. |
| `viz: false` | Never draw a graphic for this param, whatever a detector thinks. |

Kinds and their roles:

| `kind` | Roles | Notes |
|--------|-------|-------|
| `envelope` | `attack`, `decay`, `sustain`, `release` | Any 2–4 of them: AD, AR, ASR and ADSR all draw. |
| `filter` | `cutoff`, `resonance`, optional `mode`, `slope` | `mode` should be an enum naming LP/HP/BP/notch. |
| `eq` | `low`, `mid`, `high` | Band **gains**, not crossover frequencies. |
| `lfo` | `shape`, `rate`, `depth`, optional `phase` | `shape` should be an enum of waveform names. |
| `waveform` | *(single param)* | An enum of waveform names, drawn as silhouettes. |
| `fader` | *(single param)* | A level/volume, drawn as a fader rather than a dial. |
| `switch` | *(single param)* | A `toggle` or **boolean-flavoured** two-option enum, drawn as an on/off switch. See the note below — not every two-option enum qualifies, and it changes the behaviour as well as the picture. |
| `sample` | *(single param)* + optional `position` | A `filepath`; a companion `wav_position` param marks playback position on the waveform. |

**A `switch` is not just a picture — it suppresses the option list.** Turning an
enum knob normally flashes its options up over the grid for ~700ms. A switch
does not, because it already draws both of its states: the track is one and its
inversion is the other, so a list of `Off`/`On` says what the cell says.

That makes the Off/On wording load-bearing. A two-option enum whose values are a
straight **choice** rather than a boolean — `Mix`/`Reverb`, `Saw`/`Square`,
`Legato`/`Trig`, `Time`/`Rate` — is deliberately *not* detected as a switch: it
draws as an enum square showing one word, and the other word is exactly what the
list is for. 134 cells across the fleet are in that group against 212 real
switches, so this is a live distinction, not an edge case.

Practical upshot: if your parameter genuinely means on/off, name the options so
it reads that way and it will get the switch and lose the list. If it is a
choice between two named things, leave it as a plain enum.

Two more rules worth knowing:

- **A group's params must land contiguously on one row.** This is a hard gate,
  not a style preference: a page is two rows of four, and a graphic can span
  neither the gap between them nor a hole in the middle of a run. A group whose
  roles straddle the row boundary — or sit on different pages — is not drawn at
  all, and its members fall back to plain controls. Since knob order comes from
  `ui_hierarchy`, declaring a group can force you to reorder that level's knobs
  so the roles sit together. `validate.mjs` reports the failure as
  `viz-declared-not-adjacent`.
- **A group is only as good as its roles.** Do not group unrelated params to get
  a picture — an envelope drawn over four params that are not an envelope is
  worse than four honest knobs.

#### What happens when you declare nothing

Resolution order is: the module's own `chain_params` `viz` → a host override →
the detectors → nothing. A module always outranks the host, and both outrank a
detector.

The detectors run in risk order — envelope, filter, LFO, waveform, fader,
switch, EQ, sample — and each key the earlier ones claim is off the table for
the rest. None of them fire on a name alone; every one corroborates against
declared metadata. An envelope needs 2–4 numeric roles that share a key stem
(`f_attack`/`f_decay` group; `amp_attack`/`filter_decay` do not), a filter needs
both cutoff and resonance, a fader rejects a bipolar range as a pan or trim
rather than a level, and an EQ band gain must be bipolar and roughly symmetric.
All of them still require the contiguous-row run above.

A param no detector claims gets an ordinary knob dial — the same cell it would
have had before graphics existed. That is the default, and for most params it is
the right one: leaving something undeclared is a real choice, not a gap to fill.
Prefer it over inventing a group, and use `viz: false` when a detector keeps
claiming something it should not.

One page-wide exception: a graphic needs a row at least 14 px tall and a cell at
least 26 px wide. A page that cannot give it that drops to plain dials
*entirely*, rather than clipping pictures or mixing them with dials.

Check what a module declares, and what is being guessed on its behalf, with:

```bash
node tools/param-pages/validate.mjs <module-id>
```

`viz-inferred` findings are the detector reporting a guess it made — each one is
a group you could confirm or correct by declaring it.

### Child Selectors (for repeated elements)

For synths with multiple similar elements (tones, operators, parts), use child selectors:

```json
{
  "levels": {
    "tones": {
      "name": "Tones",
      "child_prefix": "nvram_tone_",
      "child_count": 4,
      "child_label": "Tone",
      "params": [
        {"key": "cutofffrequency", "name": "Cutoff"},
        {"key": "resonance", "name": "Resonance"},
        {"key": "level", "name": "Level"}
      ],
      "knobs": ["cutofffrequency", "resonance", "level"]
    }
  }
}
```

The Shadow UI will show a selector (Tone 1, Tone 2, etc.) and prefix parameter keys with `child_prefix` + index (e.g., `synth:nvram_tone_0_cutofffrequency`).

#### Custom key shapes

`child_prefix` assumes keys look like `<prefix><index>_<key>`, zero-based and
unpadded. Many modules — drum modules especially — use a different shape, and
without a way to declare it they end up listing an *alias* (`pad_vol`, meaning
"the focused pad") and leaving the concrete keys (`p01_vol` … `p16_vol`)
declared in `chain_params` but listed in no level. Those params are then
unreachable from any UI: fleet-wide that is the single largest source of
unreachable parameters.

These optional fields declare the real shape instead:

| Field | Purpose | Default |
|-------|---------|---------|
| `child_key_template` | Key pattern, with `{index}` and `{key}` placeholders | `<child_prefix>{index}_{key}` |
| `child_index_base` | First instance number — pads are usually 1..16 | `0` |
| `child_index_digits` | Zero-pad the index to this width (`p01_` not `p1_`) | none |
| `child_key_overrides` | Per-key template overrides, for the odd key that breaks the pattern | none |
| `child_index_param` | A param through which **your module** owns which instance is focused | none (UI-local) |

```json
"pad_settings": {
  "name": "Pad",
  "child_count": 16,
  "child_label": "Pad",
  "child_key_template": "p{index}_{key}",
  "child_index_base": 1,
  "child_index_digits": 2,
  "child_key_overrides": { "fx1": "v{index}_{key}" },
  "knobs": ["vol", "pan", "tune", "decay"]
}
```

That level declares four params and the host multiplies them into 64 real keys
(`p01_vol` … `p16_decay`), each one addressable, automatable and reachable from
the UI — with no per-module configuration file anywhere.

`child_prefix` continues to mean exactly what it always did, so existing
declarations are unaffected.

#### `child_index_param` — when the MODULE owns the focus

By default the focused instance is UI state, changed only by picking from the
instance list. That is right for a synth, where "Part 2" is a deliberate
navigation choice, and wrong for a drum module, where **hitting a pad** is how
you choose what you are editing.

Declare `child_index_param` and the param becomes the single source of truth in
both directions: the UI reads it and follows, and picking from the list writes
it. So a module that moves the focus itself and a user who picks from the list
can never disagree — they are the same write.

```json
"pad_settings": {
  "child_count": 16, "child_label": "Pad",
  "child_key_template": "p{index}_{key}",
  "child_index_base": 1, "child_index_digits": 2,
  "child_index_param": "focused_pad",
  "knobs": ["vol", "pan", "tune", "start"]
}
```

The value is the instance number **in your own numbering** — with
`child_index_base: 1`, pad 1 is `"1"`. Serve it from `get_param` and accept it
in `set_param`.

Two things the host guarantees, which you can rely on:

- **A read that does not answer never moves the focus.** An empty, non-numeric
  or out-of-range value is ignored rather than treated as instance 0 — moving
  the user off the pad they were editing because a read timed out would re-key
  every page on screen.
- **It costs no extra IPC.** The read shares a rotation stop with the preset
  name, and a level that does not declare it reads nothing at all.

Without this, adding a child level to a module that already follows the played
pad would *cost* that behaviour — the grid would sit on the instance the picker
last chose. With it, the declaration is purely additive.

### Example: Chord MIDI FX Hierarchy

```json
{
  "levels": {
    "root": {
      "name": "Chord",
      "params": [
        {"key": "type", "name": "Type", "type": "enum",
         "options": ["none", "major", "minor", "dim", "aug", "sus2", "sus4", "power", "octave"]},
        {"key": "inversion", "name": "Inversion", "type": "enum",
         "options": ["root", "1st", "2nd", "3rd"]},
        {"key": "strum", "name": "Strum", "type": "int", "min": 0, "max": 100}
      ],
      "knobs": ["type", "inversion", "strum"]
    }
  }
}
```

## Chain Parameters (Knob Mappings)

Modules can expose quick-access knob mappings via `chain_params`. This can be defined statically in `module.json` or dynamically via `get_param("chain_params")`.

### Static Definition (module.json)

For modules with fixed parameters, define in capabilities:

```json
{
  "capabilities": {
    "chain_params": [
      {
        "key": "cutoff",
        "name": "Cutoff",
        "type": "int",
        "min": 0,
        "max": 127,
        "default": 64
      },
      {
        "key": "type",
        "name": "Filter Type",
        "type": "enum",
        "options": ["lowpass", "highpass", "bandpass"],
        "default": "lowpass"
      }
    ]
  }
}
```

### Dynamic Definition (get_param)

For modules with state-dependent parameters:

```c
int get_param(void *instance, const char *key, char *buf, int buf_len) {
    if (strcmp(key, "chain_params") == 0) {
        const char *json = "["
            "{\"key\": \"cutoff\", \"name\": \"Cutoff\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"value\": 64},"
            "{\"key\": \"resonance\", \"name\": \"Resonance\", \"type\": \"int\", \"min\": 0, \"max\": 127, \"value\": 32}"
        "]";
        strncpy(buf, json, buf_len);
        return strlen(json);
    }
    return -1;
}
```

#### Pattern: a knob whose options depend on another selection

*"Select a folder, then turn a pot through the wavetables in it."* This works
today; nothing needs adding to the host. Reported as impossible by a module
author, so it is written down here.

**Why a `filepath` cannot do it.** `type: "filepath"` is **opaque**: the grid can
*open* it but never *turn* it, on purpose — turning would write nonsense into a
path. A knob asking to drive a filepath is asking for a control the host has
classified as un-turnable. That is usually the whole reason this looks impossible.

**The shape that works:**

1. Declare the dependent control as `type: "enum"` with an `options` array
   holding the current folder's entries. It becomes turnable *and* divable —
   hold the knob and click opens a scrolling picker, which is what you want past
   a handful of entries.
2. Serve `chain_params` from `get_param` (dynamic, above) rather than a static
   string, so the option list can change.
3. Offer the folder as its own level with `items_param` / `select_param`.

**The host re-reads your contract when the user chooses.** Committing an items
selection arms a settle deadline; once your answer stops changing, the host
re-reads `chain_params` / `ui_hierarchy` and re-plans the pages, so the knob
steps the new folder's list. It is throttled — the deadline re-arms per detent
and two agreeing readings are required — so spinning a folder list costs about
five contract reads in total, not one per step.

**The obligation: do not scan the filesystem to answer.** `get_param` and
`set_param` are the SPI audio callback (see the threading section). Scan on your
own `SCHED_OTHER` worker when the folder changes, publish the result by pointer
swap, and let `get_param` only *format* an already-cached list. A module that
scans a directory inside `get_param` pays that cost **once per repaint**.

**Limits to design against:**

| Limit | Value | What happens past it |
|---|---|---|
| `MAX_ENUM_OPTIONS` (`chain_internal.h`) | 128 | The knob grid still lists them (JS parses the JSON itself), but the chain host's C-side knob-mapping and modulation tables truncate — the picker works while CC mapping quietly does not |
| `chain_params` string | 64 KB (`SHADOW_PARAM_VALUE_LEN`) | The read fails; see the three-answers rule |

Keep a folder under 128 entries, or paginate it into sub-levels.

And the enum wire rule still applies: index in, index out — or names both ways
with `options_as_string: true`. A module that already resolves an index into its
scan list should report the index and declare nothing.

### Parameter Types

Use the canonical type list in `Shadow UI Parameter Hierarchy -> Parameter Types`.
For `chain_params`, the same types apply, and `default`/`value` can be used to provide initial values.

#### `filepath` in module.json

Use `type: "filepath"` in `capabilities.chain_params` to let Shadow UI open a reusable file browser.

```json
{
  "capabilities": {
    "chain_params": [
      {
        "key": "sample_file",
        "name": "Sample File",
        "type": "filepath",
        "root": "/data/UserData",
        "start_path": "/data/UserData/UserLibrary/Samples",
        "filter": ".wav",
        "default": ""
      }
    ]
  }
}
```

`filepath` fields:

- `key` (required): Parameter key passed to `set_param`.
- `name` (required): Label shown in Shadow UI.
- `type` (required): Must be `"filepath"`.
- `root` (optional, recommended): Absolute folder where browsing starts and is constrained.
- `start_path` (optional): Absolute folder or file path used as the initial location when current value is empty.
- `filter` (optional): File extension filter as a string or array, for example `".wav"` or `[".wav", ".aif"]`.
- `live_preview` (optional): When true, moving the file-browser cursor over files temporarily sets the parameter to that file until the user confirms or cancels.
- `browser_hooks` (optional): Event hooks to run additional parameter writes at browser lifecycle points. Supported keys: `on_open`, `on_preview`, `on_cancel`, `on_commit`.
- `default` or `value` (optional): Initial absolute path. If the path exists and is inside `root`, the browser opens to the parent folder and highlights the file.

Behavior notes:

- Selected files are stored as absolute paths.
- Initial browser location priority is: current/default value, then `start_path`, then `root`.
- If the chosen start location is missing, invalid, or outside `root`, the browser falls back to `root`.
- With `live_preview: true`, preview changes are temporary: Back cancels and restores the original value, Click commits the highlighted file.
- `browser_hooks` action format is `{ "key": "<param>", "value": "<string>", "restore": true|false }`. Non-prefixed keys are resolved against the active component prefix.
- `browser_hooks` supports value placeholders: `$path`/`$selected_path` and `$filename`/`$selected_filename`.
- For pad samplers, you can suspend auto-pad switching while browsing by adding `{"key":"ui_auto_select_pad","value":"off","restore":true}` to `browser_hooks.on_open`.
- Example user sample file path: `/data/UserData/UserLibrary/Samples/Drums/Kick01.wav`.

#### `wav_position` in module.json

Use `type: "wav_position"` for numeric position/trim controls with waveform visualization in edit mode.

`wav_position` fields:

- `key` (required): Parameter key passed to `set_param`.
- `name` (required): Label shown in Shadow UI.
- `type` (required): Must be `"wav_position"`.
- `display_unit` (optional): `percent`, `ms`, `sec`/`s` (default `percent`). In `percent` mode values are stored internally as normalized `0..1` and displayed as `0..100%`.
- `mode` (optional): `position`, `start`, `end` (legacy aliases: `trim_front`, `trim_end`).
- `filepath_param` (recommended): Key of the linked filepath parameter containing the WAV source.
- `min`, `max`, `step` (optional): Numeric range and increment for editing. Defaults: `percent` uses `0..1` with `step: 0.01`; `ms` uses `step: 1`; `sec` uses `step: 0.01`.
- `shift_increment_multiplier` (optional): Multiplier for Shift fine-step (default `0.1`; alias `shift_step_multiplier`).

Behavior notes:

- Waveform view opens only while the parameter is in edit mode.
- `mode: start` and `mode: end` use side-aware waveform rendering for trim workflows.
- On filepath selection commit, empty linked `mode: end` params are initialized to file end (`1.0` internal / `100%` displayed in percent mode, WAV duration for `ms`/`sec`).

#### `canvas` in module.json

Use `type: "canvas"` to open a module-defined fullscreen canvas UI from the hierarchy editor.

`canvas` fields:

- `key` (required): Parameter key passed to `set_param`.
- `name` (required): Label shown in Shadow UI.
- `type` (required): Must be `"canvas"`.
- `display_value_type` (optional): `string`, `int`, `float`, or `percent` formatting for value display.
- `canvas_script` (optional): Script path relative to module root (default `canvas.js`), supports `file.js#overlay_name`.
- `canvas_overlay` (optional): Named overlay object selector (aliases: `canvas_target`, `overlay`).
- `show_footer` (optional): Show/hide footer in canvas view (default `true`; alias `showfooter`).
- `show_value` (optional): Show/hide parameter value in hierarchy and canvas footer (default `true`; alias `showvalue`).

Behavior notes:

- Clicking the parameter enters a dedicated fullscreen canvas view.
- Set `show_value: false` for button-style canvas entries that should not show a value.
- The loaded script should expose `globalThis.canvas_overlay` (or `globalThis.canvas_overlays`) with hooks such as `onOpen`, `onMidi`, `tick`, `draw`, `onClose`, `onExit`.

#### Dynamic Target Pickers

Use `module_picker` and `parameter_picker` for chain-aware target routing without custom UI code.

- `module_picker`: Renders as a normal enum editor whose options are refreshed from currently loaded chain components.
- `parameter_picker`: Renders as a normal enum editor whose options are refreshed from the component selected by `target_key`.
- `allow_none` (optional, default true): Includes an empty option for clearing assignment.
- `allow_self` (module_picker only, optional, default false): Allows selecting the hosting component itself.
- `allowed_targets` (module_picker only, optional): Comma-separated string or array of target IDs to whitelist.
- `param_key` (module_picker, optional): Companion parameter key to clear when target changes (for example `lfo1_target_param`).
- `target_key` (parameter_picker, recommended): Companion key holding selected module target.
- `numeric_only` (parameter_picker, optional, default true): Restricts options to float/int parameters.

These map to knobs 1-8 in the Shadow UI for quick access.

#### Additional Type Examples

```json
{
  "capabilities": {
    "chain_params": [
      { "key": "root_note", "name": "Root", "type": "note", "mode": "multi", "min_note": 24, "max_note": 96 },
      { "key": "lfo_rate", "name": "Rate", "type": "rate", "include_bars": true, "bars_mode": "bars-every", "include_triplets": true },
      { "key": "sample_file", "name": "Sample", "type": "filepath", "root": "/data/UserData/UserLibrary/Samples", "filter": [".wav", ".aif"] },
      { "key": "start_ms", "name": "Start", "type": "wav_position", "display_unit": "ms", "mode": "start", "filepath_param": "sample_file", "min": 0, "max": 5000, "step": 1, "shift_increment_multiplier": 0.05 },
      { "key": "label", "name": "Label", "type": "string", "default": "Init" },
      { "key": "draw", "name": "Draw", "type": "canvas", "display_value_type": "percent", "canvas_script": "canvas.js#draw_overlay", "canvas_overlay": "draw_overlay", "show_footer": false }
    ]
  }
}
```

```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "params": [
          { "key": "sync", "name": "Sync", "type": "enum", "options": ["Off", "On"] },
          { "key": "lfo_rate", "name": "Rate", "visible_if": { "param": "sync", "equals": "On" } },
          { "level": "advanced", "label": "Advanced" }
        ]
      },
      "advanced": {
        "visible_if": { "param": "sync", "truthy": true },
        "params": [
          { "key": "label", "name": "Label", "type": "string" }
        ]
      }
    }
  }
}
```

### `access` — which direction a param means something in

Optional on a `chain_params` entry. Defaults to `readwrite`; declare it when
your parameter is not both.

| value | meaning | host behaviour |
|---|---|---|
| `readwrite` | an ordinary control (default) | turnable, divable if it has options |
| `read` | a **readout** — the value means something, writing means nothing | never turnable, never opens a picker, still refreshed on screen, and drawn inside a **dotted frame** |
| `write` | a **trigger** — writing does something, the value means nothing | never turnable, a click **fires** it |

```json
{"key": "detected_key", "type": "enum", "options": ["C","C#","D"], "access": "read"}
{"key": "rnd_preset",   "type": "enum", "options": ["—","Rnd!"],   "access": "write"}
```

**Why a readout needs saying.** `keydetect`'s `detected_key` is 25 key names
with no `set_param` branch at all — deliberately, and documented as such back
when an enum could only be nudged one detent at a time. Enums became divable in
1.0, so the picker opened on it and silently discarded whatever you chose. That
was a gap in the contract, not a bug in the module: display-only was never
expressible.

The cell says so too: a readout is **dotted** — the enum square draws its own
frame dotted, a dial or a big number gains a dotted frame around the cell. See
*Which widget a cell draws* above.

**Why a trigger needs saying, and why it is the more urgent half.** A momentary
action modelled as a two-option enum is a live hazard. `euclidrum`'s
`rnd_preset` declares `["—","Rnd!"]` and fires on anything that is not the
em-dash — so an **index** write of `"0"`, which *means* the em-dash, "do
nothing", randomises all eight lanes and destroys the kit. Declaring
`access: "write"` makes the host fire it through your own enum wire (the
**name**, if that is what your `get_param` reports) and never scrub it with a
knob, so the "do nothing" option can never be written by accident.

A trigger is not the same as a **switch**. `["Off","On"]` is a two-state
*setting* — it has a value worth reading, it should be turnable, and it draws as
a switch. Leave those as ordinary enums; roughly 150 params in the fleet are
switches and only about ten are triggers.

### `max_param` is NOT supported — publish a real `max`

Eight modules declare `"max_param": "preset_count"` (or similar) hoping the host
will resolve the bound at runtime. **Nothing has ever consumed it.** It is
parsed and then read by no one, in C or in JS.

Until 2026-08 it also *damaged* the parameter it decorated: the parser set an
internal `max_val = -1` marker that the chain host serialised literally, so sf2
shipped `{"min":0,"max":-1}` — an inverted range — and every other user lost its
declared bound. That marker is gone; a `max_param` declaration is now recorded
and otherwise inert, and your declared `max` survives.

It is deliberately not implemented, because the two real uses disagree about
what the referenced key means:

```
preset        max_param="preset_count"   wants count - 1   (7 modules)
laneN_pulses  max_param="laneN_steps"    wants the value   (eucalypso)
```

Picking one convention would silently be wrong for the other, which is the exact
failure this field already caused once.

**Do this instead:** emit a real `max`. Every module using `max_param` builds its
`chain_params` string at runtime from `get_param`, so it already knows the number
at the moment it serialises:

```c
/* not: "min":0,"max_param":"preset_count"   */
offset += snprintf(buf + offset, buf_len - offset,
    "{\"key\":\"preset\",\"type\":\"int\",\"min\":0,\"max\":%d}",
    preset_count > 0 ? preset_count - 1 : 0);
```

A knob declared `0..9999` against 27 real presets is ~99.7% dead travel, and
that shape is currently in `hera`, `surge`, `moog`, `minijv`, `helm`, `nusaw`,
`rex` and `obxd`.

### Recognized Units

The shared shadow-UI formatter (`src/shared/param_format.mjs`) renders any
`chain_params` entry that declares a `unit` field consistently across modules.
Recognized units:

| Unit  | Behavior                                                 | Example display     |
|-------|----------------------------------------------------------|---------------------|
| `dB`  | Signed, decimals from `step` (fallback 1)                | `-6.0 dB`           |
| `Hz`  | Auto-scales to kHz at >= 1000                            | `440 Hz`, `1.50 kHz`|
| `ms`  | Non-negative, decimals from `step` (fallback 1)          | `12.5 ms`           |
| `sec` | Non-negative, decimals from `step` (fallback 3)          | `1.234 sec`         |
| `%`   | Values in `0..1` are scaled ×100; values >1 shown raw    | `50%`               |
| `st`  | Semitones; signed integer with explicit `+` prefix       | `+7 st`, `-3 st`    |
| `BPM` | Integer                                                  | `120 BPM`           |

Other unit strings are appended verbatim with a leading space (e.g. `unit:"cents"` → `"200 cents"`).

`display_format` (printf-style `.Nf` or `.N%`) overrides decimal selection but
still appends the unit suffix when present (e.g. `display_format:"%.2f"` with
`unit:"Hz"` → `"440.00 Hz"`).

For raw `set_param` writes the wire value never includes the unit suffix —
just the numeric string, with at least 3 decimals for floats.

#### `%` units and `max`

When using `unit:"%"`, **declare `max` explicitly** so the formatter knows
whether your raw values are normalized (`0..1`) or already in display range
(`0..100`). Without `max`, the default is `1` and the formatter scales by
×100 — a module that stores percentages as `0..100` and forgets `max:100`
will see `50` displayed as `5000%`.

#### Enum wire format

An enum can go over the `set_param` wire as its numeric INDEX or as its option
NAME. The UI **auto-detects which your plugin speaks** and latches the answer
per key: it looks at what `get_param` reports — a value that matches a declared
option means names, a value that parses as a number means indices, anything
else teaches nothing and leaves the question open for the next read.
Detection only ever uses values that came from the DSP, never values the UI
itself wrote (`learnEnumWireFormat` in `src/shared/param_format.mjs`); the
option picker asks the same question through `enumWireValue`.

`options_as_string: true` on the chain_params entry is therefore an **override,
not a requirement** — it is checked first and never learned over. Declare it if
you want the convention nailed down regardless of what your `get_param` says.

**The one obligation: a `set_param` that accepts only NAMES must report NAMES
from `get_param`.** That read is the only signal detection has. Report an index
while accepting only names and every write is discarded in silence — which is
exactly what the built-in `chord` module did: its `set_param` is a `strcmp`
ladder over the option names with **no trailing `else`**, so the value never
moved while the UI drew the index it had invented.

Two `set_param` shapes worth avoiding, both found in the fleet, neither of which
reports an error:

- **No trailing `else`** (chord) — an unrecognised value is silently ignored.
- **`else atof(val)`** (arp's `division`) — an unrecognised value is silently
  *accepted*, turning index `3` into a division of `3.0`.

Prefer accepting both conventions and rejecting anything else loudly enough to
show up in a log.

<!-- BEGIN generated widgets. Written by tools/param-pages/widget_sheet.mjs
     from the same code the device draws with. Do not hand-edit between these
     markers; regenerate instead. -->

#### Which widget a cell draws

Authors do not pick a widget. Declare `type`, a range and `options`; the
widget follows. `drawKnobWidget` (`render_page_movy.mjs`) is one ordered
dispatch, and the order is the specification — each branch owns its cell
outright:

| # | test | widget | |
|---|---|---|---|
| 1 | `kind === KIND_OPAQUE` | opaque box | ![opaque-box](images/widgets/opaque-box.png) |
| 2 | `writeOnly` (a trigger) | button | ![button](images/widgets/button.png) |
| 3 | `kind === KIND_ENUM` | enum square | ![enum-square](images/widgets/enum-square.png) |
| 4 | `shouldDrawBigNumber` | big number | ![big-number](images/widgets/big-number.png) |
| 5 | *(otherwise)* | arc knob | ![arc-knob](images/widgets/arc-knob.png) |

A **viz graphic** pre-empts all of it: a resolved group covers its cells and
draws one picture across them, and the per-cell widget is skipped.

Notes worth having before you declare something:

- **The opaque box shows three states** — a value, `NONE` for `""`, and
  `--` for a read that has not answered. Do not collapse the last two; an
  empty slot and a slow one are different facts.
- **A trigger is `access: "write"` on an ordinary enum**, not a type. The cap
  carries no text: the module reports a constant idle spelling and the fleet
  proves it is not readable (euclidrum's is an em-dash the 5x7 atlas cannot
  draw, which rendered as a blank square). The cell's label names the action.
  Fired by a jog click *or* a knob detent, either direction — the footer says
  `CLK FIRE` and `KNB FIRE`, one verb because it is one action. The knob
  path LATCHES: a whole spin is one fire, and the latch clears on RELEASE, or
  after `TRIGGER_KNOB_GESTURE_GAP_MS` of stillness if the cap sensor never
  registered. A rate limit was tried first and still fired eight times across
  a two-second spin.
- **`short_options` is for the enum square only.** The held-knob header keeps
  the full spelling, which is where a value has room to be read.
- **The big-number span bound is load-bearing.** An earlier version bounded at
  128 and drew 1392 params big, including `volume [0..100]` — a sweep, where
  an arc is the honest picture.
- **A modulated knob keeps the pointer on the base and adds a dot** at the live
  value. The dot is drawn even when they coincide: suppressing it there made a
  modulated knob pixel-identical to an unmodulated one.

  ![arc-knob-modulated](images/widgets/arc-knob-modulated.png)

#### Viz graphics

Detection runs in a fixed priority order and each detector gets first refusal
on unclaimed keys. A graphic must be **contiguous and within one row** — it
cannot span the label band between row 0 and row 1.

| graphic | from | |
|---|---|---|
| envelope | adjacent attack/decay/sustain/release | ![viz-envelope](images/widgets/viz-envelope.png) |
| filter | cutoff + resonance (mode/slope optional) | ![viz-filter](images/widgets/viz-filter.png) |
| lfo | shape + rate + depth, sharing a stem | ![viz-lfo](images/widgets/viz-lfo.png) |
| eq | bipolar, roughly symmetric band gains | ![viz-eq](images/widgets/viz-eq.png) |
| waveform | one oscillator-shape enum | ![viz-waveform](images/widgets/viz-waveform.png) |
| fader | a level | ![viz-fader](images/widgets/viz-fader.png) |
| switch | `enum` Off/On **or** `int` 0..1 | ![viz-switch](images/widgets/viz-switch.png) |
| sample | a file plus positions within it | ![viz-sample](images/widgets/viz-sample.png) |

- **An optional role is dropped when it does not fit.** `detectFilter` used to
  require every role it found to be contiguous, so a Mode knob parked at the far
  end of the page deleted the corroborated cutoff/resonance pair.
- **The switch takes `int` 0..1 as well as an Off/On enum** — 61 params across
  11 modules spell it that way and drew as a number, which is the one widget
  that tells you nothing. It draws both states, which is why it never raises the
  option-list peek.
- **The sample's file does not claim a cell.** It is `roles.value` — the
  waveform is drawn *from* it, never *on* it — because it dives to the file
  browser while every other member dives to the wave editor. With no file the
  graphic is not drawn at all and the cells fall back to their own widgets.
- **There is no representative shape.** A read that did not answer must never
  become a picture; the synthetic waveform that used to fill in for missing
  peaks drew a sample that was never loaded.

#### The marks

![brackets](images/widgets/brackets.png)

Corner brackets mean **the knob works, and it also opens something** —
`alsoOpens(meta)`, which in practice is a ranged `wav_position`. A viz group
wears one across its whole span when any covered cell `opensOnClick`.

See *Divability, and the two cell marks* below for why the brackets and the
chevron are not two spellings of one idea.

![readout](images/widgets/readout.png)

A **dotted frame** means `access: "read"` — telemetry you can look at and not
change. Each pair above is the same declaration with and without it.

The input layer has always honoured `access`: turning a readout shows the
reading and writes nothing, a click opens no picker, and `isDivable` /
`isTurnable` exclude it. The *drawing* did not, so a readout was
pixel-identical to a control — reported from the device as a knob that "does
not seem to do anything", which was a correct reading of the picture.

- **The rule is *a readout is dotted*; where the stroke lives is the widget's
  business.** A dial and a big number have none, so a frame is added around the
  cell; the enum square has one, so it dots that. One dotted rectangle per cell,
  never two. Either way the value does not move.
- **The square dots its own stroke because an outer frame did not work there.**
  Measured against an identical editable twin, an outer frame differed by 17
  pixels at full width against 27 at the narrow one — the wider the value, the
  more of the mark the square absorbed — and side by side the two cells were
  indistinguishable. It failed exactly where the feature is for: `keydetect`'s
  values are musical keys, always full width. Dotting the stroke inverts the
  gradient, to 39 against 26.
- **Not an inverted slab** — inversion already means *a finger is on this knob*
  in the label band and *this is the selection* in a list. **Not corner
  brackets** — those mean the opposite claim, that the knob works *and* opens
  something.
- **An opaque cell is not marked at all.** Its own notched frame is on the same
  rect, so an outer frame's dots would show only in the chevron's cut, and
  dotting that frame would blunt the one widget that says which direction its
  door goes.
- **A readout inside a viz graphic is not framed.** No fleet module has one;
  if one appears, mark the span once, the way the door bracket does.

#### Chrome

| | |
|---|---|
| ![chrome-header](images/widgets/chrome-header.png) | **Header** — where you are, and which page. The right side is a measured share against a `HEADER_MIN_LEFT` floor, not a fixed column. |
| ![chrome-header-held](images/widgets/chrome-header-held.png) | Holding a knob inverts the band and shows that parameter's full name and value. One clear row above and below is load-bearing *only* when inverted. |
| ![chrome-bank-bar](images/widgets/chrome-bank-bar.png) | **Bank bar** — one tick per page. It owns row 7, which is why a menu page cannot start its list at y=9 the way the enum picker does. |
| ![chrome-footer](images/widgets/chrome-footer.png) | **Footer** — hint pairs, key inverted into a pill. Fit-aware: three pairs need every word ≤4 chars, and a longer one drops a pair rather than overflowing. Hints come from the caller, never the renderer. |
| ![chrome-label-cell](images/widgets/chrome-label-cell.png) | **Label cell** — name at rest, value while held, `~` while modulated. Budgeted in *characters*, not pixels. |
| ![chrome-list](images/widgets/chrome-list.png) | **List** — one dotted column with a solid thumb, in `drawMenuList`, so every list in the tree has it. 2px thumb floor; the track covers the rows, not the rect; the selection highlight stops short of the gutter or it draws a phantom second thumb. |

#### Motion

Time is passed **in**, never read — there is no `Date.now()` in the renderer,
which is what lets a page be filmed deterministically.

**The store must be passed from the controller.** Every widget guards on
`anim && typeof nowMs === "number"`, so an undefined store draws the settled
frame forever — silently, and identically to a correct render of a value that
is not moving. `createAnimState` was written, exported, unit-tested and never
*called*; every animation below shipped inert for months.

*Slowed 5x — sampled at real time, so the curve is the device's.*

| | |
|---|---|
| ![motion-waveform](images/widgets/motion-waveform.gif) | **Waveform**, 100ms — one shape bends into the next. The enum peek is instant and covers this while it plays. |
| ![motion-enum](images/widgets/motion-enum.gif) | **Enum square**, 120ms — the frame travels, the glyphs swap outright. Text is served short while the box is narrow and completes as it arrives. |
| ![motion-button](images/widgets/motion-button.gif) | **Trigger**, 300ms — press then rings. Bursts append rather than replace, so a double-tap throws two. |

<!-- END generated widgets -->

### `short_name` — a cell label that differs from the full name

The cell is five characters wide; the header is the width of the screen. They
want different words, so declare both:

```json
{ "key": "osc1_pitch", "name": "Osc 1 Pitch", "short_name": "Pitch" }
```

The cell draws `PITCH` where it drew `OSC1PIT`. The held-knob header and the
list keep `Osc 1 Pitch`, which is what tells you *which* oscillator you are
holding. Optional, and inert when absent — this is the same split
`short_options` already makes for enum values.

**Do not repeat the page in the cell.** A page called *Filter Envelope* has
already said "filter envelope", so its knobs are `Attack` and `Decay` — not
`F.Atk` or `FENVAT`.

Measured across the 39 catalogued modules by this author: 1766 controls on
knob pages, 1150 of them squeezed past being words, and **500 whose name
merely repeats their own page**. 425 of those stop being squeezed with a
`short_name` alone, with no change to what the header says.

It is still a label, so it is still fitted to the cell — a long `short_name`
is not a way to smuggle six characters into five.

**Drawn as you typed it, when it fits.** A `short_name` of `Noise` draws
`NOISE`, not the `NSE` the abbreviation table would pick, and `Amt` stays
`AMT` rather than being expanded back to `AMOUNT`. Only when what you typed is
too wide does the table get a say (`Sustain` → `SUS`), and only then the
squeeze. You are the author; the grid does not second-guess a word that fits.

##### A page can override it

`short_name` belongs to the parameter, and the same parameter can sit on more
than one page. `env_amount` on an *Envelope* page wants `Amt`; on a *Main*
page beside an LFO Amt, `Amt` would name them both. So a level may carry its
own, in the inline entry it already uses to name its params:

```json
"levels": {
  "envelope": { "params": [ { "key": "env_amount", "short_name": "Amt" } ] }
}
```

The level's value wins on its own page; the parameter's applies everywhere
else.

##### Four questions, in order

These came out of naming ~1100 controls across 39 modules. Each is a word you
can delete because something on screen already says it.

1. **Does the page say it?** A page titled *VCF* has said "filter", so
   `VCF Cutoff` is just `Cutoff`. A page titled *Filter Envelope* has said
   both, so its knobs are `Attack`, `Decay`, `Sustain`, `Release`.

2. **Does the widget say it?** A fader *is* a level — `Output Level` under a
   fader is just `Output`, and `Noise Volume` is `Noise`. A switch is an
   enable; an LFO graphic is an LFO; a waveform is a shape.

3. **Does the word imply its own domain?** Cutoff and resonance are a
   filter's; attack, decay, sustain and release are an envelope's. So
   `Filter 1 Cutoff` is `Cutoff 1` — the index survives and moves to the end,
   because it is the only part that distinguishes it.

4. **Do the neighbours say it?** If every cell on the page carries the same
   word, that word is context, and the one that differs should lead.

##### Two things not to do

**Never reduce to a bare index.** `Volume 1` under a fader is *not* `1`. Four
cells reading `1 2 3 4` beneath four identical faders are worse than
`VOL 1`. A word is only redundant when something else in the label still
carries meaning.

**Never make two cells on a page draw the same thing.** Check the whole page,
not the one control — and check every page the parameter appears on. An ugly
long label beats an ambiguous short one. `obxd` shipped two cells both
reading `OCTAVE`, because it names `octave` and `octave_transpose` alike.

##### When a word must shrink anyway

Take the **front** of it: `Compression` → `Comp`, `Panorama` → `Pano`,
`Algorithm` → `Algo`. A leading prefix is what a reader recognises, where a
devowelled skeleton (`CMPRS`, `PANRM`, `ALGRTH`) has to be decoded. The
exception is a prefix that reads as a different word — `Scatter` → `SCAT` and
`Restart` → `REST` mislead, so those devowel instead: `Scttr`, `Rstrt`.

For **two** words, devowelling wins and prefixing does not: `Feedback Tone` is
better as `FBTONE` than `FEETON`. Cut the words, not the label.


#### Divability, and the two cell marks

Every enum with a non-empty `options` array is **divable**: on the knob grid,
holding its knob and clicking opens a scrolling option list. You get this for
free — there is nothing to declare, and nothing to declare it away.

**A two-option param is turned differently, too, and how it is DRAWN decides
how it turns.**

- A param drawn as a **switch** — `Off`/`On`, or an `int` 0..1 — is
  **direction-absolute**: clockwise is on, anticlockwise is off. The switch has
  a track with its knob at one end, so the picture already tells you which way
  is which. Turning an already-on switch clockwise is a no-op, not a flip, and
  there is no gesture latch because the write is idempotent.
- A param drawn as the **enum square** — `Mix`/`Reverb`, `Saw`/`Square` — is a
  boxed value: both options sit in the same place, so it shows a state and names
  no direction. A detent **toggles** it whichever way you turned, and one flick
  of the encoder is one flip, not a dozen.

You do not choose between these; the widget rule does, and it follows your
`options`. Three or more options keep the four-detent gate and clamp at the
ends. A trigger (`access: "write"`) is never toggled and never draws as a
switch; it fires.

**Except at exactly two options, where there is no list to open.** On the knob
grid the click FLIPS it — the picker would show the value already in the cell
and the one other value there is — and the footer reads `CLK FLIP` rather than
`CLK OPEN`. The knob still steps it.

In the **list** view the same parameter is FOCUSED instead: click puts the row
into edit mode and the jog steps it, exactly like a float row. The flip only
saves a gesture when a knob is already under your hand, and in a list none is,
so flipping there would leave one row with no focus state while every other row
has one.

This is a different line from the `switch` distinction two sections up, and
they do not have to agree: a switch is a **boolean-flavoured** two-option enum
and only that group loses the peek, whereas *every* two-option enum flips —
`Mix`/`Reverb` included. The peek exists to show a word the cell has no room
for; the flip exists to save a gesture, and a choice pays that gesture exactly
as a boolean does.

Triggers (`access: "write"`) and readouts (`access: "read"`) are not divable at
all, so neither flips: a trigger is a two-option enum on the wire, and firing
it is not the same as setting it.

**The cell marks do not mean "divable."** Measured over the fleet (2026-08):
967 divable cells on knob pages, and **953 of them — 99% — wear no mark at
all**, because almost every divable cell is an enum. Divability is announced by
the **footer**: hold the knob and it reads `CLK OPEN`. Marking every enum would
erase what a mark means.

The two marks you *will* see distinguish something narrower, with no overlap
anywhere in the fleet — a handful of cells each:

| mark | knob turns it? | means |
|---|---|---|
| corner brackets | always | the knob works, **and** it opens something |
| chevron box | never | there is no knob here — only a door |

The **chevron is not a mark at all**: it is the *widget*. An opaque param
(`filepath`/`file`, `string`, `canvas`, non-ranged `wav_position`, the two
picker types) has no value-shape to draw, so `drawOpaqueBox`'s notched frame
with a chevron in its broken edge is simply what that cell looks like.

The **brackets** are an annotation on a working widget, and in practice mean
one thing: a **ranged `wav_position`** — a number a knob turns perfectly well
that *also* has a waveform editor behind it. The predicate is `alsoOpens()` in
`param_meta.mjs`; the underlying declaration fact is `meta.opaque_type`.

Module authors influence all of this only through `type`, `options`, and
whether a `wav_position` declares `min`/`max`.

### Knob Acceleration

All chain / master-FX / slot / patch param knob edits share one acceleration
curve from `src/shared/knob_engine.mjs`. Modules don't opt in — every numeric
adjust path runs through the same engine.

| Time since last tick | Step divisor          |
|----------------------|-----------------------|
| First tick (cold)    | 1 (unscaled "click")  |
| < 50 ms              | 4 (fast sweep)        |
| 50 – 150 ms          | 8                     |
| > 150 ms             | 16 (fine control)     |
| > 2 seconds          | self-reset (staleness)|

`int` and `enum` params accumulate raw ticks until the divisor threshold,
then emit `floor(|accum| / divisor)` integer steps per call so a fast jog
sweep (batched delta) advances proportionally — a delta of 8 with divisor=4
emits 2 steps. `enum` types use a fixed `enum_divisor = 10` ticks per option
regardless of count — binary toggles and 47-option pickers feel equally
snappy.

The engine **self-resets** after a >2 second idle gap, so re-entering a
parameter editor after a long pause feels like a fresh edit rather than
continuing a stale acceleration curve. There's no JS-side lifecycle plumbing
to maintain — the engine handles it internally.

Default float step (when a chain_params entry doesn't declare `step`) is
`0.01`. Default int step is `1`. Modules that need a different feel should
declare `step` explicitly. (Note: this is half the previous default of
`0.02` for floats — most modules already declare step, so this is invisible
in practice.)

## Shared Utilities

Import path from modules: `../../shared/<file>.mjs`

| File | Contents |
|------|----------|
| `constants.mjs` | Hardware constants (pads, buttons, knobs), MIDI message types, colors |
| `input_filter.mjs` | Capacitive touch filtering, LED control, encoder delta decoding with acceleration |
| `menu_items.mjs` | Menu item types and factory functions |
| `menu_nav.mjs` | Menu input handling (jog wheel, arrows, back button) |
| `menu_stack.mjs` | Hierarchical menu navigation stack |
| `menu_render.mjs` | Menu rendering with scroll support |
| `menu_layout.mjs` | Title/list/footer menu layout helpers |
| `text_scroll.mjs` | Marquee scrolling for long text |
| `move_display.mjs` | Display utilities |
| `filepath_browser.mjs` | Reusable filesystem browser helpers for `chain_params` type `filepath` |
| `logger.mjs` | Unified logging utilities |
| `screen_reader.mjs` | Screen reader announce/announceMenuItem/announceView helpers |
| `sampler_overlay.mjs` | Quantized sampler UI overlay |
| `text_entry.mjs` | On-screen keyboard for text input |
| `store_utils.mjs` | Module Store catalog fetching and install/remove functions |
| `scrollable_text.mjs` | Scrollable text component |
| `sound_generator_ui.mjs` | Sound generator UI helpers |
| `chain_param_utils.mjs` | Chain parameter handling utilities |
| `chain_ui_views.mjs` | Shadow UI view components |
| `parse_move_manual.mjs` | Move manual content parsing |

### Common Imports

```javascript
import {
    // Colors
    Black, White, LightGrey, BrightRed, BrightGreen,

    // MIDI message types
    MidiNoteOn, MidiNoteOff, MidiCC,

    // Hardware buttons (CC numbers)
    MoveShift, MoveMenu, MoveBack, MoveCapture,
    MoveUp, MoveDown, MoveLeft, MoveRight,
    MoveMainKnob, MoveMainButton,

    // Grouped arrays (preferred)
    MovePads,         // [68-99] all 32 pads
    MoveSteps,        // [16-31] all 16 step buttons
    MoveCCButtons,    // All CC button numbers
    MoveRGBLeds,      // All RGB LED addresses
    MoveWhiteLeds,    // All white LED addresses
} from '../../shared/constants.mjs';

// Usage:
if (MovePads.includes(note)) { /* handle pad */ }
const padIndex = note - MovePads[0];  // 0-31
```

### C Shared Utilities

For native code, shared headers are in `src/host/`:

| File | Contents |
|------|----------|
| `js_display.h/c` | Display primitives (set_pixel, draw_rect, print), font loading, QuickJS bindings |
| `shadow_constants.h` | Shadow mode shared memory names, buffer sizes, control structures |
| `plugin_api_v1.h` | DSP plugin interface |
| `audio_fx_api_v2.h` | Audio effects plugin interface |

## Help Content (help.json)

Modules can provide on-device help accessible from the Shadow UI's Help viewer (Global Settings → Updates → `[Help...]`). Add a `help.json` file to your module's source directory.

### File Location

```
src/modules/your-module/
  module.json
  ui.js
  help.json          # Help content for the Help viewer
```

The host scans all installed module directories for `help.json` at runtime. Module help topics appear alphabetically in the "Modules" section of the Help viewer.

They are also reachable **in place**: a chain component whose module ships a
`help.json` with a non-empty `children` array gets a **`Module Help`** row on the
`Module` page at the end of its knob-grid jog sequence, which opens that module's
topics directly (Back returns to the module, not up into the Help tree). A module
with no help content gets no row — an empty viewer is worse than no door — so
shipping `help.json` is what puts your help one jog from your controls.

### Format

Help content is a tree of sections and leaf topics:

```json
{
  "title": "Your Module",
  "children": [
    {
      "title": "Overview",
      "lines": [
        "Brief description",
        "of your module.",
        "",
        "Second paragraph",
        "with more detail."
      ]
    },
    {
      "title": "Controls",
      "children": [
        {
          "title": "Knob Mapping",
          "lines": [
            "Knob 1: Cutoff",
            "Knob 2: Resonance",
            "Knob 3: Attack",
            "Knob 4: Release"
          ]
        },
        {
          "title": "Other Settings",
          "lines": [
            "Detail about other",
            "settings here."
          ]
        }
      ]
    },
    {
      "title": "MIDI",
      "lines": [
        "MIDI behavior",
        "description."
      ]
    }
  ]
}
```

### Node Types

| Type | Fields | Description |
|------|--------|-------------|
| Branch | `title`, `children` | Navigable folder (shows as a list) |
| Leaf | `title`, `lines` | Scrollable text content |

- **Branch nodes** have a `children` array of other branch or leaf nodes. Nesting depth is unlimited.
- **Leaf nodes** have a `lines` array of strings displayed as scrollable text.

**The top-level object MUST have `children`.** The loader's whole test is
`if (helpData.children) helpMap[id] = helpData.children;` — a file that parses
as valid JSON but names its topics something else (`sections`, `pages`,
`parameters`) is **discarded without a word**, and the viewer shows "No help
content available for this module" as though the file were absent. A 2026-08
sweep of the catalog found 12 modules in exactly that state, several of them
carrying several KB of carefully written help nobody could read. `title` and
any other sibling keys are ignored; `children` is the only one that is read.

### Text Formatting Rules

The display is 128x64 pixels and a help line is **drawn, never wrapped and
never truncated**. `drawScrollableText` calls `print(4, y, line)`, `print()`
walks the string one glyph at a time, and everything past x=127 is dropped by
`set_pixel` with no error anywhere. So an over-long line loses its tail
silently — there is no ellipsis and nothing in the log.

**The budget is pixels, not characters.** The atlas is fixed-pitch, but
`load_font` auto-trims every glyph to its own inked extent, so text is
*proportional* on screen: `.` advances 3 px and `W` advances 6 px. Roughly 20
average characters fit, which is why the guidance below is 20 — but a line of
narrow letters can run longer and a line of capitals can overflow sooner. If
you are near the edge, measure rather than count.

- Keep lines to about **20 characters**, and treat that as a budget to check
  rather than a number to trust. A 2026-08 sweep of the catalog found 27
  modules shipping lines that genuinely run off the screen, the worst by 100 px
  — most of a second line's worth of text, invisible.
- **Stay in ASCII.** The bitmap font carries the printable ASCII range plus
  exactly `Ä Ö Ü ä ö ü € † ‡ °`. Any other character — an em dash, a curly
  quote, `≥`, an arrow — has no glyph, and `glyph()` renders it as a bare 1 px
  gap. It does not fall back to anything.

`tests/host/test_help_content_width.sh` measures the host's own help content
this way, deriving the glyph widths from the `FONT` table in
`scripts/generate_font.py`.
- Use empty strings (`""`) for blank lines between paragraphs
- Indent continuation lines with a leading space for readability:
  ```json
  "lines": [
    "Knob 3: Contour",
    " (filter env depth)"
  ]
  ```

### Packaging

Include `help.json` in your build script so it ends up in the distributed tarball:

```bash
# In scripts/build.sh, after copying other files to dist/<id>/
[ -f src/help.json ] && cp src/help.json dist/<id>/help.json
```

The host discovers `help.json` automatically — no changes to `module.json` are needed.

### Recommended Sections

A typical module help file includes:

| Section | Content |
|---------|---------|
| Overview | What the module does, key features |
| Controls / Knob Mapping | Which knobs control which parameters |
| MIDI | MIDI behavior, supported CCs, channel info |
| Presets | List of factory presets (if applicable) |

## Example Modules

See these modules for reference:

- **chain**: Signal chain with synths, MIDI FX, and audio FX
- **dexed**: Dexed FM synthesizer with native DSP (loads .syx patches)
- **sf2**: SoundFont synthesizer with native DSP
- **m8**: MIDI translator (UI-only, no DSP)
- **controller**: MIDI controller with banks (UI-only)

## Signal Chain Module

The Signal Chain module allows combining MIDI sources, MIDI effects, sound generators, and audio effects into configurable patches.

### Chain Structure

```
[Input or MIDI Source] → [MIDI FX] → [Sound Generator] → [Audio FX] → [Output]
```

### Available Components

| Type | Components |
|------|------------|
| MIDI Sources | Sequencers or other modules referenced via `midi_source` |
| Sound Generators | Line In, SF2, Dexed, CLAP, plus any module marked `"chainable": true` with `"component_type": "sound_generator"` (for example `obxd`, `minijv`) |
| MIDI Effects | Chord (15 chord types with inversions, voicings, strum), Arpeggiator (off, up, down, up_down, random with BPM/division/sync), Velocity Scale (min/max velocity mapping), plus external MIDI FX via Module Store |
| Audio Effects | Freeverb (reverb), CLAP effects, plus external audio FX via Module Store (CloudSeed, PSXVerb, Tapescam, etc.) |

### CLAP Host Module

The CLAP module (separate repo: `move-anything-clap`) hosts arbitrary CLAP audio plugins:

- Place `.clap` plugin files in `/data/UserData/schwung/modules/clap/plugins/`
- Plugins are discovered at load time
- Use jog wheel to browse plugins, encoders to control parameters
- CLAP synths work as sound generators in Signal Chain
- CLAP effects can be used in the audio FX slot

### Patch Files

Patches are stored in `/data/UserData/schwung/patches/` on the device as JSON:

```json
{
    "name": "Arp Piano Verb",
    "version": 1,
    "chain": {
        "input": "pads",
        "midi_fx": {
            "arp": {
                "mode": "up",
                "bpm": 120,
                "division": "8th"
            }
        },
        "synth": {
            "module": "sf2",
            "config": {
                "preset": 0
            }
        },
        "midi_source": {
            "module": "sequencer"
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.8,
                    "wet": 0.3
                }
            }
        ]
    }
}
```

JavaScript MIDI FX can be added per patch:

```json
"midi_fx_js": ["octave_up", "fifths"]
```

### Line In Sound Generator

The Line In sound generator passes external audio through the chain for processing:

```json
{
    "name": "Line In + Reverb",
    "chain": {
        "synth": {
            "module": "linein",
            "config": {}
        },
        "audio_fx": [
            {"type": "freeverb", "params": {"wet": 0.4}}
        ]
    }
}
```

Note: Audio input routing depends on the last selected input in the stock Move interface.

## Audio FX Plugin API

Audio effects use an in-place processing API. The v2 API supports multiple instances:

**Filename:** When loaded inside Signal Chain, the chain host expects the shared library at `modules/audio_fx/<id>/<id>.so` — it does not read `module.json`'s `dsp` field. Name your audio FX shared library `<module-id>.so` (e.g. `freeverb.so`, `cloudseed.so`), not `dsp.so`.

```c
typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);  // Optional
} audio_fx_api_v2_t;

// Entry point
audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);
```

The `on_midi` callback is optional (can be NULL). Implement it to receive MIDI from capture rules or other sources.

## MIDI FX Plugin API

MIDI effects transform or generate MIDI messages. They use a separate API defined in `src/host/midi_fx_api_v1.h`:

```c
typedef struct midi_fx_api_v1 {
    uint32_t api_version;  /* Must be 1 (MIDI_FX_API_VERSION) */

    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);

    /* Transform incoming MIDI. Returns number of output messages (0 = swallow, >1 = expand).
     * out_msgs: array of 3-byte MIDI messages
     * max_out: maximum output messages (MIDI_FX_MAX_OUT_MSGS = 16) */
    int (*process_midi)(void *instance,
                        const uint8_t *in_msg, int in_len,
                        uint8_t out_msgs[][3], int out_lens[],
                        int max_out);

    /* Time-based generation (e.g., arpeggiator). Called each audio block.
     * Returns number of output messages to inject. */
    int (*tick)(void *instance,
                int frames, int sample_rate,
                uint8_t out_msgs[][3], int out_lens[],
                int max_out);

    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} midi_fx_api_v1_t;

// Entry point
midi_fx_api_v1_t* move_midi_fx_init(const host_api_v1_t *host);
```

**Key differences from sound generators and audio FX:**

- `process_midi()` transforms incoming MIDI — can output 0 (swallow), 1 (pass/modify), or multiple messages (e.g., chord generates multiple notes)
- `tick()` handles time-based generation (e.g., arpeggiator note sequencing) — called every audio block
- No `render_block()` — MIDI FX don't process audio
- Maximum 2 native MIDI FX per chain (`MAX_MIDI_FX`)
- Maximum 16 output messages per `process_midi()` call (`MIDI_FX_MAX_OUT_MSGS`)

### Building MIDI FX

MIDI FX are built identically to other native plugins:

```bash
"${CROSS_PREFIX}gcc" -g -O3 -shared -fPIC \
    src/modules/midi_fx/your-fx/dsp/your_fx.c \
    -o build/modules/midi_fx/your-fx/dsp.so \
    -Isrc
```

### Built-in MIDI FX

| Module | ID | Description |
|--------|----|-------------|
| Chord | `chord` | Chord generator (15 types, inversions, voicings, strum) |
| Arpeggiator | `arp` | Arpeggiator (up, down, up_down, random with tempo sync) |
| Velocity Scale | `velocity_scale` | Velocity range mapping (min/max) |

### MIDI FX module.json Example

```json
{
    "id": "velocity_scale",
    "name": "Velocity Scale",
    "abbrev": "VS",
    "version": "0.1.0",
    "builtin": true,
    "capabilities": {
        "chainable": true,
        "component_type": "midi_fx",
        "ui_hierarchy": {
            "levels": {
                "root": {
                    "name": "Velocity Scale",
                    "params": [
                        {"key": "min", "name": "Min", "type": "int", "min": 1, "max": 127},
                        {"key": "max", "name": "Max", "type": "int", "min": 1, "max": 127}
                    ],
                    "knobs": ["min", "max"]
                }
            }
        }
    }
}
```

## Host API (Passed to Plugins)

All plugin init functions receive a `host_api_v1_t` struct providing access to host services:

```c
typedef struct host_api_v1 {
    uint32_t api_version;

    /* Audio constants */
    int sample_rate;         /* 44100 */
    int frames_per_block;    /* 128 */

    /* Direct mailbox access (use with care) */
    uint8_t *mapped_memory;
    int audio_out_offset;    /* Offset to audio output in mailbox */
    int audio_in_offset;     /* Offset to audio input in mailbox */

    /* Logging */
    void (*log)(const char *msg);

    /* MIDI send functions
     * msg: 4-byte USB-MIDI packet [cable|CIN, status, data1, data2]
     * Returns: bytes queued, or 0 on failure */
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);

    /* Clock status for sync-aware plugins */
    int (*get_clock_status)(void);

    /* Transport beat position for phase-locked sync. Beats elapsed since the
     * active transport's start, derived from 24-PPQN MIDI clock and
     * interpolated per audio block, tracking whichever transport is playing —
     * Move's native sequencer (cable-0 clock) or an internal overtake
     * sequencer that emits clock (e.g. movy). Returns < 0 when no transport is
     * running (callers should fall back, e.g. free-run an LFO). Use this
     * instead of accumulating phase from get_bpm() to stay drift-free and
     * bar-aligned. May be NULL on older hosts — always guard. Appended 2026-07. */
    double (*get_beat_position)(void);
} host_api_v1_t;
```

**Tempo while stopped:** `get_bpm()` retains the last-playing transport's tempo
after it stops (so a synced LFO that switches from phase-lock to free-run keeps
the same rate). It updates from emitted clock, so changing an internal
sequencer's tempo while it is *stopped* is not reflected until it plays again.

## Audio Specifications

- Sample rate: 44100 Hz
- Block size: 128 frames
- Latency: ~3ms
- Format: Stereo interleaved int16

## Shadow Mode Integration

Shadow Mode runs custom signal chains alongside stock Ableton Move. Your modules are **automatically available** in shadow mode without any additional work - no recompilation required.

### How It Works

Shadow mode loads the chain module and patches. When you install a module via Module Store (or manually copy it to the modules directory), it becomes available in Shadow Mode.

Modules and patches are read from:
- Modules: `/data/UserData/schwung/modules/`
- Patches: `/data/UserData/schwung/patches/`

### Making Your Module Shadow-Compatible

If your module is chainable (sound generator or audio FX), it works in shadow mode automatically. Ensure your `module.json` has:

```json
{
    "capabilities": {
        "chainable": true,
        "component_type": "sound_generator"
    }
}
```

Valid `component_type` values for chainable modules:
- `sound_generator` - Synths and samplers
- `audio_fx` - Audio effects

### Creating Shadow Chain Patches

Patches are JSON files in `/data/UserData/schwung/patches/`. To create a patch using your module:

```json
{
    "name": "My Synth + Reverb",
    "version": 1,
    "chain": {
        "input": "pads",
        "synth": {
            "module": "your-module-id",
            "config": {
                "preset": 0
            }
        },
        "audio_fx": [
            {
                "type": "freeverb",
                "params": {
                    "room_size": 0.7,
                    "wet": 0.25
                }
            }
        ]
    }
}
```

The `module` field must match the `id` in your module's `module.json`.

### Shadow Mode MIDI Routing

Each shadow slot listens on a configurable MIDI channel (default 1-4):

| Shadow Slot | Default Channel |
|-------------|-----------------|
| Slot A | Ch 1 |
| Slot B | Ch 2 |
| Slot C | Ch 3 |
| Slot D | Ch 4 |

**Forward Channel:** Some synths need MIDI on a specific channel regardless of slot. Configure via the slot's "Forward Ch" setting:
- **Auto**: Pass MIDI through on the receive channel (default)
- **1-16**: Remap all MIDI to that specific channel before sending to the synth

Each shadow slot can load a different chain patch. Slot settings and synth states persist across restarts.

### Making a module compatible with Module Presets

The shadow UI lets users save/recall **module presets** — per-component snapshots of a
single chain component's state (one synth's patch, one FX's setting), independent of the
whole-chain patch. The feature is **generic and needs no per-module code**: it captures and
restores the same opaque `state` blob the host already uses for per-slot autosave. A module
is preset-compatible as long as it honors that state contract:

- `get_param("state", buf, len)` returns the module's **full, self-contained** state
  (everything needed to reproduce the current sound — not a reference/index into a bank).
- `set_param("<prefix>:state", blob)` restores it. The host drives recall through the
  verified slot-load restore path; `<prefix>` is the component slot (`synth`, `fx1`..`fxN`,
  or `midi_fx1`) and is supplied by the host.

Modules that already support per-slot autosave get module presets for free. A module that
doesn't expose a self-contained `state` (or that returns a referential blob) won't produce a
usable preset, so make `state` round-trip-complete. (User-facing usage is covered in the
manual.)

#### If you implement no `state` at all

Your parameters do not survive a reboot. That is the expected cost and it is
yours alone to pay — **but until 2026-08 it was not.** The host asked for
`state`, could not tell `""` ("this module declares none") apart from `null`
("the read did not complete"), and took the second reading: it then skipped the
save to avoid clobbering a good file with defaults, and skipped it for the
**whole slot**, including the other components in it.

So a slot containing `denis` or `branchage` never autosaved *anything*, ever —
not the synth, not the FX behind it — and silently, because the thing that fires
is a guard designed to leave your file alone. Fixed; the two answers are kept
apart now.

The obligation this leaves you: **answer the `state` query one way or the
other.** Return your blob, or return `-1` / an empty buffer to say you have
none. What you must not do is fail to answer — a read that times out is still
indistinguishable from a module that is simply slow, and the host will (rightly)
protect the existing file rather than overwrite it with defaults. If your
`get_param` can block — see the threading section; several modules read files
from it — that is the same bug wearing a different hat.

### Testing Shadow Mode

1. Build and install Schwung: `./scripts/build.sh && ./scripts/install.sh local`
2. Launch stock Move (or reboot device)
3. Toggle shadow mode: **Shift + touch Volume + touch Knob 1**
4. Use jog wheel to select a slot, click to browse patches
5. Load a patch that uses your module
6. Set a Move track to MIDI channel 5-8 and play pads

### Capture Rules

Chain patches can capture specific Move controls exclusively. When a slot with capture rules is focused in the shadow UI, captured controls are blocked from reaching Move and routed to the slot's DSP.

**Patch-level capture:**

```json
{
    "name": "Performance Effect",
    "synth": { "module": "sf2" },
    "audio_fx": [{ "type": "freeverb" }],
    "capture": {
        "groups": ["steps"]
    }
}
```

**Capture format:**

```json
{
    "capture": {
        "groups": ["steps", "pads"],
        "notes": [60, 61, 62],
        "note_ranges": [[68, 75]],
        "ccs": [118, 119],
        "cc_ranges": [[100, 110]]
    }
}
```

All fields are optional and combine as a union.

**Control group aliases:**

| Alias | Type | Values | Description |
|-------|------|--------|-------------|
| `pads` | notes | 68-99 | 32 performance pads |
| `steps` | notes | 16-31 | 16 step sequencer buttons |
| `tracks` | CCs | 40-43 | 4 track buttons |
| `knobs` | CCs | 71-78 | 8 encoders (relative) |
| `knobs_abs` | CCs | 102-109 | 8 knobs (absolute 0-127, scaled to assigned param range) |
| `jog` | CC | 14 | Main encoder |

**Module-level capture:**

A module can declare capture rules for itself in `module.json`, under
`capabilities`. This is the right place when the need belongs to the module
rather than to a patch — a drum machine whose own sequencer reads the step
buttons wants them whatever patch wraps it:

```json
{
    "id": "9w9",
    "capabilities": {
        "component_type": "sound_generator",
        "capture": { "groups": ["steps"] }
    }
}
```

A chain slot captures the **union** of its patch's rules and its synth
module's. The module rules are re-derived on every load path — library load,
autosave restore at boot, set switch, module swap — which matters because the
patch rules are read from the patch *file* and only when a patch is loaded from
the library by index. A slot restored from autosave has no patch file to read,
so until module-level capture existed it came back with no rules at all.

Audio FX modules use the same block for Master FX:

```json
{
    "id": "perfverb",
    "capabilities": {
        "component_type": "audio_fx",
        "capture": {
            "groups": ["steps"]
        }
    }
}
```

**Note:** Audio FX modules that want to receive captured MIDI must implement `on_midi` in their API. If `on_midi` is NULL, captured MIDI is blocked from Move but not routed to the FX.

### Parameter Locks

A patch that captures the steps also gets **parameter locks**: hold a step
button, turn an encoder, and that value belongs to that step. It is a host
feature, not a module one — any module in a chain slot gets it, and a module
needs to do nothing to opt in beyond declaring its parameters as it already
does.

```json
{
    "name": "9W9",
    "chain": { "synth": { "module": "9w9" } },
    "capture": { "groups": ["steps"] }
}
```

**Every grid gets the gesture.** The editor half lives in the shared
`page_controller` (`onStepButton`), fed by `page_input`'s `step` intent — so a
module that draws its own grid through `createController` + `decodeInput` +
`applyInput` (9W9 and its ports) places locks with no changes of its own, and so
does the native shadow grid. A module whose sequencer also reads the step
buttons should toggle on a short *tap's release*, not on the press: a held step
is the lock gesture, and the note still reaches the module's `on_midi`.

**Non-destructive.** A lock is published as an *absolute* modulation source
while its step plays and cleared when the step passes, so the saved value is
never written. Turning locks off restores exactly the sound that was saved.
Relative sources (an LFO) still sum on top, so a locked parameter can still be
modulated.

**Timing is explicit, not inferred.** Nothing reports Move's clip length to
Schwung — `get_beat_position()` gives beats since transport start and there is
no Song Position Pointer anywhere in the stack. The step is therefore derived
from a pattern length and a step rate that the **user sets**, defaulting to 16
steps of 1/16. Guessing these would put every lock on the wrong step whenever a
clip was 32 steps or a triplet feel, with nothing on screen to explain it.

| key | direction | meaning |
|---|---|---|
| `lock:enabled` | set/get | master on/off; off clears every lock source |
| `lock:pattern_len` | set/get | steps in the loop, 1–64 (default 16) |
| `lock:rate_div` | set/get | index into `lfo_divisions[]` (default `1/16`) |
| `lock:rate_div_label` | get | that division's label, e.g. `"1/16"` |
| `lock:set` | set | `"<target>:<param>:<step>:<value>"` |
| `lock:clear` | set | `"<target>:<param>:<step>"` |
| `lock:clear_all` | set | drop every lock; keeps the timing settings |
| `lock:step` | get | the playing step, or `-1` when stopped |
| `lock:at:<step>` | get | every lock on one step, as `{"synth:key":value}` |
| `lock:steps:<target>:<param>` | get | that parameter's step mask, as a decimal bitmap |
| `lock:lane_count` | get | lanes in use (see the cap below) |
| `lock_config` | get | the whole thing as JSON, for patch save |

Locks persist in the patch, beside `lfos`, and are restored with it.

**Settings.** Slot settings → **Locks** page: `Locks` on/off, `Rec`, `Steps`
(pattern length, 1–64) and `Rate` (step division). **Actions → Clear Locks**
drops every lock and keeps Steps/Rate.

**Master FX has the same feature**, on the same clock and the same page, keyed
`master_fx:lock:*`. It is served by the shim rather than the chain — Master FX
has no modulation bus — so a lock there displaces the parameter directly and
the shim remembers the exact string it displaced, putting it back when the step
passes. Master locks are saved in the master preset beside its LFO configs.
Lane targets are `fx1`…`fxN` by master position.

**Live recording.** Press Move's **Record** button while a Schwung slot is
focused — Move arms its own clip recording on the same press; this is the
Schwung half — or turn `Rec` on. While the transport runs, every knob move is
also stamped as a lock on the step playing at that moment, Elektron-style: the
steps you pass keep the values you passed them with, the base keeps moving so
you hear the turn, and steps you never reached play the value you ended on.
Recording ends with the transport. Any write records — a module-owned grid,
the native grid, the web panel, an assigned CC — because the stamping happens
in the chain, not in a UI.

**What a lock is attached to — and what it is not.** Locks belong to the
*slot's sound*: they are saved with the patch and the set, not with a Move
clip, because Schwung cannot see Move's clips. So:

- Switching clips on the track: the locks stay with the sound.
- Copying or duplicating a Move clip: the locks are not in it; they do not copy.
- Changing the clip length: locks repeat every `Steps` steps. Set Steps to 32
  for a 32-step clip, or a lock on step 9 plays on 9 and 25.
- Locks beyond the current `Steps` are kept, not deleted, so shortening and
  lengthening again loses nothing.

**Limits.** Up to 16 *lanes* — distinct `(target, param)` pairs holding locks —
and 64 steps. Lanes are capped because each one that fires on the current step
consumes one of the chain's 32 modulation entries, which the LFOs also draw
from; only the current step's lanes are live at once. Asking for a 17th lane
does not silently do nothing: it is refused and logged.

### Shadow Mode Configuration

Shadow slot configuration is stored in `/data/UserData/schwung/shadow_chain_config.json`:

```json
{
    "patches": [
        { "name": "SF2 + Freeverb", "channel": 5 },
        { "name": "Dexed + Freeverb", "channel": 6 },
        { "name": "OB-Xd + Freeverb", "channel": 7 },
        { "name": "Mini-JV + Freeverb", "channel": 8 }
    ]
}
```

The shadow UI updates this file when you select patches for each slot.

## Overtake Modules

Overtake modules take complete control of Move's UI while running in shadow mode. Unlike regular shadow mode (which displays a custom UI alongside Move), overtake modules fully replace Move's display and control all LEDs.

### Configuration

Set `component_type: "overtake"` in module.json:

```json
{
    "id": "controller",
    "name": "MIDI Controller",
    "version": "1.0.0",
    "component_type": "overtake",
    "ui": "ui.js",
    "api_version": 2
}
```

Overtake modules appear in a dedicated section of the shadow UI menu.

### Lifecycle

When an overtake module is loaded:

1. **LED Clearing**: The host progressively clears all LEDs (pads, steps, buttons, knob indicators)
2. **Loading Screen**: "Loading..." is displayed during LED clearing
3. **Deferred Init**: After ~500ms delay, the module's `init()` is called
4. **Module Takes Over**: Module controls display and all LEDs

The progressive LED clearing prevents MIDI buffer overflow (the buffer holds ~64 packets).

#### Optional lifecycle hooks

Beyond `init()`, `tick()`, and `onMidiMessageInternal()`/`onMidiMessageExternal()`, overtake modules may define two optional hooks:

- `globalThis.onUnload()` — called once when the module is being torn down. Use it to release resources or persist state.
- `globalThis.onResume()` — called once each time a suspended `suspend_keeps_js` overtake module is brought back to the foreground. It is **not** called on first load (`init()` handles that). While the module was backgrounded its hardware LEDs were cleared, so the typical use is to invalidate any on-change LED delta-cache and force a full repaint on the next `tick()`. Both hooks are opt-in: modules that do not define them are unaffected.

### Host-Level Escape

The host provides a built-in escape mechanism that always works, regardless of module implementation:

**Shift + Volume Touch + Jog Click** exits any overtake module

The host tracks shift and volume touch state locally (not relying on the shim's tracking, which doesn't work in overtake mode) to ensure the escape always functions.

### Progressive LED Handling

The MIDI output buffer is limited (~64 packets). Sending all LED commands at once causes buffer overflow. Use progressive LED handling:

**In the host (LED clearing):**
```javascript
const LEDS_PER_BATCH = 20;
let ledClearIndex = 0;

function clearLedBatch() {
    // Clear 20 LEDs per frame until done
    // Covers: pads (68-99), steps (16-31), buttons, knob indicators
}
```

**In your module (initialization):**

Use the shared `setLED()` and `setButtonLED()` from `input_filter.mjs` — they provide caching (skip duplicate sends) and correct MIDI packet formatting:

```javascript
import {
    MoveBack, MoveCapture, MoveUndo, MoveLoop, MoveCopy, MoveMute, MoveDelete,
    MovePads, White, DarkGrey,
    WhiteLedDim, WhiteLedMedium, WhiteLedBright
} from '/data/UserData/schwung/shared/constants.mjs';

import { setLED, setButtonLED } from '/data/UserData/schwung/shared/input_filter.mjs';

let ledInitPending = false;
let ledInitIndex = 0;
const LEDS_PER_FRAME = 8;

globalThis.init = function() {
    ledInitPending = true;
    ledInitIndex = 0;
};

function setupLedBatch() {
    const leds = [];
    // Button LEDs (CC-based) — use setButtonLED
    leds.push({ type: 'cc', id: MoveBack, color: WhiteLedDim });
    leds.push({ type: 'cc', id: MoveCapture, color: WhiteLedDim });
    // Pad LEDs (note-based) — use setLED
    for (const pad of MovePads) {
        leds.push({ type: 'note', id: pad, color: DarkGrey });
    }

    const end = Math.min(ledInitIndex + LEDS_PER_FRAME, leds.length);
    for (let i = ledInitIndex; i < end; i++) {
        if (leds[i].type === 'cc') setButtonLED(leds[i].id, leds[i].color);
        else setLED(leds[i].id, leds[i].color);
    }
    ledInitIndex = end;
    if (ledInitIndex >= leds.length) ledInitPending = false;
}

globalThis.tick = function() {
    if (ledInitPending) {
        setupLedBatch();
    }
    drawUI();
};
```

**Important:** Always use the shared `setLED()` and `setButtonLED()` from `input_filter.mjs` rather than calling `move_midi_internal_send()` directly. The shared helpers handle LED caching and correct USB-MIDI cable byte formatting. Use absolute import paths (`/data/UserData/schwung/shared/...`) for module location independence.

### LED Addresses

When clearing or setting LEDs, address both note-based and CC-based LEDs:

| Type | Addressing | Values |
|------|-----------|--------|
| Pads | Notes | 68-99 |
| Steps | Notes | 16-31 |
| Knob touch | Notes | 0-7 |
| Step icons | CCs | 16-31 |
| Track buttons | CCs | 40-43 |
| Shift | CC | 49 |
| Menu/Back/Capture | CCs | 50-52 |
| Up/Down | CCs | 54-55 |
| Undo/Loop/Copy | CCs | 56, 58, 60 |
| Left/Right | CCs | 62-63 |
| Knob indicators | CCs | 71-78 |
| Play/Rec | CCs | 85-86 |
| Mute | CC | 88 |
| Record/Delete | CCs | 118-119 |

### MIDI Routing

In overtake mode:
- All internal MIDI is passed to the module's `onMidiMessageInternal`
- External MIDI is passed to `onMidiMessageExternal`
- The host intercepts Shift+Vol+Jog before the module sees it (for escape)
- Modules can send MIDI out via `move_midi_external_send` and `move_midi_internal_send`

### Example: MIDI Controller

The built-in MIDI Controller module (`src/modules/controller/`) demonstrates overtake patterns:

- 16 banks of pad/knob mappings
- Step buttons switch banks
- Jog wheel and Up/Down buttons for octave shift
- Progressive LED initialization
- Dynamic C note highlighting based on octave

## Publishing to Module Store

External modules can be distributed via the built-in Module Store. Users can browse, install, update, and remove modules directly from their Move device.

### Requirements

1. Module builds as a self-contained tarball: `<id>-module.tar.gz`
2. Tarball extracts to a folder matching the module ID (e.g., `minijv/`)
3. GitHub repository with releases enabled
4. GitHub Actions workflow for automated builds

### Tarball Structure

```
<id>-module.tar.gz
  └── <id>/
      ├── module.json       # Required
      ├── ui.js             # Optional: JavaScript UI
      ├── dsp.so            # Optional: Native DSP plugin
      └── ...               # Other module files
```

### Release Workflow

1. **Make changes and update version** in `src/module.json`:
   ```json
   {
     "version": "0.2.0"
   }
   ```

2. **Commit and tag the release**:
   ```bash
   git add .
   git commit -m "Release v0.2.0"
   git tag v0.2.0
   git push && git push --tags
   ```

3. **GitHub Actions automatically**:
   - Builds the module using Docker cross-compilation
   - Creates `<id>-module.tar.gz`
   - Attaches it to the GitHub release

4. **Update the catalog** in `move-anything/module-catalog.json` (if not already listed):
   ```json
   {
     "id": "your-module",
     "name": "Your Module",
     "description": "What it does",
     "author": "Your Name",
     "component_type": "sound_generator",
     "github_repo": "username/move-anything-yourmodule",
     "default_branch": "main",
     "asset_name": "your-module-module.tar.gz",
     "min_host_version": "0.3.0",
     "requires": "Optional: external assets needed (e.g. sample files, ROMs)"
   }
   ```

5. **Commit catalog update**:
   ```bash
   cd move-anything
   git add module-catalog.json
   git commit -m "Update your-module to v0.2.0"
   git push
   ```

### GitHub Actions Workflow Template

Add `.github/workflows/release.yml` to your module repository:

```yaml
name: Release

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: ubuntu-latest

    steps:
      - uses: actions/checkout@v4

      - name: Set up Docker
        uses: docker/setup-buildx-action@v3

      - name: Build module
        run: ./scripts/build.sh

      - name: Package module
        run: |
          cd dist
          tar -czvf ../${{ github.event.repository.name }}-module.tar.gz */

      - name: Create Release
        uses: softprops/action-gh-release@v1
        with:
          files: ${{ github.event.repository.name }}-module.tar.gz
        env:
          GITHUB_TOKEN: ${{ secrets.GITHUB_TOKEN }}

      - name: Update release.json
        run: |
          VERSION="${GITHUB_REF_NAME#v}"
          git fetch origin main
          git checkout -f main
          git config user.name "github-actions[bot]"
          git config user.email "github-actions[bot]@users.noreply.github.com"

          cat > release.json << EOF
          {
            "version": "${VERSION}",
            "download_url": "https://github.com/${{ github.repository }}/releases/download/${{ github.ref_name }}/${{ github.event.repository.name }}-module.tar.gz"
          }
          EOF

          git add release.json
          git commit -m "chore: update release.json for ${{ github.ref_name }}" || echo "No changes to commit"
          git push origin main
```

### Catalog Entry Schema (v2)

Each module in `module-catalog.json`:

```json
{
  "id": "module-id",
  "name": "Display Name",
  "description": "Short description",
  "author": "Author Name",
  "component_type": "sound_generator|audio_fx|midi_fx|overtake|utility|tool",
  "github_repo": "username/repo-name",
  "default_branch": "main",
  "asset_name": "module-id-module.tar.gz",
  "min_host_version": "0.3.0",
  "requires": "Optional: external assets needed (e.g. ROM files, .sf2 soundfonts)"
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Module ID (lowercase hyphenated) |
| `name` | Yes | Display name |
| `description` | Yes | Short description |
| `author` | Yes | Author name |
| `component_type` | Yes | `sound_generator`, `audio_fx`, `midi_fx`, `overtake`, `utility`, `tool` |
| `github_repo` | Yes | GitHub `owner/repo` |
| `default_branch` | Yes | Branch to fetch `release.json` from (usually `main`) |
| `asset_name` | Yes | Expected tarball filename |
| `min_host_version` | Yes | Minimum compatible host version |
| `requires` | No | User-facing note about required external assets |

### release.json

Each module repo must have a `release.json` on its main branch. The Module Store fetches this file (not the GitHub releases API) to determine the latest version and download URL.

```json
{
  "version": "0.2.0",
  "download_url": "https://github.com/username/move-anything-mymodule/releases/download/v0.2.0/mymodule-module.tar.gz"
}
```

For a repository that publishes more than one catalog module, use a `modules`
object keyed by the exact catalog IDs. Each entry has the same fields as a
single-module release:

```json
{
  "modules": {
    "module-a": {
      "version": "0.2.0",
      "download_url": "https://github.com/username/repo/releases/download/v0.2.0/module-a-module.tar.gz"
    },
    "module-b": {
      "version": "0.2.0",
      "download_url": "https://github.com/username/repo/releases/download/v0.2.0/module-b-module.tar.gz"
    }
  }
}
```

Schwung Manager and the shared store utilities select the entry matching the
catalog module ID. If it is missing, the manager falls back to the catalog's
`asset_name` latest-release URL.

Optional fields: `install_path`, `name`, `description`, `requires`, `post_install`, `repo_url`. Fields like `name`, `description`, and `requires` in `release.json` override their catalog equivalents.

The release workflow should auto-update `release.json` on each tagged release (see the workflow template above for an example).

### How the Module Store Works

1. Fetches `module-catalog.json` from the main branch
2. For each module, fetches `release.json` from the module's GitHub repo (on `default_branch`)
3. Compares `release.json` version to installed version
4. Downloads tarball from `release.json`'s `download_url`
5. Extracts tarball to category subdirectory (e.g., `modules/sound_generators/<id>/`)

### Component Types

| Type | Description |
|------|-------------|
| `sound_generator` | Synthesizers and samplers that produce audio |
| `audio_fx` | Audio effects that process audio |
| `midi_fx` | MIDI effects that transform MIDI |
| `overtake` | Overtake modules (full UI control) |
| `utility` | Utility modules |

## Host Updates

The Schwung host can also be updated via the Module Store. When an update is available, "Update Host" appears at the top of the Module Store category list.

### Releasing a Host Update

1. **Bump the version** in `src/host/version.txt`:
   ```
   1.0.1
   ```

2. **Build and package**:
   ```bash
   ./scripts/build.sh
   ```

3. **Create a GitHub release** with the tarball:
   ```bash
   gh release create v1.0.1 schwung.tar.gz --title "v1.0.1" --notes "Release notes here"
   ```

4. **Update the catalog** in `module-catalog.json`:
   ```json
   {
     "host": {
       "name": "Schwung",
       "github_repo": "charlesvestal/schwung",
       "asset_name": "schwung.tar.gz",
       "latest_version": "1.0.1",
       "min_host_version": "1.0.0"
     }
   }
   ```

5. **Push the catalog update**:
   ```bash
   git add module-catalog.json
   git commit -m "Update host to v1.0.1"
   git push
   ```

### How Host Updates Work

1. Module Store fetches `module-catalog.json` from the main branch
2. Fetches `release.json` from the host repo for the latest version and download URL
3. Compares to installed version in `/data/UserData/schwung/host/version.txt`
4. If different, shows "Update Host" option with version numbers
5. Update downloads the tarball and extracts over the existing installation
6. User must restart Schwung for changes to take effect

### Catalog Location

The Module Store fetches the catalog from:
```
https://raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json
```
