#!/usr/bin/env bash
# A SHADOW_PARAM_VALUE_LEN buffer must never be a local, and neither must a
# chain_param_info_t array.
#
# chain_mod_refresh_target_param_cache declared both: 128KB plus ~1.05MB
# (chain_param_info_t is ~4.3KB, of which options[128][32] is 4KB, times
# MAX_CHAIN_PARAMS). That is a ~1.2MB stack frame on a function reachable from
# a plugin entry point -- which IS the SPI callback -- every 250ms. It survived
# because it never crashed on a default 8MB pthread stack; it would not survive
# a smaller one, and the constant it is sized from grows.
#
# Both buffers are `static` now: single callback thread, no recursion, and
# malloc on that thread is a realtime violation.
set -euo pipefail
cd "$(dirname "$0")/../.."

fail=0

# A declaration of this buffer that is not static and not a parameter.
while IFS= read -r hit; do
    case "$hit" in
        *static*|*"const char"*|*"char *"*) continue ;;
        *shadow_constants.h*) continue ;;   # shadow_param_t's own member
    esac
    echo "FAIL: SHADOW_PARAM_VALUE_LEN buffer on the stack: $hit"
    fail=1
done < <(grep -rn "^[[:space:]]*[a-z_]* *char [a-z_]*\[SHADOW_PARAM_VALUE_LEN\]" src/ || true)

while IFS= read -r hit; do
    case "$hit" in
        *static*) continue ;;
        *chain_internal.h*) continue ;;   # the per-instance members, by design
    esac
    echo "FAIL: chain_param_info_t array on the stack: $hit"
    fail=1
done < <(grep -rn "^[[:space:]]*chain_param_info_t [a-z_]*\[" src/ || true)

# The segment must be able to hold the struct; the compile-time check in
# shadow_constants.h enforces it, this says so where a reader will look.
value_len=$(grep -oE '^#define SHADOW_PARAM_VALUE_LEN +[0-9]+' src/host/shadow_constants.h | grep -oE '[0-9]+$')
buffer=$(grep -oE '^#define SHADOW_PARAM_BUFFER_SIZE +[0-9]+' src/host/shadow_constants.h | grep -oE '[0-9]+$')
if [ "$buffer" -le "$value_len" ]; then
    echo "FAIL: SHADOW_PARAM_BUFFER_SIZE ($buffer) must exceed SHADOW_PARAM_VALUE_LEN ($value_len)"
    fail=1
fi

[ "$fail" -eq 0 ] && echo "PASS: param buffers are static, and the segment holds the struct ($value_len in $buffer)"
exit "$fail"
