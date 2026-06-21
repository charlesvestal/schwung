# Move MCP

Move MCP is a proposed WiFi control bridge for Schwung. The working idea is:
external tools such as Codex, a phone app, or a desktop companion connect to
Move over the local network, inspect Schwung state, upload generated musical
material, and trigger safe actions on-device.

This is separate from the proposed **Move MPC** performance module. Move MCP is
the control/data bridge; Move MPC can be one module that consumes generated loop
packs from that bridge.

## Current Module Status

There is now a minimal installable `move-mcp` tool module:

```text
src/modules/tools/move-mcp/
  module.json
  settings-schema.json
  ui.js
```

This module is a configuration/status surface, not the WiFi server itself. It
lets Schwung Manager write a per-module `config.json` and optional token secret
that Manager endpoints read before allowing read/write operations.

Current settings:

| Key | Purpose |
| --- | --- |
| `enabled` | Master switch for future bridge endpoints |
| `bind_mode` | `localhost` or `lan` / `move.local` |
| `require_token` | Require token for write/control endpoints |
| `allow_read` | Permit status/list/read operations |
| `allow_write` | Permit upload/write operations |
| `allow_delete` | Permit pack/file deletion |
| `allow_actions` | Permit actions such as load-pack or preview |
| `max_upload_mb` | Upload size cap |
| `pack_root` | Root for Move MPC pack storage |
| `log_requests` | Log mutating bridge requests |

Launch `Move MCP` from the Tools menu to see the current config on Move's
display. Edit settings from Schwung Manager at `http://move.local:7700`.

Schwung Manager also exposes the first Move MCP JSON endpoints:

```text
GET  /api/mcp/status
GET  /api/mcp/capabilities
GET  /api/mcp/sets
POST /api/mcp/sets/select
POST /api/mcp/sets/duplicate
GET  /api/mcp/current-set
GET  /api/mcp/current-set/settings
POST /api/mcp/current-set/settings
POST /api/mcp/current-set/clips
GET  /api/mcp/current-set/tracks/<track>/devices
POST /api/mcp/current-set/tracks/<track>/devices
POST /api/mcp/current-set/tracks/<track>/devices/parameters
GET  /api/mcp/samples
POST /api/mcp/samples/index
```

These endpoints are controlled by the `move-mcp` module settings. Reads require
`enabled` + `allow_read`; writes require `enabled` + `allow_write`. If
`require_token` is enabled, clients must send `Authorization: Bearer <token>` or
`X-Move-MCP-Token: <token>`.

The current API can read current-set metadata, tracks, devices/effects, clip
notes, clip envelopes, and per-note automations from `Song.abl`. It can write
song settings such as tempo, global groove amount, scale, melodic layout, step
resolution, and time signature. It can also write one clip's note list,
per-note automations, and clip envelopes with a timestamped backup. It can read
and write one track's raw device tree, including instruments and effects.
For smaller edits, it can update selected parameters on a nested device/effect
without replacing the whole tree.
Device writes are structurally validated, but still advanced: preset-backed
devices must keep their Ableton `lockId` and `lockSeal`, and the safest
supported operation is copying a complete device chain from another existing
track.

It can also list Move sets, switch the active set by updating
`Settings.json/currentSongIndex`, and duplicate an existing set under a fresh
UUID. Switching restarts Move by default because the official UI loads the
selected set during startup. Creating a blank set from scratch is intentionally
not exposed yet; duplicating a known-good set preserves Ableton's required
device, lock, and metadata structure.

Move MCP can store a Mac-generated sample index at
`/data/UserData/schwung/move-mcp/sample-index.json`. The helper script
`src/modules/tools/move-mcp/scan_samples.py` scans a local sample folder, maps
paths to `/data/UserData/UserLibrary/Samples`, infers lightweight metadata from
headers and filenames, and uploads the result to Manager. This keeps heavier
analysis off Move while making samples searchable over WiFi.

