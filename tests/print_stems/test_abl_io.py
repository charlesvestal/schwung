"""Tests for tests/print_stems/abl_io.py — Song.abl parser."""
import json
import os

from abl_io import parse_clip_grid

FIXTURE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "fixtures"
)


def test_set26_clip_grid():
    src = json.load(open(os.path.join(FIXTURE_DIR, "set26-source.abl")))
    grid = parse_clip_grid(src)
    assert grid["tempo"] == 112.0
    assert grid["num_tracks"] == 4
    assert grid["time_signature"] == {"upper": 4, "lower": 4}
    # T0/C0: 16 beats, loop_enabled, audio, color=1
    cell = grid["cells"][0][0]
    assert cell["exists"] is True
    assert cell["beats"] == 16.0
    assert cell["loop_enabled"] is True
    assert cell["color"] == 1
    assert cell["kind"] == "audio"
    # T0/C7: empty
    assert grid["cells"][0][7]["exists"] is False
    assert grid["cells"][0][7]["beats"] == 0


def test_track1_three_clips():
    """T1 has clips in cols 0, 1, 2 with beats 8, 2, 4."""
    src = json.load(open(os.path.join(FIXTURE_DIR, "set26-source.abl")))
    grid = parse_clip_grid(src)
    row = grid["cells"][1]
    assert row[0]["exists"] is True
    assert row[0]["beats"] == 8.0
    assert row[0]["kind"] == "audio"
    assert row[1]["exists"] is True
    assert row[1]["beats"] == 2.0
    assert row[2]["exists"] is True
    assert row[2]["beats"] == 4.0
    # Remaining cols empty
    for c in range(3, 8):
        assert row[c]["exists"] is False
        assert row[c]["beats"] == 0


def test_midi_tracks_all_empty():
    """T2 and T3 in Set 26 are MIDI with no clips; all 8 cells exists=False, kind='midi'."""
    src = json.load(open(os.path.join(FIXTURE_DIR, "set26-source.abl")))
    grid = parse_clip_grid(src)
    for t in (2, 3):
        for c in range(8):
            cell = grid["cells"][t][c]
            assert cell["exists"] is False, f"T{t}/C{c} should be empty"
            assert cell["beats"] == 0
            assert cell["color"] is None
            assert cell["kind"] == "midi"


def test_loop_disabled_uses_region_length():
    """loop.isEnabled=False → parser uses region.end - region.start."""
    song = {
        "tempo": 120.0,
        "timeSignature": {"upper": 4, "lower": 4},
        "tracks": [
            {
                "kind": "audio",
                "clipSlots": [
                    {
                        "clip": {
                            "color": 5,
                            "region": {
                                "start": 0.0,
                                "end": 12.0,
                                "loop": {"isEnabled": False, "start": 0.0, "end": 4.0},
                            },
                        }
                    }
                ],
            }
        ],
    }
    grid = parse_clip_grid(song)
    cell = grid["cells"][0][0]
    assert cell["exists"] is True
    assert cell["beats"] == 12.0
    assert cell["loop_enabled"] is False


def test_clip_without_loop_key():
    """No 'loop' subkey under region → parser uses region.end - region.start."""
    song = {
        "tempo": 100.0,
        "tracks": [
            {
                "kind": "audio",
                "clipSlots": [
                    {
                        "clip": {
                            "color": 3,
                            "region": {"start": 1.0, "end": 7.0},
                        }
                    }
                ],
            }
        ],
    }
    grid = parse_clip_grid(song)
    cell = grid["cells"][0][0]
    assert cell["exists"] is True
    assert cell["beats"] == 6.0
    assert cell["loop_enabled"] is False


def test_track_count_clamped_to_four():
    """6 tracks in source → num_tracks=4; only first 4 appear in cells."""
    song = {
        "tempo": 90.0,
        "tracks": [
            {"kind": "audio", "clipSlots": []},
            {"kind": "audio", "clipSlots": []},
            {"kind": "midi", "clipSlots": []},
            {"kind": "midi", "clipSlots": []},
            {
                "kind": "audio",
                "clipSlots": [
                    {"clip": {"color": 9, "region": {"start": 0, "end": 4}}}
                ],
            },
            {"kind": "midi", "clipSlots": []},
        ],
    }
    grid = parse_clip_grid(song)
    assert grid["num_tracks"] == 4
    assert len(grid["cells"]) == 4
    # T4 clip must not leak into row 0..3
    for t in range(4):
        for c in range(8):
            assert grid["cells"][t][c]["exists"] is False


def test_clip_slots_clamped_to_eight():
    """10 clipSlots in source → parser only reads first 8."""
    slots = [
        {"clip": {"color": c, "region": {"start": 0, "end": c + 1}}}
        for c in range(10)
    ]
    song = {
        "tempo": 100.0,
        "tracks": [{"kind": "audio", "clipSlots": slots}],
    }
    grid = parse_clip_grid(song)
    row = grid["cells"][0]
    assert len(row) == 8
    # First 8 are populated
    for c in range(8):
        assert row[c]["exists"] is True
        assert row[c]["beats"] == float(c + 1)
