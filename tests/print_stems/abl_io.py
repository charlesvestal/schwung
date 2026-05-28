"""Song.abl I/O helpers for print-stems.

Mirrors src/modules/tools/song-mode/ui.js:parseSong for clip-grid extraction.
"""
import urllib.parse

NUM_TRACKS = 4
NUM_COLS = 8


def parse_clip_grid(song):
    """Extract a Move-shaped 4x8 clip grid from a Song.abl dict.

    Returns:
        {
            "tempo": float,
            "num_tracks": int,        # min(len(song.tracks), 4)
            "time_signature": {"upper": int, "lower": int},
            "cells": 4 x 8 list of cell dicts (see below)
        }

    Each cell:
        {
            "exists": bool,
            "beats": float,           # 0.0 if not exists
            "loop_enabled": bool,
            "color": int or None,
            "kind": str,              # "audio" or "midi" from the source track,
                                      # or "" if the source has fewer than 4 tracks
                                      # and this row has no corresponding source track.
        }
    """
    tracks = song.get("tracks") or []
    num_tracks = min(len(tracks), NUM_TRACKS)

    cells = []
    for t in range(NUM_TRACKS):
        track = tracks[t] if t < len(tracks) else None
        kind = (track or {}).get("kind", "") if track else ""
        row = []
        slots = (track or {}).get("clipSlots") or []
        for c in range(NUM_COLS):
            slot = slots[c] if c < len(slots) else None
            clip = (slot or {}).get("clip") if slot else None
            if not clip:
                row.append({
                    "exists": False,
                    "beats": 0.0,
                    "loop_enabled": False,
                    "color": None,
                    "kind": kind,
                })
                continue

            region = clip.get("region") or {}
            loop = region.get("loop") or {}
            loop_enabled = bool(loop.get("isEnabled"))
            if loop_enabled and loop.get("end", 0) > loop.get("start", 0):
                beats = loop["end"] - loop["start"]
            else:
                beats = (region.get("end") or 0) - (region.get("start") or 0)

            row.append({
                "exists": True,
                "beats": float(beats),
                "loop_enabled": loop_enabled,
                "color": clip.get("color"),
                "kind": kind,
            })
        cells.append(row)

    return {
        "tempo": song.get("tempo"),
        "num_tracks": num_tracks,
        "time_signature": song.get("timeSignature") or {"upper": 4, "lower": 4},
        "cells": cells,
    }


def sample_uri(filename):
    """Encode a bare WAV filename into a user-library Recordings URI.

    Uses urllib.parse.quote with safe="" so that every reserved character
    (including "/") is percent-encoded. This matches the on-device URI shape
    Move firmware writes for clip sampleUri fields.
    """
    return "ableton:/user-library/Recordings/" + urllib.parse.quote(filename, safe="")


def build_stems_song_abl(source_song, grid, set_name, stem_filename_for):
    """Build a sibling audio-clip Song.abl from the parsed source.

    See the print-stems plan / docstring in the test file for the field spec.
    """
    src_tracks = source_song.get("tracks") or []
    tempo = source_song.get("tempo")

    tracks = []
    for t in range(NUM_TRACKS):
        src_track = src_tracks[t] if t < len(src_tracks) else None
        name = (src_track or {}).get("name", "") if src_track else ""
        color = (src_track or {}).get("color", 1) if src_track else 1

        clip_slots = []
        for c in range(NUM_COLS):
            cell = grid["cells"][t][c]
            if not cell["exists"]:
                clip_slots.append({"hasStop": True, "clip": None})
                continue
            beats = cell["beats"]
            clip = {
                "name": "",
                "color": cell["color"],
                "isEnabled": True,
                "timeSignature": {"upper": 4, "lower": 4},
                "region": {
                    "start": 0.0,
                    "end": beats,
                    "loop": {
                        "start": 0.0,
                        "end": beats,
                        "isEnabled": cell["loop_enabled"],
                    },
                },
                "stepEditorScrollPosition": 0.0,
                "sampleUri": sample_uri(stem_filename_for(t, c)),
                "warping": {
                    "markers": [],
                    "tempoAfterLastMarker": tempo,
                },
                "gain": 0.0,
                "transpose": 0,
                "detune": 0.0,
                "envelopes": [],
            }
            clip_slots.append({"hasStop": True, "clip": clip})

        tracks.append({
            "kind": "audio",
            "name": name,
            "color": color,
            "isSelected": False,
            "clipSlots": clip_slots,
            "devices": [],
            "mixer": {
                "pan": 0.0,
                "solo-cue": False,
                "speakerOn": True,
                "volume": 0.0,
                "sends": [],
            },
        })

    out = {}
    for k in (
        "$schema",
        "stepEditorResolution",
        "tempo",
        "globalGrooveAmount",
        "timeSignature",
        "rootNote",
        "scale",
        "melodicLayout",
    ):
        if k in source_song:
            out[k] = source_song[k]
    out["tracks"] = tracks
    for k in ("returnTracks", "masterTrack", "scenes", "grooves", "metadata"):
        if k in source_song:
            out[k] = source_song[k]
    return out
