# Song.abl Notes for Move MCP

These notes come from inspecting an on-device Ableton Move set over SSH while
designing the Move MCP bridge. They are implementation notes, not a stable public
Ableton file-format contract.

## Location

Move stores user sets under:

```text
/data/UserData/UserLibrary/Sets/<uuid>/<set name>/Song.abl
```

The current set index is stored in:

```text
/data/UserData/settings/Settings.json
```

`Settings.json` contains `currentSongIndex`. The corresponding set directory can
be found by reading the `user.song-index` extended attribute on each UUID
directory under `UserLibrary/Sets`.

Example inspected on device:

```text
currentSongIndex: 27
/data/UserData/UserLibrary/Sets/d918b52d-c9fb-491a-8ac4-ea3d191713af/Set 33/
  user.song-index="27"
  Song.abl
```

## Set Selection and Duplication

Move's active set selection is file-backed. Schwung can switch sets by:

1. Asking the Move browser service to save the current song if dirty.
2. Updating `/data/UserData/settings/Settings.json` so `currentSongIndex` equals
   the target set directory's `user.song-index` xattr.
3. Restarting Move so the official UI loads the new selection.

The MCP implementation exposes this as `POST /api/mcp/sets/select`. It requires
`allow_actions=true` because it changes device state and normally restarts the
Move UI.

Creating a truly blank set should wait until we have an official or
known-compatible template. For now, the safer primitive is duplicating an
existing set directory, assigning a fresh UUID folder and next `user.song-index`
xattr, then optionally selecting that duplicate. This keeps Ableton's nested
device structures, lock fields, and metadata intact.

## File Shape

`Song.abl` is UTF-8 JSON. The inspected Set 33 file used:

```json
{
  "$schema": "http://tech.ableton.com/schema/song/1.8.3/song.json",
  "stepEditorResolution": "1/16",
  "tempo": 127.0,
  "timeSignature": { "upper": 4, "lower": 4 },
  "rootNote": 7,
  "scale": "Minor",
  "tracks": [],
  "scenes": []
}
```

Top-level keys observed:

```text
$schema
stepEditorResolution
tempo
globalGrooveAmount
timeSignature
rootNote
scale
melodicLayout
tracks
returnTracks
masterTrack
scenes
grooves
metadata
```

## Tracks

The inspected set had four MIDI tracks. Each track had:

```text
kind
name
color
isSelected
clipSlots
isNoteRepeatOn
noteRepeatRate
noteRepeatArpeggio
uiOctaveIndex
midiInputMode
midiOutputEndpoint
devices
mixer
```

Useful fields for a first read API:

```json
{
  "kind": "midi",
  "name": "",
  "color": 12,
  "isSelected": false,
  "midiInputMode": "auto",
  "mixer": {
    "pan": 0.0,
    "solo-cue": false,
    "speakerOn": true,
    "volume": 0.0,
    "sends": []
  }
}
```

Track devices include enough metadata to identify the loaded preset:

```json
{
  "kind": "instrumentRack",
  "name": "Chicago Kit",
  "presetUri": "ableton:/packs/abl-core-library/Track%20Presets/Drums/Electronic/Chicago%20Kit.json",
  "lockId": 1001,
  "lockSeal": -973461132
}
```

Set 33 track summary at inspection time:

```text
Track 1: Chicago Kit, clip slot 1 contained 13 notes
Track 2: Fifth Bass, clip slot 1 contained 11 notes
Track 3: Dance Keys, clip slot 1 contained 3 notes
Track 4: Thumb Piano Tiny, no clips
```

Move MCP now treats `tracks[n].devices` as the raw instrument/effect tree for
write-back. The normalized current-set response summarizes devices for compact
inspection, while:

```text
GET /api/mcp/current-set/tracks/<track>/devices
```

returns the raw JSON array used by Ableton. The matching write endpoint can
replace that array either by copying `source_track` from the same set or by
accepting a raw `devices` array. Copying from an existing track is the safer
first workflow because it preserves Ableton-specific lock fields and nested
chain metadata exactly.

Validation rules for device writes are intentionally conservative:

- Top-level devices must be non-empty and limited in count.
- Each device must be an object with a non-empty `kind`.
- Devices with `presetUri` must keep `lockId` and `lockSeal`.
- `presetUri` must use `ableton:/` or `file:`.
- Nested `chains`, `returnChains`, and `devices` are bounded by size and depth.
- Track name/color changes are optional and validated separately.

## Clips and Notes

Clips live at:

```text
tracks[track_index].clipSlots[scene_index].clip
```

An occupied clip slot had this shape:

