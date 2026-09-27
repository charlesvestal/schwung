#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE RESAMPLE BRIDGE IS THE LAST WRITER OF AUDIO_IN IN A FRAME.
#
# native_resample_bridge_apply() overwrites the shadow mailbox's AUDIO_IN with
# Schwung's total mix. A Line In chain slot reads that same region during the
# slot render. With the bridge BEFORE the render, Line In read Schwung's own
# mix -- its own output included -- and fed back: a closed digital loop that
# rang with Resample on Mix and a Line In slot loaded.
#
# Source-order, because shim_post_transfer does not build on the host: inside
# that function the bridge call must come AFTER the render call, and exactly
# once. `schwung_spi_lib.c` runs post_fn inside the hooked ioctl, so the end of
# the function is still before Move reads the mailbox -- also asserted.

fn=$(awk '/^static void shim_post_transfer\(/{f=1} f{print NR": "$0} f&&/^}/{exit}' src/schwung_shim.c)
bridge=$(printf '%s\n' "$fn" | grep -E '^[0-9]+: +native_resample_bridge_apply\(\);' | cut -d: -f1)
render=$(printf '%s\n' "$fn" | grep -E '^[0-9]+: +shadow_inprocess_render_to_buffer\(\);' | cut -d: -f1)

n=$(printf '%s\n' "$bridge" | grep -c . || true)
[ "$n" = "1" ] || { echo "FAIL: expected exactly one bridge call in shim_post_transfer, found $n"; exit 1; }
[ -n "$render" ] || { echo "FAIL: the slot render call was not found in shim_post_transfer"; exit 1; }
[ "$bridge" -gt "$render" ] || {
  echo "FAIL: the resample bridge (line $bridge) runs before the slot render (line $render) --"
  echo "      a Line In slot reads Schwung's own mix and feeds back"; exit 1; }

# post_fn must run before the hooked ioctl returns, or "after the render" is
# after Move has already read the mailbox and Resample gets the jack.
lib=src/lib/schwung_spi_lib.c
post=$(grep -n 'g_spi.post_fn(g_spi.ctx' "$lib" | head -1 | cut -d: -f1)
ret=$(awk -v p="$post" 'NR>p && /return ret;/ {print NR; exit}' "$lib")
[ -n "$post" ] && [ -n "$ret" ] || { echo "FAIL: could not find post_fn / return in $lib"; exit 1; }

echo "PASS: the resample bridge writes AUDIO_IN after the slot render (line $bridge > $render), inside the ioctl"
