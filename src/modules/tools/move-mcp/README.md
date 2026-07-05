# Move MCP Module

Installable configuration/status module for the planned Move MCP WiFi bridge.

This module does not serve HTTP itself. It gives Schwung Manager a per-module
settings surface for bridge enablement, read/write permissions, delete/action
permissions, upload limits, bind mode, and optional token auth.

Open Schwung Manager at `http://move.local:7700`, edit the module settings, then
launch `Move MCP` from Tools to inspect the current on-device config.

## Enabling

Recommended first setup:

```text
Enable Bridge: on
Require Token: on
Allow Read: on
Allow Write: off
Allow Delete: off
Allow Actions: off
```

This lets assistant clients inspect Move state without modifying sets. Enable
`Allow Write` only while testing write workflows. Enable `Allow Actions` only
when a client should be allowed to select sets, restart into a set, or trigger
other state-changing operations.

Smoke-test from a computer on the same network:

```sh
curl http://move.local:7700/api/mcp/status
curl -H "Authorization: Bearer $MOVE_MCP_TOKEN" \
  http://move.local:7700/api/mcp/current-set
```

## LLM and MCP Clients

Move serves a guarded HTTP API through Schwung Manager. For Claude, Codex,
Cursor, or another MCP-capable client, run a small desktop MCP server on the
computer and have that server call Move's HTTP endpoints:

```text
LLM client -> desktop MCP server -> http://move.local:7700/api/mcp/*
```

Configure the desktop server with:

```text
MOVE_MCP_BASE_URL=http://move.local:7700
MOVE_MCP_TOKEN=<token from Schwung Manager>
```

Typical MCP client entry:

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

The first desktop server profile should expose read-only tools (`status`,
`capabilities`, `sets`, `current-set`, `samples`). Add write/action tools only
when the matching Schwung Manager permission is enabled, and send
`Authorization: Bearer <token>` or `X-Move-MCP-Token: <token>` on every
mutating request. Without a valid MCP token, POST requests must satisfy Schwung
Manager's normal browser CSRF flow.

## HTTP API

The first JSON endpoints are served by Schwung Manager:

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

`/api/mcp/status` reports whether the bridge is enabled and which capabilities
are available. It is always readable so clients can explain what to enable.

`/api/mcp/current-set` requires `enabled=true` and `allow_read=true` in this
module's settings. It returns the current set metadata plus normalized tracks,
devices/effects, clip notes, clip envelopes, and per-note automations.

`/api/mcp/current-set/settings` reads or writes song-level settings such as
tempo, global groove amount, root note, scale, melodic layout, step editor
resolution, and time signature. The write endpoint requires `enabled=true` and
`allow_write=true` and backs up `Song.abl` before changing it.

`/api/mcp/sets` requires `enabled=true` and `allow_read=true`. It lists Move
sets by reading `/data/UserData/UserLibrary/Sets`, each set directory's
`user.song-index` xattr, and the current `Settings.json` song index.

`/api/mcp/sets/select` requires `enabled=true`, `allow_read=true`, and
`allow_actions=true`. It updates `Settings.json` to point at an existing set and
restarts Move by default so the selection is loaded by the official UI.

`/api/mcp/sets/duplicate` requires `enabled=true` and `allow_write=true`. It
copies an existing set directory, assigns a fresh UUID and next `user.song-index`
xattr, then optionally selects and restarts into the duplicate when
`allow_actions=true`.

`/api/mcp/current-set/clips` requires `enabled=true` and `allow_write=true`. It
backs up the current `Song.abl`, then creates or replaces one clip's note list.

`/api/mcp/current-set/tracks/<track>/devices` reads or writes one track's raw
Ableton device tree. The write endpoint requires `enabled=true` and
`allow_write=true`, backs up `Song.abl`, and validates device structure before
writing. The safest write mode is copying devices from another existing track.
Raw device payloads are accepted only when each device has a `kind`, preset
devices retain `lockId` and `lockSeal`, nested chains/devices stay within size
limits, and preset URIs use `ableton:/` or `file:` schemes.

When `require_token=true`, pass the token as either:

```text
Authorization: Bearer <token>
X-Move-MCP-Token: <token>
```

Minimal write body:

```json
{
  "track": 4,
  "scene": 1,
  "name": "Dub MCP",
  "end": 4.0,
  "loop_end": 4.0,
  "notes": [
    { "noteNumber": 60, "startTime": 0.0, "duration": 0.25, "velocity": 96, "offVelocity": 0 }
  ]
}
```

Set tempo and groove:

```json
{
  "tempo": 82,
  "global_groove_amount": 0.55
}
```

Select Set 33 and restart Move:

```json
{
  "name": "Set 33",
  "restart": true
}
```

Duplicate the current set, select it, and restart into it:

```json
{
  "name": "Dub MCP Sketch",
  "select_new": true,
  "restart": true
}
```

Copy the instrument/effect chain from track 3 to track 4:

```json
{
  "source_track": 3,
  "track_name": "Dub Chords"
}
```

Raw device writes use the same endpoint with a full `devices` array, usually
copied from `GET /api/mcp/current-set/tracks/<track>/devices`:

```json
{
  "devices": [
    {
      "kind": "instrumentRack",
      "name": "Chicago Kit",
      "presetUri": "ableton:/packs/abl-core-library/Track%20Presets/Drums/Electronic/Chicago%20Kit.json",
      "lockId": 1001,
      "lockSeal": -973461132
    }
  ]
}
```

For smaller effect edits, update parameters on one nested device/effect. The
`device_path` starts at the track's top-level `devices` array, then walks through
the first chain at each nested level:

```json
{
  "device_path": [0, 0, 2],
  "parameters": {
    "DryWet": 0.28,
    "Feedback": 0.35
  }
}
```

## Sample Index

Move MCP stores an optional searchable sample database at:

```text
/data/UserData/schwung/move-mcp/sample-index.json
```

Build it on a Mac and upload it to Move:

```sh
python3 src/modules/tools/move-mcp/scan_samples.py \
  "/path/to/your/Move Samples" \
  --move-root /data/UserData/UserLibrary/Samples \
  --output sample-index.json \
  --upload http://move.local:7700 \
  --token "$MOVE_MCP_TOKEN"
```

The scanner reads WAV headers with Python's standard library, reads AIFF when
the local Python still provides `aifc`, optionally uses `ffprobe` when available
for AIFF/MP3/FLAC/M4A/OGG metadata, and infers BPM, key, tags, one-shot/loop
hints, mood, and loop bars from filenames and folders.

Query the uploaded index:

```text
GET /api/mcp/samples?q=vocal&bpm_min=124&bpm_max=132&tag=dub&limit=25
```

`POST /api/mcp/samples/index` requires `enabled=true` and `allow_write=true`.
`GET /api/mcp/samples` requires `enabled=true` and `allow_read=true`.

Backups are written under:

```text
/data/UserData/schwung/move-mcp/backups/<set-uuid>/<timestamp>/Song.abl
```

The full bridge specification lives in
[`docs/MOVE_MCP.md`](../../../../docs/MOVE_MCP.md).

Current reverse-engineering notes for reading Move set files live in
[`song-abl-notes.md`](song-abl-notes.md).
