#!/usr/bin/env bash
# The /mirror page decodes surface_live_shm_t BY BYTE OFFSET (the struct is
# shipped to the browser base64). Two consumers of one layout, written in two
# languages: this asks the compiler for every offset and fails on any the
# page's OFF table disagrees with, and on a version mismatch.
set -euo pipefail
cd "$(dirname "$0")/../.."
T=$(mktemp -d "${TMPDIR:-/tmp}/mirror_layout.XXXXXX"); trap 'rm -rf "$T"' EXIT
cat > "$T/o.c" <<'C'
#include <stdio.h>
#include "surface_live_shm.h"
#define O(js, f) printf("%s %zu\n", js, offsetof(surface_live_shm_t, f))
int main(void) {
    O("frame", frame); O("evCount", event_count); O("noteLed", note_led); O("noteAnim", note_led_anim);
    O("ccLed", cc_led); O("ccAnim", cc_led_anim); O("rgb", rgb); O("noteDown", note_down);
    O("press", note_pressure); O("ccVal", cc_value); O("enc", enc_pos); O("events", events);
    printf("size %zu\nversion %d\n", sizeof(surface_live_shm_t), SURFACE_LIVE_VERSION);
    return 0;
}
C
${CC:-cc} -Isrc/host -o "$T/o" "$T/o.c"
"$T/o" > "$T/want"
PAGE=schwung-manager/static/mirror.html
fails=0
while read -r key val; do
  if [ "$key" = version ]; then
    got=$(sed -n 's/^const SURFACE_VERSION = \([0-9]*\);.*/\1/p' "$PAGE")
  else
    got=$(grep -o "[{ ,]$key: [0-9]*" "$PAGE" | head -1 | sed 's/.*: //')
  fi
  if [ "$got" = "$val" ]; then echo "ok   $key = $val"; else echo "FAIL $key: header $val, page ${got:-missing}"; fails=$((fails+1)); fi
done < "$T/want"
# /mirror is served over plain http (move.local, 172.16.254.1), which is NOT a
# secure context, so any API gated on one is simply absent there. The first
# sound build used an AudioWorklet: it passed a localhost test and on the
# device Unmute did nothing and recordings silently lost their audio track.
for api in audioWorklet AudioWorkletNode navigator.clipboard serviceWorker crypto.subtle; do
  if grep -q "$api" "$PAGE"; then echo "FAIL $PAGE uses $api, which plain http does not have"; fails=$((fails+1));
  else echo "ok   no $api"; fi
done
[ "$fails" -eq 0 ]
