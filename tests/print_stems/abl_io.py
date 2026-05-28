"""Song.abl I/O helpers for print-stems.

Mirrors src/modules/tools/song-mode/ui.js:parseSong for clip-grid extraction.
"""

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
