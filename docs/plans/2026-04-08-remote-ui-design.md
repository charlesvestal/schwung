# Remote Web UI for Schwung Modules

**Date:** 2026-04-08
**Branch:** `remote-ui`
**Status:** Design

## Goal

Translate the on-device module UI (128x64 OLED, hardware knobs/pads) into an interactive web interface accessible from a phone, tablet, or laptop. Bidirectional — changes on the web reflect on hardware and vice versa.

## Architecture

Three layers:

```
Browser (Web UI)
    ↕ WebSocket (JSON messages)
schwung-manager (Go)
    ↕ HTTP on localhost (param bridge API)
display_server (C)
    ↕ direct shared memory
/schwung-param, /schwung-ui, /schwung-control
```

### Communication Flow

- **Browser → Device:** User drags a knob → WebSocket message → Go proxies to C param bridge → C writes to shared memory → DSP picks it up next audio block.
- **Device → Browser:** Go polls C param bridge every ~100ms per subscribed slot → diffs against last known state → pushes only changed params over WebSocket.

## C Param Bridge (display_server.c)

New HTTP listener on `localhost:7682` (local-only, Go proxies everything).

### Endpoints

```
GET  /params/:slot/hierarchy    → ui_hierarchy JSON
GET  /params/:slot/chain_params → parameter metadata
GET  /params/:slot/:key         → current value
POST /params/:slot/:key         → set value (body = new value)
GET  /params/:slot/bulk         → all current param values at once
```

### Shared Memory Access

- `set_param`: Write to `shadow_param_t` — set `request_type=1`, `slot`, `key`, `value`, wait for `request_type` to return to 0.
- `get_param`: Write `request_type=2`, `slot`, `key`, read back `value` when `request_type` returns to 0.
- `bulk`: Iterate over known param keys from `chain_params`, issue sequential get_param calls. Microseconds each via shared memory.
- Serialized with a mutex. At 100ms polling intervals across 4 slots, contention is negligible.

## Go WebSocket Layer (schwung-manager)

Single WebSocket connection per browser client at `ws://<move-ip>:port/ws/remote-ui`.

### Messages (browser → server)

```json
{"type": "subscribe", "slot": 0}
{"type": "unsubscribe", "slot": 0}
{"type": "set_param", "slot": 0, "key": "synth:cutoff", "value": "0.75"}
{"type": "get_hierarchy", "slot": 0}
```

### Messages (server → browser)

```json
{"type": "hierarchy", "slot": 0, "data": {}}
{"type": "chain_params", "slot": 0, "data": []}
{"type": "param_update", "slot": 0, "params": {"synth:cutoff": "0.75"}}
{"type": "slot_info", "slot": 0, "synth": "braids", "fx1": "cloudseed", "name": "Slot 1"}
```

### Polling Logic

- For each subscribed slot, poll C param bridge bulk endpoint every 100ms.
- Diff against last known state; push only changed params via `param_update`.
- On subscribe, send full `hierarchy` + `chain_params` + initial `param_update` with all values.
- Knob drag optimization: browser sends `set_param` throttled at ~30Hz, Go forwards immediately (no batching).

## Web UI

### Layout

Tab bar at top: **Slot 1 | Slot 2 | Slot 3 | Slot 4 | Master FX**

Each tab shows:

```
┌─────────────────────────────────┐
│ Slot 1: Braids                  │  ← slot name + loaded synth
│ [Breadcrumb: Root > Filter]     │  ← hierarchy navigation
├─────────────────────────────────┤
│  ◎ Cutoff   ◎ Reso   ◎ Drive   │  ← visual knobs (from hierarchy
│   0.75       0.30      0.50    │     "knobs" array, up to 8)
├─────────────────────────────────┤
│  ▸ Algorithm: Analog Saw  [▾]  │  ← enum → dropdown
│  ▸ Octave: [-2 ───●── +2]     │  ← int → slider
│  ▸ Choose Preset           →   │  ← navigation → drills into
│  ▸ Effects Settings        →   │     child hierarchy level
└─────────────────────────────────┘
```

### Control Type Mapping (from chain_params metadata)

- `float` → visual knob (if in `knobs` array) or horizontal slider
- `int` → slider with step=1
- `enum` → dropdown
- Navigation entries (from `ui_hierarchy` params with `level` field) → clickable links, breadcrumb updates

### Custom Module UI

If a module ships `web_ui.html` in its module directory, the tab loads that in an iframe instead of auto-generating. The iframe gets an injected JS API:

```javascript
schwungRemote.getParam(key)              // → Promise<string>
schwungRemote.setParam(key, value)       // → void
schwungRemote.onParamChange(callback)    // callback({key, value})
schwungRemote.getHierarchy()             // → Promise<object>
```

## Implementation Phases

### Phase 1 — C Param Bridge
- Add localhost HTTP endpoints to display_server.c
- GET/POST for individual params, bulk reads, hierarchy
- Test with curl from device

### Phase 2 — Go WebSocket Layer
- Add WebSocket endpoint to schwung-manager
- Subscribe/unsubscribe per slot
- Poll param bridge, diff, push updates
- Forward set_param to bridge

### Phase 3 — Web UI (Auto-Generated)
- Tab bar with slot switching
- Parse ui_hierarchy into navigable menu
- Render knobs (knobs.js or similar) for knob-mapped params
- Render sliders/dropdowns for other param types
- Breadcrumb navigation for hierarchy levels
- Bidirectional sync

### Phase 4 — Custom UI Support
- Detect `web_ui.html` in module directory
- Load in iframe with injected `schwungRemote` API

### Phase 5 — Polish
- Mobile-friendly layout (phone/tablet)
- WebSocket reconnection handling
- Loading states, error feedback

## Design Decisions

- **One slot at a time** with tabs (not multi-slot dashboard). Custom multi-slot layouts are a future feature.
- **Auto-generated UI by default**, optional custom `web_ui.html` per module.
- **Go for web, C for shared memory** — hybrid approach keeps complexity in the right places.
- **100ms polling** for device→browser sync — good enough for live feedback without excessive load.
- **knobs.js-style** visual knobs for the 8 hardware-mapped parameters; appropriate controls for other param types.