## Installing and Enabling

For local development, build and install Schwung normally:

```sh
./scripts/build.sh
./scripts/install.sh local --skip-confirmation
```

On Move, open Schwung Manager at `http://move.local:7700`, then open the
`move-mcp` module settings. The safest first configuration is:

```text
Enable Bridge: on
Require Token: on
Allow Read: on
Allow Write: off
Allow Delete: off
Allow Actions: off
```

With that configuration, assistant clients can inspect status, sets, clips,
devices, and sample metadata, but cannot change the current set. Turn on
`Allow Write` only while actively testing write workflows. Turn on
`Allow Actions` only when you want clients to select sets or trigger other
state-changing actions. Keep `Allow Delete` off unless a client genuinely needs
pack deletion.

If `Require Token` is enabled, set an access token in Schwung Manager and pass
it from clients as either:

```text
Authorization: Bearer <token>
X-Move-MCP-Token: <token>
```

Smoke-test the bridge from the computer on the same network:

```sh
curl http://move.local:7700/api/mcp/status
curl -H "Authorization: Bearer $MOVE_MCP_TOKEN" \
  http://move.local:7700/api/mcp/current-set
```

## Using With LLM Clients

Move does not need to run a full Model Context Protocol server on-device for
the first version. The recommended shape is:

```text
Claude / Codex / Cursor / other MCP client
  -> desktop stdio MCP server
  -> http://move.local:7700/api/mcp/*
  -> Schwung Manager on Move
```

Run the desktop MCP server on the Mac, PC, or Linux machine that already has
network access to Move. Configure it with:

```text
MOVE_MCP_BASE_URL=http://move.local:7700
MOVE_MCP_TOKEN=<token from Schwung Manager>
```

For read-only use, expose only safe tools such as:

| Tool | HTTP call |
| --- | --- |
| `move_status` | `GET /api/mcp/status` |
| `move_capabilities` | `GET /api/mcp/capabilities` |
| `move_list_sets` | `GET /api/mcp/sets` |
| `move_current_set` | `GET /api/mcp/current-set` |
| `move_song_settings` | `GET /api/mcp/current-set/settings` |
| `move_track_devices` | `GET /api/mcp/current-set/tracks/<track>/devices` |
| `move_search_samples` | `GET /api/mcp/samples` |

For write-capable profiles, add tools only after enabling the matching
permission in Schwung Manager:

| Tool | Required setting | HTTP call |
| --- | --- | --- |
| `move_write_song_settings` | `allow_write` | `POST /api/mcp/current-set/settings` |
| `move_write_clip` | `allow_write` | `POST /api/mcp/current-set/clips` |
| `move_write_track_devices` | `allow_write` | `POST /api/mcp/current-set/tracks/<track>/devices` |
| `move_write_device_parameters` | `allow_write` | `POST /api/mcp/current-set/tracks/<track>/devices/parameters` |
| `move_select_set` | `allow_actions` | `POST /api/mcp/sets/select` |
| `move_duplicate_set` | `allow_write` | `POST /api/mcp/sets/duplicate` |
| `move_upload_sample_index` | `allow_write` | `POST /api/mcp/samples/index` |

Most desktop MCP clients accept a command plus environment variables. A typical
client entry should point at the local proxy, not at Move directly:

```json
{
  "mcpServers": {
    "move-mcp": {
      "command": "move-mcp-server",
      "env": {
        "MOVE_MCP_BASE_URL": "http://move.local:7700",
        "MOVE_MCP_TOKEN": "replace-with-your-token"
      }
    }
  }
}
```

If a client does not support MCP servers, use the same HTTP endpoints directly
from scripts or custom tools. Keep destructive or restart-capable actions behind
separate tool names so the user can see what kind of operation is being
requested.

## Goals

- Control Schwung over WiFi from a Mac, phone, or local agent.
- Let Codex or another assistant generate loop packs, MIDI clips, sample maps,
  and patch metadata on the computer, then push them to Move.
