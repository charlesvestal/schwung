#!/usr/bin/env bash
# Every WAV the preroll trim has to read back must be opened readable.
#
# test_sampler_wav_trim.c pins the trim itself, and would stay green through
# this bug: the defect was never in the arithmetic, it was that both callers
# handed it a stream opened "wb". fread on a write-only stream returns 0, the
# copy loop breaks on the first pass, and what lands on the card is the
# preroll at exactly the right duration. Only the CALL SITE can show that.
#
# The two files are the sampler's master take and each of its five stems.

set -uo pipefail
cd "$(dirname "$0")/../.."

SRC=src/host/shadow_sampler.c
fails=0

fail() { echo "FAIL: $1"; fails=$((fails + 1)); }

# The master take.
if ! grep -q 'sampler_wav_file = fopen(sampler_current_recording, "w+b")' "$SRC"; then
    fail "master take is not opened \"w+b\" — the preroll trim cannot read it back"
fi

# The stems, opened in sampler_worker_open_stems.
if ! grep -q 'st->file = fopen(st->path, "w+b")' "$SRC"; then
    fail "stem files are not opened \"w+b\" — their preroll trim cannot read them back"
fi

# And nothing may quietly reintroduce a write-only open for either of them.
if grep -n 'fopen(sampler_current_recording, "wb")\|fopen(st->path, "wb")' "$SRC"; then
    fail "a sampler WAV is opened write-only above"
fi

if [ "$fails" -eq 0 ]; then
    echo "test_sampler_wav_open_mode: all checks passed"
    exit 0
fi
exit 1
