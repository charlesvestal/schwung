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

import time

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
    # Constructed as a COLD start (mode observed at 0 first). Written as
    # modes=[2] it would instead be indistinguishable from a reload — see
    # test_reload_falls_through_when_no_load_ever_starts.
    bus = make_bus(modes=[0, 2], readys=["1"])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)
    assert bus._calls["ready"] == 1


def test_host_without_ready_param_is_treated_as_ready():
    """Older firmware errors on the unknown key; mirror shadow_ui's catch.

    The message must be the daemon's REAL one. On a host predating the
    param the shim answers error 14 (no handler claimed the key) and
    commands.c replies "param GET error from peer" — which is
    deliberately not one of _PARAM_TRANSIENT_ERRORS, so it propagates
    without retrying. An invented string here would pass against a
    helper that catches SchwungBusError wholesale, which is precisely
    the bug the discrimination fixes.
    """
    bus = make_bus(
        modes=[0, 2],
        readys=[SchwungBusError("param GET error from peer")],
    )
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)


def test_transient_state_errors_are_tolerated():
    """shadow_ui can be briefly unresponsive mid-load."""
    bus = make_bus(
        modes=[SchwungBusError("param SHM busy"), 2],
        readys=["1"],
    )
    # reload_settle=0 keeps this test about gate 1 only. A state() error is
    # deliberately NOT counted as evidence of a transition, so without this
    # the helper would also take the gate-1b settle path.
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0, reload_settle=0.0)


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


# --- the three review findings ------------------------------------------
#
# Each of these passed vacuously before the fix, in the worst way available:
# the helper RETURNED, so a test built on it went on to press a pad into a
# shim that drops it. A silent pass, not a failure.


def test_contention_on_ready_is_not_mistaken_for_an_old_host():
    """Finding 1. A contention error must not end the wait.

    _param_request_with_retry retries the transient errors three times and
    then re-raises SchwungBusError — the same type an unknown key raises.
    The DSP load window is exactly when /schwung-param is most contended
    (shim worker in dlopen() while shadow_ui ticks), so catching the base
    class returned "ready" at the one moment it is least true.
    """
    bus = make_bus(
        modes=[0, 2],
        readys=[SchwungBusError("param GET timeout"), "0", "1"],
    )
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0)
    # It kept polling through the error rather than returning on it.
    assert bus._calls["ready"] == 3


def test_gate2_gets_its_own_budget():
    """Finding 2. A slow mode flip must not consume gate 2's deadline.

    With one shared deadline, a state() round-trip that ate the budget left
    gate 2's `while` false on entry: no __ready GET was ever issued, and the
    helper still raised "DSP still loading ... may have failed to load",
    blaming the module for pure budget exhaustion.
    """
    bus = make_bus(modes=[2], readys=["1"])
    inner = bus.state

    def slow_state():
        # One round-trip that alone outlasts gate 1's whole budget. Stated
        # as an inequality (0.3 > 0.2) rather than a race between two polls
        # and a deadline, so scheduler overshoot can only strengthen it.
        time.sleep(0.3)           # shadow_ui mid-load
        return inner()

    bus.state = slow_state
    bus.wait_for_overtake_dsp(timeout=0.2, poll_interval=0.0,
                              ready_timeout=1.0, reload_settle=0.0)
    # The point of the fix: gate 2 actually ran. Sharing gate 1's deadline
    # leaves this at 0 and raises instead.
    assert bus._calls["ready"] >= 1


def test_reload_does_not_pass_on_the_outgoing_modules_state():
    """Finding 3. overtake_mode is already 2 across a module -> module swap.

    unloadModuleUi() does not reset the overtake mode, and the
    open_tool_cmd handler's unload+load pair runs in ONE synchronous
    shadow_ui tick — so there is no observable 2 -> x -> 2 transition and
    gate 1 passes on the stale mode. __ready then still reads "1" (the new
    load has not been requested yet), so the helper returned before
    anything had happened.

    Reachable in practice because the `fresh_move` fixture is documented as
    skippable, so a file that opens tool A then tool B without it hits this.
    """
    bus = make_bus(modes=[2], readys=["1", "0", "0", "1"])
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.0,
                              reload_settle=1.0)
    # It must have waited out the "0"s rather than returning on the leading
    # stale "1". Returning early would leave this at 1.
    assert bus._calls["ready"] == 4


def test_reload_falls_through_when_no_load_ever_starts():
    """Finding 3, the case that is NOT covered, pinned so it stays known.

    A module with no dsp.so never requests a load, so it never drives
    __ready to "0" — indistinguishable from a stale mode. The helper waits
    the bounded settle and then proceeds, degrading to the pre-fix
    behaviour rather than hanging. Telling the two apart needs the
    open_tool_cmd edge, which the daemon does not expose.
    """
    bus = make_bus(modes=[2], readys=["1"])
    started = time.monotonic()
    bus.wait_for_overtake_dsp(timeout=5.0, poll_interval=0.01,
                              reload_settle=0.15)
    elapsed = time.monotonic() - started
    assert elapsed >= 0.15, "should have waited the settle window"
    assert elapsed < 2.0, "settle must be bounded, not the full timeout"


def test_no_successful_ready_read_is_reported_as_such():
    """A channel that never answers is not a module that failed to load.

    Distinct from the "DSP still loading" message, which now means a real
    observed "0" rather than a read that never happened.
    """
    bus = make_bus(
        modes=[0, 2],
        readys=[SchwungBusError("param SHM busy")],
    )
    with pytest.raises(SchwungBusError, match="no successful"):
        bus.wait_for_overtake_dsp(timeout=0.2, poll_interval=0.02,
                                  ready_timeout=0.1, reload_settle=0.0)
