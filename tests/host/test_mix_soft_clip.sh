#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

bin="build/tests/test_mix_soft_clip"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
  -Isrc/host \
  tests/host/test_mix_soft_clip.c \
  -o "$bin" -lm

"$bin"

# The rebuild path must convert through the curve, never clamp into the
# mailbox add by add -- that per-add clamp is the defect this replaced.
shim=src/schwung_shim.c
if ! grep -q 'mix_soft_clip_block(la_acc, mailbox_audio' "$shim"; then
  echo "FAIL: rebuild mix no longer converts la_acc through mix_soft_clip_block"
  exit 1
fi
# The three adds the rebuild used to clamp into the mailbox one at a time.
for pat in \
  'mailbox_audio\[i\] \+ \(int32_t\)lroundf\(\(float\)fx_buf' \
  'mailbox_audio\[i\] \+ \(int32_t\)shadow_deferred_dsp_buffer' \
  'int32_t mixed = \(int32_t\)mailbox_audio\[i\] \+$'; do
  if grep -nE "$pat" "$shim"; then
    echo "FAIL: a per-add clamp into mailbox_audio is back in the rebuild (above)"
    exit 1
  fi
done
echo "test_mix_soft_clip.sh: rebuild call-site PASS"