- Keep Move's realtime audio/SPI path untouched.
- Use explicit, small, auditable commands rather than arbitrary shell access.
- Make the same bridge useful for browser UI, scripts, and Model Context
  Protocol-style tools.

## Non-Goals

- Do not expose raw root shell over HTTP.
- Do not run LLM inference on Move for the first version.
- Do not generate audio in the realtime callback.
- Do not require a phone or computer for normal Move playback once content is
  loaded.
- Do not try to replace Schwung Manager immediately.

## Connection Model

Preferred first version:

```text
Mac / phone / Codex
  -> WiFi / USB-C gadget ethernet
  -> http://move.local:7700 or a future MCP endpoint
  -> schwung-manager / move-mcp bridge
  -> files + shared memory + module state
  -> Move MPC or other Schwung modules
```

Move already exposes Schwung Manager at `http://move.local:7700`, and the current
architecture treats that web UI as a trusted-network tool. Move MCP should either
extend Schwung Manager with a small JSON API or run as a sibling service that
uses the same safety rules.

USB-C connected to a Mac is useful for debugging because Move normally exposes
USB ethernet/NCM in gadget mode. WiFi should still be the default mental model,
because USB-C may be needed for other host-mode experiments.

## Security Model

Move MCP must be designed as a trusted-local-network interface.

Initial safety rules:

- Bind to localhost or the existing `move.local` LAN interface only.
- Require an explicit feature flag before enabling write/control endpoints.
- Store generated files only under `/data/UserData/schwung/` or documented user
  library folders.
- Reject `..`, absolute-path traversal, symlink escapes, and shell metacharacter
  execution.
- Prefer JSON commands with schema validation over command strings.
- Log every mutating request to `/data/UserData/schwung/move-mcp.log`.
- Include a global disable switch in Schwung Manager settings.

If exposed beyond a private LAN, add authentication before any mutating endpoint.
Schwung Manager is currently unauthenticated by default, so treat this as a
local studio tool, not an internet service.

## First Consumer: Move MPC Loop Packs

The first useful workflow can be file-based:

```text
Codex prompt:
  "Make a 90 BPM dusty boom-bap loop with 8 pad chops"

Codex / desktop generator:
  creates pack.json
  creates pattern JSON / MIDI
  optionally creates or references WAV samples

Move MCP:
  uploads pack to /data/UserData/schwung/move-mpc/packs/<pack-id>/

Move MPC module:
  scans packs
  maps pads to chops/loops
  plays or routes MIDI/audio
```

This avoids needing realtime network control for the first version. The network
side only uploads a pack. The performance module reads files locally.

## Pack Directory

Suggested location:

```text
/data/UserData/schwung/move-mpc/
  packs/
    <pack-id>/
      pack.json
      samples/
        kick.wav
        snare.wav
        chop-01.wav
      patterns/
        main.json
        fill-a.json
      renders/
        preview.wav
```

Pack IDs should be lowercase URL-safe strings:

```text
dusty-boom-bap-90
minimal-techno-128
ambient-breaks-82
```

## Pack JSON v1

Minimal schema:

```json
{
    "schema": "move-mpc-pack-v1",
    "id": "dusty-boom-bap-90",
    "name": "Dusty Boom Bap 90",
    "bpm": 90,
    "bars": 4,
    "swing": 0.56,
    "author": "Codex",
    "created_at": "2026-06-15T12:00:00Z",
    "pads": [
        {
            "pad": 1,
            "label": "Kick",
            "type": "sample",
            "path": "samples/kick.wav",
            "mode": "one_shot",
            "gain_db": -3.0
        },
        {
            "pad": 2,
            "label": "Snare",
            "type": "sample",
            "path": "samples/snare.wav",
            "mode": "one_shot",
            "gain_db": -2.0
        }
    ],
    "patterns": [
        {
            "id": "main",
            "name": "Main",
            "path": "patterns/main.json"
        }
    ]
}
```

