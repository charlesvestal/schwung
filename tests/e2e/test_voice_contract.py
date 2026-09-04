"""The pad_layout / voices contract, on real hardware.

The unit suites (tests/host/test_voices.sh, test_voice_follow.sh,
test_chain_last_note.sh) construct their subject by hand. This one drives the
shipped `voice-poc` module through the real pipeline and asserts what the
DEVICE answers, because the deliverable is a contract other people implement
and a test that builds its own subject cannot see the integration.

It has already earned its keep twice. The POC could not be loaded at all in an
earlier form (a chain slot dlopens <synth>/dsp.so, and it had none), and a
sound generator's module.json hierarchy turns out never to be read
(parse_ui_hierarchy_cache runs for FX only) -- neither was visible from the
host, and both would have shipped a contract nobody could implement.

STIMULUS: cable 2, channel 1. A cable-0 pad injection reaches Move but is
broadcast to slots as MOVE_MIDI_SOURCE_FX_BROADCAST, which by design forwards
to AUDIO FX ONLY and never to the synth (schwung_shim.c), so it can never move
last_note. An earlier version of this test used it and was measuring the
harness, not the product. shadow_dispatch_cable2_channeled_slots delivers
channel-matched external MIDI straight to the slot, which is what a synth
actually hears.

Requires `voice-poc` installed on the device and slot 0 on receive channel 1.
"""

from __future__ import annotations

import pytest

SNARE, HAT, TOM_LO = 38, 42, 60


def _read(bus, key, tries=8):
    """A param read has THREE answers.

    An error from the peer is a read that did not complete -- no information --
    and treating it as "the module said nothing" is how a timeout becomes a
    fabricated defect. Retry, then report None.
    """
    for _ in range(tries):
        try:
            return bus.get_param(key, overtake=False).strip()
        except Exception:
            bus.wait_frame(4)
    return None


def _note_on(bus, note, vel=100):
    bus.inject_midi(bytes([0x29, 0x90, note, vel]))   # cable 2, note-on, ch 1
    bus.wait_frame(25)


def _note_off(bus, note):
    bus.inject_midi(bytes([0x28, 0x80, note, 0]))
    bus.wait_frame(15)


@pytest.fixture
def voice_poc(bus):
    """Load voice-poc into slot 0 and wait for it to serve its contract."""
    bus.set_param("synth:module", "voice-poc", overtake=False)
    for _ in range(40):
        bus.wait_frame(5)
        hier = _read(bus, "synth:ui_hierarchy", tries=1)
        if hier and "pad_layout" in hier:
            return hier
    pytest.skip("voice-poc is not installed on this device")


def test_device_serves_the_declaration(voice_poc):
    flat = voice_poc.replace(" ", "")
    assert '"pad_layout":"drums"' in flat
    assert '"focus_param":"cur_voice"' in flat
    assert "Tom Lo" in voice_poc, "declared child_names not served"
    # A page that declares no note is a page, not a voice.
    assert '"reverb":{"name":"Reverb"' in flat


def test_last_note_starts_unset(bus, voice_poc):
    assert _read(bus, "synth:last_note") == "-1"


def test_last_note_follows_a_played_note(bus, voice_poc):
    _note_on(bus, SNARE)
    assert _read(bus, "synth:last_note") == "38"


def test_note_off_does_not_clear_last_note(bus, voice_poc):
    """A released pad is still the pad you are editing."""
    _note_on(bus, SNARE)
    _note_off(bus, SNARE)
    assert _read(bus, "synth:last_note") == "38"


def test_last_note_follows_a_template_rack_voice(bus, voice_poc):
    _note_on(bus, TOM_LO)
    assert _read(bus, "synth:last_note") == "60"


def test_module_owns_its_focus(bus, voice_poc):
    _note_on(bus, SNARE)
    assert _read(bus, "synth:cur_voice") == "snare"
    _note_on(bus, HAT)
    assert _read(bus, "synth:cur_voice") == "hat"


def test_writing_the_focus_param_moves_it(bus, voice_poc):
    """Both directions: a pick writes the same param the module writes."""
    bus.set_param("synth:cur_voice", "kick", overtake=False)
    bus.wait_frame(15)
    assert _read(bus, "synth:cur_voice") == "kick"


def test_following_a_voice_changes_no_pad_led(bus, voice_poc):
    """Move owns the pads. Measured over every pad, not eyeballed."""
    before = bus.snapshot_pad_leds()
    for note in (SNARE, HAT, TOM_LO):
        _note_on(bus, note)
        _note_off(bus, note)
    bus.set_param("synth:cur_voice", "kick", overtake=False)
    bus.wait_frame(15)
    after = bus.snapshot_pad_leds()
    diff = sum(1 for a, b in zip(before, after) if a != b)
    assert before == after, f"{diff} of {len(before)} pad bytes changed"
