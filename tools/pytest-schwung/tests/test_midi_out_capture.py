"""Unit tests for MidiOutEvent / MidiOutCapture parsing & filtering.

These tests don't need a running daemon — they exercise the data
classes directly. Run with `pytest tools/pytest-schwung/tests` on the
dev machine.
"""

from __future__ import annotations

import pytest

from schwung_bus import MidiOutEvent, MidiOutCapture


def make_note_on(note: int = 60, velocity: int = 100, cable: int = 0, channel: int = 0) -> MidiOutEvent:
    return MidiOutEvent(
        frame=1, cable=cable, cin=0x9,
        status=0x90 | channel, data1=note, data2=velocity,
    )


def make_note_off(note: int = 60, velocity: int = 0x40, cable: int = 0, channel: int = 0) -> MidiOutEvent:
    return MidiOutEvent(
        frame=2, cable=cable, cin=0x8,
        status=0x80 | channel, data1=note, data2=velocity,
    )


def make_cc(cc: int = 7, value: int = 100, cable: int = 0, channel: int = 0) -> MidiOutEvent:
    return MidiOutEvent(
        frame=3, cable=cable, cin=0xB,
        status=0xB0 | channel, data1=cc, data2=value,
    )


def test_event_kind_mapping():
    assert make_note_on().kind == "note_on"
    assert make_note_off().kind == "note_off"
    assert make_cc().kind == "cc"
    pb = MidiOutEvent(frame=0, cable=0, cin=0xE, status=0xE0, data1=0, data2=64)
    assert pb.kind == "pitch_bend"


def test_event_channel_extraction():
    assert make_note_on(channel=0).channel == 0
    assert make_note_on(channel=15).channel == 15


def test_filter_by_kind():
    cap = MidiOutCapture(events=[make_note_on(60), make_note_off(60), make_cc(7)])
    assert len(cap.filter(kind="note_on")) == 1
    assert len(cap.filter(kind="note_off")) == 1
    assert len(cap.filter(kind="cc")) == 1
    assert len(cap.filter(kind="aftertouch")) == 0


def test_filter_by_note():
    cap = MidiOutCapture(events=[
        make_note_on(60), make_note_on(62),
        make_note_off(60), make_note_off(62),
    ])
    assert len(cap.filter(note=60)) == 2
    assert len(cap.filter(note=60, kind="note_off")) == 1


def test_filter_by_cable_and_channel():
    cap = MidiOutCapture(events=[
        make_note_on(60, cable=0, channel=0),
        make_note_on(60, cable=2, channel=0),
        make_note_on(60, cable=0, channel=5),
    ])
    assert len(cap.filter(cable=0)) == 2
    assert len(cap.filter(cable=2)) == 1
    assert len(cap.filter(channel=5)) == 1
    assert len(cap.filter(cable=0, channel=0)) == 1


def test_filter_by_cc():
    cap = MidiOutCapture(events=[make_cc(7), make_cc(11), make_note_on(60)])
    assert len(cap.filter(cc=7)) == 1
    assert len(cap.filter(cc=11)) == 1
    assert len(cap.filter(cc=99)) == 0


def test_filter_returns_independent_capture():
    """Filter must not mutate the original or share its list."""
    cap = MidiOutCapture(events=[make_note_on(60), make_note_off(60)])
    filtered = cap.filter(kind="note_on")
    filtered.events.clear()
    assert len(cap) == 2, "filter() must return an independent capture"


def test_stuck_note_assertion_pattern():
    """The shape of the regression assertion that motivated this whole infra."""
    healthy = MidiOutCapture(events=[make_note_on(60, 100), make_note_off(60)])
    assert len(healthy.filter(kind="note_off", note=60)) >= len(healthy.filter(kind="note_on", note=60))

    stuck = MidiOutCapture(events=[
        make_note_on(60, 100), make_note_on(60, 100), make_note_off(60),
    ])
    # In a real test, this would FAIL — exactly the desired behavior.
    assert len(stuck.filter(kind="note_off", note=60)) < len(stuck.filter(kind="note_on", note=60))