```json
{
  "hasStop": true,
  "clip": {
    "isPlaying": true,
    "name": "",
    "color": 12,
    "isEnabled": true,
    "timeSignature": { "upper": 4, "lower": 4 },
    "region": {
      "start": 0.0,
      "end": 4.0,
      "loop": { "start": 0.0, "end": 4.0, "isEnabled": true }
    },
    "grooveId": 1,
    "stepEditorScrollPosition": 0,
    "notes": [],
    "envelopes": []
  }
}
```

Notes are simple objects in beat units:

```json
{
  "noteNumber": 36,
  "startTime": 0.0,
  "duration": 0.25,
  "velocity": 127.0,
  "offVelocity": 0.0
}
```

Some notes can carry automation, for example pressure:

```json
{
  "noteNumber": 67,
  "startTime": 0.047789450133200136,
  "duration": 0.8614018793706294,
  "velocity": 127.0,
  "offVelocity": 0.0,
  "automations": {
    "Pressure": [{ "time": 0.6327922077922078, "value": 0.0 }]
  }
}
```

Clip length is represented by `clip.region.end - clip.region.start`; loop length
is represented by `clip.region.loop.end - clip.region.loop.start`.

## Read API Candidate

A first Move MCP read endpoint can safely return a normalized subset:

```json
{
  "set": {
    "uuid": "d918b52d-c9fb-491a-8ac4-ea3d191713af",
    "name": "Set 33",
    "song_index": 27,
    "tempo": 127.0,
    "scale": "Minor",
    "root_note": 7,
    "time_signature": { "upper": 4, "lower": 4 }
  },
  "tracks": [
    {
      "index": 1,
      "kind": "midi",
      "device_name": "Chicago Kit",
      "preset_uri": "ableton:/packs/abl-core-library/Track%20Presets/Drums/Electronic/Chicago%20Kit.json",
      "clips": [
        {
          "scene": 1,
          "start": 0.0,
          "end": 4.0,
          "loop_start": 0.0,
          "loop_end": 4.0,
          "notes": []
        }
      ]
    }
  ]
}
```

## Write-Back Strategy

Direct `Song.abl` mutation looks feasible because clips and notes are plain JSON,
but it should be treated as experimental until proven across firmware versions
and set states.

Recommended safety rules:

- Never edit `Song.abl` while Move may have unsaved in-memory changes.
- Ask Move to save first when a reliable save trigger is available.
- Snapshot the original file before mutation:

  ```text
  /data/UserData/schwung/move-mcp/backups/<uuid>/<timestamp>/Song.abl
  ```

- Validate JSON before and after edits.
- Preserve unknown fields exactly.
- Start with additive edits to an empty clip slot.
- Prefer creating or replacing `clip.notes` over editing device internals first.
- Log every write under `/data/UserData/schwung/move-mcp.log`.

For the user-facing workflow "use tracks 1-3, create a 4-bar dub techno idea on
track 4", a conservative first implementation should:

1. Read tempo, scale, track devices, and existing clip notes from tracks 1-3.
2. Generate a new clip object for track 4 scene 1.
3. Write only `tracks[3].clipSlots[0].clip` and leave track 4's device alone.
4. Let the user reload/open the set and verify before adding device/preset
   replacement.

Changing the actual instrument or kit on a track requires copying or generating
the `devices` tree, including fields such as `lockId`, `lockSeal`, `parameters`,
and nested chains. That is possible later, but it has a larger compatibility
surface than note-clip edits.

## Implemented Manager API

Initial Schwung Manager endpoints:

```text
GET  /api/mcp/status
GET  /api/mcp/current-set
POST /api/mcp/current-set/clips
```

`GET /api/mcp/current-set` returns a normalized view of:

- set metadata;
- tracks;
- track mixer state;
- device/effect summaries, including nested chains and parameter maps;
- occupied clip slots;
- clip notes;
- clip envelopes;
- per-note automations.

`POST /api/mcp/current-set/clips` writes only one clip's note list. It mutates
the raw decoded JSON tree and then reserializes it, so unknown Ableton fields
outside the edited clip path are preserved. The endpoint creates a timestamped
backup before writing.

Instrument/device write-back is intentionally not implemented in the first API.
The read endpoint exposes enough device detail to let a desktop MCP client
reason about the loaded instrument/effects, but replacing those trees needs
more compatibility testing.

## Open Questions

- Does Move hot-reload `Song.abl`, or does the set need to be reopened?
- Can a D-Bus or HTTP call reliably trigger `saveSongIfDirty` before editing?
- Are `lockId` and `lockSeal` validated when replacing track devices?
- Do audio clips and sample references use the same note-clip structure or a
  separate schema branch?
- How does Move handle a new clip with `isPlaying: false` versus `true` when the
  set is reopened?
