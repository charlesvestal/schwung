#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two halves, and both are needed.
#
#  1. The behaviour: build and run test_chain_deferred_load.c, which measures
#     WHERE a module gets built and what priority the thread it spawns is born
#     at. See that file's header.
#  2. The wiring: a perfect loader that nothing calls would pass every
#     assertion in (1). So pin that `synth:module` actually reaches it, that
#     the commit sits on the render path, and that the old blocking sequence is
#     no longer what a module write does.

work="$(mktemp -d "${TMPDIR:-/tmp}/schwung-deferred-load.XXXXXX")"
trap 'rm -rf "$work"' EXIT

# chain_internal.h includes <malloc.h> (glibc-only) and chain_host.c calls
# malloc_trim. One shim header so this builds on macOS as well as Linux; it
# changes nothing about the code under test.
mkdir -p "$work/shim"
cat > "$work/shim/malloc.h" <<'EOF'
#include <stdlib.h>
extern int malloc_trim(size_t);
EOF

bin="build/tests/test_chain_deferred_load"
mkdir -p "$(dirname "$bin")"

cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -I"$work/shim" -Isrc -Isrc/modules/chain/dsp -Isrc/host \
  tests/host/test_chain_deferred_load.c \
  src/modules/chain/dsp/chain_loader.c \
  -o "$bin" -lpthread

"$bin"

# ---------------------------------------------------------------- wiring pins

host="src/modules/chain/dsp/chain_host.c"
fail=0
note() { echo "WIRING FAIL: $*"; fail=1; }

# A `synth:module` write must ASK the loader, not load inline.
if ! command grep -q 'chain_loader_request_synth(inst, val)' "$host"; then
  note "synth:module no longer routes through chain_loader_request_synth"
fi

# The commit has to be on the render path. Anywhere else and a staged module
# is built and never published — which looks exactly like a module that failed
# to load, with no error anywhere.
if ! awk '/^static void v2_render_block/,/^}/' "$host" | command grep -q 'chain_loader_commit(inst)'; then
  note "chain_loader_commit is not called from v2_render_block"
fi

# The old blocking sequence must not be what an ordinary module write does.
# It survives ONLY as the fallback when a loader cannot be started, so the
# blocking call has to sit INSIDE the request's failure branch. Checking that
# both lines exist would pass on a revert that simply calls both.
if ! awk '
    /if \(chain_loader_request_synth\(inst, val\) != 0\) \{/ { armed = NR }
    armed && NR > armed && NR <= armed + 8 && /v2_load_synth\(inst, val\);/ { found = 1 }
    END { exit(found ? 0 : 1) }
' "$host"; then
  note "the synchronous v2_load_synth is not inside the failed-request branch"
fi

# ...and that is the ONLY place a module write may load synchronously.
n_sync="$(command grep -c 'v2_load_synth(inst, val)' "$host" || true)"
if [ "$n_sync" != "1" ]; then
  note "expected exactly 1 synchronous v2_load_synth(inst, val) call, found $n_sync"
fi

# is_loading must answer exactly 1/0 — any other answer latches the component
# as not-implementing-it in the shadow UI, permanently.
if ! command grep -q 'chain_loader_synth_busy(inst) ? 1 : 0' "$host"; then
  note "synth:is_loading does not report a bare 1/0"
fi

# The loader must be joined before the instance is torn down; detaching would
# race the free of everything the in-flight load is standing on.
if ! awk '/^static void v2_destroy_instance/,/^}/' "$host" \
     | command grep -q 'chain_loader_shutdown(inst)'; then
  note "v2_destroy_instance does not shut the loader down"
fi

# The SPI-side of the loader must not take a lock. An RT thread blocking on a
# mutex held by a SCHED_OTHER thread is unbounded priority inversion — the
# exact defect the 2026-08-22 audit flagged in minijv's ring_mutex.
spi_side="$(awk '/^int chain_loader_request_synth/,/^}/' src/modules/chain/dsp/chain_loader.c
            awk '/^void chain_loader_commit/,/^}/'      src/modules/chain/dsp/chain_loader.c
            awk '/^int chain_loader_synth_busy/,/^}/'   src/modules/chain/dsp/chain_loader.c)"
if echo "$spi_side" | command grep -qE 'pthread_mutex_lock|pthread_cond_|sem_wait|sem_post'; then
  note "an SPI-thread entry point takes a lock or blocks — priority inversion"
fi

if [ "$fail" -ne 0 ]; then
  echo "wiring pins FAILED"
  exit 1
fi
echo "ok: wiring pins"
