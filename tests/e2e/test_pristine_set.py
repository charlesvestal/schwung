"""On-device test for the portable ``pristine_set`` fixture.

Exercises the full lifecycle on real hardware: find-or-create the
dedicated test set (by display name), swap the
``user.song-index`` xattr so Move loads it, cp the canonical empty
Song.abl, restart Move, and — on teardown — restore the device's
pre-session xattr state.

Skips cleanly when the daemon or SSH to the device is unavailable, so
it is safe to run without a Move attached.
"""

from __future__ import annotations


def test_pristine_set_resets_and_recovers(bus, pristine_set):
    # Reaching the body means the fixture found-or-created the template
    # set, performed the xattr swap, cp'd the empty Song.abl, restarted
    # Move, and the shim came back. Assert the device is live and
    # stepping after the restart-move cycle.
    st = bus.state()
    assert st.shim_counter >= 0

    a = bus.wait_frame(2).counter
    b = bus.wait_frame(2).counter
    assert (b - a) & 0xFFFFFFFF >= 2, "shim not stepping after restart-move"

    assert len(bus.snapshot_pad_leds()) == 32
