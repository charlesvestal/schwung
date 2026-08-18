"""Unit tests for SchwungBus.wait_for_overtake_dsp().

These tests don't need a running daemon — SchwungBus connects lazily,
so state() and get_param() are stubbed directly. Run with
`pytest tools/pytest-schwung/tests` on the dev machine.

The behaviour under test exists because the overtake DSP load moved off
the SPI thread: overtake_mode flips when the load is *requested*, and
the instance appears up to ~200ms later. Gating on either signal alone
is wrong in a different way, so the ordering is the thing worth pinning.
"""

from __future__ import annotations

import pytest

from schwung_bus import SchwungBus, SchwungBusError, BusState


def make_bus(modes, readys):
    """A bus whose state()/get_param() replay the given sequences.

    The last element of each sequence repeats once exhausted, so a test
    only has to spell out the transitions it cares about.
    """
    bus = SchwungBus(host="127.0.0.1", port=1)  # never connected
    calls = {"state": 0, "ready": 0, "order": []}

    def fake_state():
        i = min(calls["state"], len(modes) - 1)
        calls["state"] += 1
        calls["order"].append("state")
        mode = modes[i]
        if isinstance(mode, Exception):
            raise mode
        return BusState(
            move_ui_mode=0, overtake_mode=mode, shift_held=0,
            selected_slot=0, ui_slot=0, shim_counter=0,
            transport_playing=0, speaker_active=0,
            line_in_connected=0, display_mode=0,
        )

    def fake_get_param(key, overtake=True):
        assert key == "__ready", f"unexpected param GET: {key!r}"
        assert overtake is True, "must route through the overtake_dsp: prefix"
        i = min(calls["ready"], len(readys) - 1)
        calls["ready"] += 1
        calls["order"].append("ready")
        val = readys[i]
        if isinstance(val, Exception):
            raise val
        return val

    bus.state = fake_state           # type: ignore[method-assign]
    bus.get_param = fake_get_param   # type: ignore[method-assign]
    bus._calls = calls               # type: ignore[attr-defined]
    return bus


def test_returns_once_mode_flips_and_dsp_reports_ready():
    bus = make_bus(modes=[0, 0, 2], readys=["0", "0", "1"])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)
    assert bus._calls["state"] == 3
    assert bus._calls["ready"] == 3


def test_stale_ready_before_mode_flip_does_not_return_early():
    """The regression this helper exists for.

    __ready answers "1" whenever *nothing* is loading, which includes the
    window before the load starts. A bare __ready poll would pass instantly
    against the previous module's state. Gating on the mode first must stop
    that: no param GET may happen until overtake_mode == 2.
    """
    bus = make_bus(modes=[0, 0, 0, 2], readys=["1"])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)

    order = bus._calls["order"]
    first_ready = order.index("ready")
    # Every state() poll that returned a non-module mode must precede the
    # first param GET — i.e. we never consulted __ready while mode was 0.
    assert order[:first_ready] == ["state"] * 4, order
    assert bus._calls["ready"] == 1


def test_module_without_dsp_returns_as_soon_as_mode_flips():
    """No dsp.so => no load requested => __ready reads "1" throughout."""
    bus = make_bus(modes=[2], readys=["1"])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)
    assert bus._calls["ready"] == 1


def test_host_without_ready_param_is_treated_as_ready():
    """Older firmware errors on the unknown key; mirror shadow_ui's catch."""
    bus = make_bus(modes=[2], readys=[SchwungBusError("param GET error 14")])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)


def test_transient_state_errors_are_tolerated():
    """shadow_ui can be briefly unresponsive mid-load."""
    bus = make_bus(
        modes=[SchwungBusError("param SHM busy"), 2],
        readys=["1"],
    )
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)


def test_raises_when_mode_never_reaches_module():
    bus = make_bus(modes=[0], readys=["1"])
    with pytest.raises(SchwungBusError, match="overtake_mode never reached"):
        bus.wait_for_overtake_dsp(timeout=0.2, poll_interval=0.05)
    # Gate 1 never passed, so __ready must never have been consulted.
    assert bus._calls["ready"] == 0


def test_raises_when_dsp_never_finishes_loading():
    bus = make_bus(modes=[2], readys=["0"])
    with pytest.raises(SchwungBusError, match="DSP still loading"):
        bus.wait_for_overtake_dsp(timeout=0.2, poll_interval=0.05)


def test_busstate_module_constant_is_two():
    """The wire value the shim reports for module mode."""
    assert BusState.OVERTAKE_MODULE == 2