Pattern JSON v1:

```json
{
    "schema": "move-mpc-pattern-v1",
    "id": "main",
    "bars": 4,
    "steps_per_bar": 16,
    "events": [
        { "step": 0,  "pad": 1, "velocity": 110 },
        { "step": 4,  "pad": 2, "velocity": 100 },
        { "step": 8,  "pad": 1, "velocity": 105 },
        { "step": 12, "pad": 2, "velocity": 96 }
    ]
}
```

Keep v1 intentionally boring. The first win is reliable transfer and playback,
not a maximal groove format.

## Proposed HTTP API

If implemented in Schwung Manager:

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/api/mcp/status` | Bridge enabled, host version, active module, storage summary |
| `GET` | `/api/mcp/capabilities` | Supported schemas, endpoints, max upload size |
| `GET` | `/api/mcp/sets` | List Move sets, song indexes, paths, and current selection |
| `POST` | `/api/mcp/sets/select` | Select an existing set by UUID, name, or song index; optionally save dirty state and restart Move |
| `POST` | `/api/mcp/sets/duplicate` | Duplicate an existing set under a new name; optionally select and restart into it |
| `GET` | `/api/mcp/current-set` | Read current set metadata, tracks, devices/effects, clips, notes, envelopes, and note automations |
| `GET` | `/api/mcp/current-set/settings` | Read song-level settings such as tempo, groove amount, scale, melodic layout, step resolution, and time signature |
| `POST` | `/api/mcp/current-set/settings` | Update song-level settings in the current set with a `Song.abl` backup |
| `POST` | `/api/mcp/current-set/clips` | Create or replace one clip's notes in the current set, with a `Song.abl` backup |
| `GET` | `/api/mcp/current-set/tracks/<track>/devices` | Read a track's raw Ableton device tree for exact copy/edit workflows |
| `POST` | `/api/mcp/current-set/tracks/<track>/devices` | Replace a track's raw device tree from `source_track` or a validated `devices` array |
| `POST` | `/api/mcp/current-set/tracks/<track>/devices/parameters` | Update named parameters on one nested device/effect addressed by `device_path` |
| `GET` | `/api/mcp/samples` | Query the uploaded sample index by text, tag, key, BPM range, and limit |
| `POST` | `/api/mcp/samples/index` | Upload a complete `move-mcp-sample-index-v1` database generated on a Mac |
| `GET` | `/api/mcp/packs` | List installed Move MPC packs |
| `POST` | `/api/mcp/packs` | Upload a pack manifest and assets as multipart/form-data |
| `GET` | `/api/mcp/packs/<id>` | Read pack metadata |
| `DELETE` | `/api/mcp/packs/<id>` | Remove a pack |
| `POST` | `/api/mcp/actions/load-pack` | Ask Move MPC to load a pack if active |
| `POST` | `/api/mcp/actions/preview` | Ask device to preview a sample or render |

All paths are suggestions. The important boundary is: file transfer and validated
module actions, not arbitrary shell commands.

## Model Context Protocol Shape

For Codex or other MCP clients, the most natural implementation may be a
desktop-side MCP server that talks to Move over HTTP/SSH rather than running MCP
directly on Move.

Suggested MCP tools:

| Tool | Action |
| --- | --- |
| `move_status` | Probe `move.local`, Schwung version, storage, active module |
| `move_upload_pack` | Validate and upload a Move MPC pack |
| `move_list_packs` | List installed packs |
| `move_load_pack` | Request active Move MPC module to load a pack |
| `move_tail_log` | Read recent Schwung or Move MCP log lines |
| `move_collect_diagnostics` | Trigger existing diagnostics collection |

Suggested MCP resources:

| Resource | Contents |
| --- | --- |
| `move://status` | Current device and Schwung status |
| `move://packs` | Installed Move MPC packs |
| `move://schemas/move-mpc-pack-v1` | Pack schema |
| `move://logs/move-mcp` | Recent bridge log |

