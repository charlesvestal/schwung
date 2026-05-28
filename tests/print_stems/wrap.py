"""Perfect-loop bounce: wrap a recorded tail back into the stem head."""

import numpy as np


def wrap_mix(stem: np.ndarray, tail: np.ndarray, loop_length: int) -> np.ndarray:
    """Mix tail chunks back into stem head for a gapless loop.

    Args:
        stem: int16 1-D array of length L (interleaved stereo if you pre-interleaved
            it; just treat L as raw sample count). Caller has already aligned the
            tail's frame-0 to immediately after the stem's last sample.
        tail: int16 1-D array of length >= 0. Will be partitioned into chunks of
            length ``loop_length``; any partial chunk at the end is ignored.
        loop_length: int. Number of samples per loop iteration.

    Returns: int16 1-D array of length ``loop_length``, with each tail chunk
        accumulated into the head and clipped to [-32768, 32767].
    """
    L = loop_length
    acc = stem.astype(np.int32).copy()
    n_chunks = len(tail) // L
    for i in range(n_chunks):
        acc += tail[i * L:(i + 1) * L].astype(np.int32)
    return np.clip(acc, -32768, 32767).astype(np.int16)
