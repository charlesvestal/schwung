import numpy as np
from .wrap import wrap_mix


def test_wrap_mix_zero_tail():
    """Empty tail → output equals stem."""
    stem = np.array([100, 200, 300], dtype=np.int16)
    tail = np.array([], dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=3)
    assert np.array_equal(out, stem)


def test_wrap_mix_decaying_tail_lands_on_head():
    """Tail chunks at offsets 0 and L should both accumulate into stem[0..L)."""
    L = 100
    stem = np.zeros(L, dtype=np.int16)
    tail = np.zeros(2 * L, dtype=np.int16)
    tail[0:10] = 1000      # Tail chunk 0, samples 0..9
    tail[L:L+10] = 500     # Tail chunk 1, samples 0..9 of next iteration
    out = wrap_mix(stem, tail, loop_length=L)
    assert out[0] == 1500
    assert out[9] == 1500
    assert out[10] == 0


def test_wrap_mix_returns_int16():
    """Output must be int16 (the WAV writer expects it)."""
    stem = np.array([0, 0], dtype=np.int16)
    tail = np.array([1, 2], dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=2)
    assert out.dtype == np.int16


def test_wrap_mix_saturating_clip_positive():
    """Sum exceeding 32767 must clip, not wrap around."""
    stem = np.array([30000], dtype=np.int16)
    tail = np.array([20000], dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=1)
    assert out[0] == 32767


def test_wrap_mix_saturating_clip_negative():
    """Sum below -32768 must clip."""
    stem = np.array([-30000], dtype=np.int16)
    tail = np.array([-20000], dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=1)
    assert out[0] == -32768


def test_wrap_mix_partial_tail_ignored():
    """A trailing partial chunk (len < L) is not mixed."""
    L = 4
    stem = np.zeros(L, dtype=np.int16)
    tail = np.array([10, 20, 30, 40, 50, 60], dtype=np.int16)  # one full chunk + 2 leftover
    out = wrap_mix(stem, tail, loop_length=L)
    # Only first chunk [10,20,30,40] mixes in; [50,60] partial is ignored
    assert list(out) == [10, 20, 30, 40]


def test_wrap_mix_three_chunks():
    """All three full chunks accumulate into the head."""
    L = 2
    stem = np.zeros(L, dtype=np.int16)
    tail = np.array([1, 2, 3, 4, 5, 6], dtype=np.int16)
    out = wrap_mix(stem, tail, loop_length=L)
    # Chunks [1,2] + [3,4] + [5,6] → [9, 12]
    assert out[0] == 9
    assert out[1] == 12


def test_wrap_mix_stereo_treated_as_raw_samples():
    """Stereo data is just longer flat arrays; wrap_mix doesn't care about channels.
    Verify L=2 (1 stereo frame) behaves the same as L=2 mono.
    """
    stem = np.array([100, 200], dtype=np.int16)
    tail = np.array([50, 50, 25, 25], dtype=np.int16)  # 2 stereo frames
    out = wrap_mix(stem, tail, loop_length=2)
    assert list(out) == [175, 275]