This keeps the heavyweight assistant-facing protocol on the Mac while Move stays
focused on validated local APIs.

## Move MPC Module Sketch

Working module path:

```text
src/modules/overtake/move-mpc/
```

Minimum behavior:

- scan `/data/UserData/schwung/move-mpc/packs/`;
- show pack list on Move's display;
- load `pack.json`;
- map pads 1-16 to pack pads;
- start/stop a simple pattern;
- expose a small status file or shared-memory state for Move MCP.

Controls:

| Move control | Action |
| --- | --- |
| Pads | Trigger samples/chops |
| Steps | Toggle pattern events |
| Jog | Select pack/pattern/page |
| Jog Click | Load/confirm |
| Play | Start/stop pattern |
| Back | Exit module |

The first implementation can trigger samples through existing Schwung preview or
sampler paths if that is enough. A later version can add a native DSP engine for
tighter timing.

## Data Flow Options

### Option A: File-Based Packs

Best first step.

- Codex generates files.
- Move MCP uploads files.
- Move MPC reads files locally.
- No realtime network dependency.

### Option B: WebSocket Live Control

Useful later for phone/desktop live editing.

- Browser or desktop app keeps a WebSocket open.
- Sends edits such as `set_step`, `trigger_pad`, `load_pack`.
- Move MCP validates and writes to a command queue.

Avoid relying on WebSocket timing for musical clock. Use it for editing and
preview, not sample-accurate playback.

### Option C: Remote UI Iframe

Good for a browser editor embedded in Schwung Manager.

- Move MPC ships `web_ui.html`.
- Browser UI edits parameters or pack metadata.
- Existing Remote UI bridge handles module parameters where possible.

This is attractive for human editing, but file upload/generation still belongs
in Move MCP or Schwung Manager.

## Phase Plan

### Phase 0: Spec and Fixtures

- Add this spec.
- Add the minimal `move-mcp` config/status tool.
- Add example `pack.json` and `pattern.json` fixtures.
- Validate JSON on desktop.

### Phase 1: Upload API

- Add a guarded endpoint to Schwung Manager or a tiny sibling service.
- Upload a pack into `/data/UserData/schwung/move-mpc/packs/<id>/`.
- Reject invalid paths and invalid schema.
- List/delete packs.

### Phase 2: Probe Module

- Add minimal `move-mpc` overtake module.
- Display installed packs.
- Load pack metadata.
- Trigger placeholder events/logs.

### Phase 3: Playback

- Implement local pattern playback.
- Trigger samples or route MIDI into existing Schwung chain paths.
- Keep timing local to Move, not network-driven.

### Phase 4: Codex/MCP Tooling

- Desktop MCP server discovers Move at `move.local`.
- Validates generated packs.
- Uploads packs.
- Reads logs and status.

### Phase 5: Phone / Desktop Editor

- Browser UI for pack editing and live preview.
- Optional WebSocket for state updates.
- Optional templates: drums, chopped sample, bassline, ambient loop.

## Open Questions

- Should Move MCP live inside `schwung-manager`, or as a separate sidecar?
- Should first playback be sample-based, MIDI-only, or both?
- What is the safest way for a module to expose "load pack now" to the manager:
  status file, shared memory, or an existing parameter path?
- How large should pack uploads be allowed to be?
- Do we want an explicit pairing/token flow for write access?

## Acceptance Test for First Real Version

1. Mac reaches `http://move.local:7700`.
2. Move MCP reports enabled status.
3. A generated `move-mpc-pack-v1` fixture uploads successfully.
4. The pack appears in `/data/UserData/schwung/move-mpc/packs/`.
5. Move MPC module lists the pack on-device.
6. Deleting the pack through the API removes only that pack directory.
7. Logs show each mutating request.
