"""Offline tests for the client's pad/step note range validators.

These exercise the public API surface (``bus.press_pad`` /
``bus.press_step``) raising ``ValueError`` at the boundary, rather
than importing private ``_check_*`` helpers. Tests in this file run
without a daemon — no network, no hardware — because the validators
fire before any inject_midi call hits the socket.

We mock the bus by instantiating ``SchwungBus`` directly and
overriding ``inject_midi`` to a no-op. The validator runs in the
public API method before inject_midi is called, so a no-op override
is enough to keep the call from raising ``SchwungBusError`` on the
unbound socket.
"""

from __future__ import annotations

import pytest

from schwung_bus.client import SchwungBus


@pytest.fixture
def offline_bus():
    """SchwungBus instance with inject_midi stubbed to a no-op.

    Validators raise BEFORE inject_midi runs, so the public API can
    be exercised at the boundaries without a daemon connection.
    """
    b = SchwungBus()
    b.inject_midi = lambda packet: None  # type: ignore[method-assign]
    return b


@pytest.mark.parametrize("note,ok", [
    (67, False),
    (68, True),    # lowest valid pad
    (99, True),    # highest valid pad
    (100, False),
    (0, False),
    (127, False),
])
def test_press_pad_note_range(offline_bus, note, ok):
    """``bus.press_pad`` accepts notes 68..99 inclusive and raises
    ValueError on anything outside. Off by one in either direction
    would silently let tests inject the wrong kind of MIDI.
    """
    if ok:
        offline_bus.press_pad(note)
    else:
        with pytest.raises(ValueError):
            offline_bus.press_pad(note)


@pytest.mark.parametrize("note,ok", [
    (15, False),
    (16, True),    # lowest valid step
    (31, True),    # highest valid step
    (32, False),
    (0, False),
    (60, False),
])
def test_press_step_note_range(offline_bus, note, ok):
    """``bus.press_step`` accepts notes 16..31 inclusive."""
    if ok:
        offline_bus.press_step(note)
    else:
        with pytest.raises(ValueError):
            offline_bus.press_step(note)


def test_press_pad_rejects_velocity_outside_1_127(offline_bus):
    """Velocity 0 is a logical note-off; press_pad must reject it
    (use release_pad for that) so tests that mean "press" can't
    accidentally send a release."""
    with pytest.raises(ValueError):
        offline_bus.press_pad(84, velocity=0)
    with pytest.raises(ValueError):
        offline_bus.press_pad(84, velocity=128)


def test_release_pad_accepts_velocity_zero(offline_bus):
    """Release velocity 0 is normal (most controllers send 0 on
    note-off). Must not raise."""
    offline_bus.release_pad(84, velocity=0)
    offline_bus.release_pad(84, velocity=64)
    offline_bus.release_pad(84, velocity=127)


def test_pad_index_round_trips(offline_bus):
    """note → index → note formula must be self-consistent at the
    boundaries. Catches a regression where pad_index uses a
    different offset than press_pad expects.
    """
    for note in (68, 84, 99):
        idx = offline_bus.pad_index(note)
        assert 0 <= idx <= 31
        assert idx == note - 68


def test_step_index_round_trips(offline_bus):
    for note in (16, 23, 31):
        idx = offline_bus.step_index(note)
        assert 0 <= idx <= 15
        assert idx == note - 16
