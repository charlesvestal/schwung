#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE FX GATE MUST BE CLOSED BEFORE THE WORKER IS POSTED, and one call site had
# it the other way round.
#
# chain_bus_apply_patch rewrote fx_request[] and then ran
#
#     if (!buf) chain_bus_request_alloc(inst, b);   /* -> post -> sem_post */
#     bus_request_work(inst, b);                    /* the close */
#
# so a worker that read fx_req_seq in that one-statement window ran a whole
# reconcile -- bus_unload_fx, destroy_instance, dlclose -- while the RT side
# still saw fx_ready == fx_req_seq and called process_block on the instance
# being destroyed. That is exactly the use-after-free the sequence number
# replaced the boolean to prevent, and the comment above it asserted the safe
# ordering the code did not have.
#
# A comment cannot enforce an ordering, so this does. Three pins:
#
#   1. Only bus_close_fx_gate writes fx_req_seq. A second bumper is a second
#      close nobody would find.
#   2. bus_request_work closes before it posts.
#   3. EVERY call that can post (chain_bus_request_alloc / chain_bus_post_work /
#      bus_post_work) is preceded within 15 lines either by a close, or by a
#      comment saying in so many words that there is "No FX change here" -- so a
#      new call site has to answer the question rather than inherit the answer.

f=src/modules/chain/dsp/chain_bus.c
fail=0
say() { echo "FAIL: $1" >&2; fail=1; }

# ---- 1. one bumper ---------------------------------------------------------
bumpers=$(grep -n '__atomic_store_n(&bus->fx_req_seq' "$f" | wc -l | tr -d ' ')
[ "$bumpers" = "1" ] || say "fx_req_seq is stored at $bumpers sites; the close must live in exactly one (bus_close_fx_gate)"
grep -q 'static void bus_close_fx_gate(slot_bus_t \*bus)' "$f" ||
  say "bus_close_fx_gate is gone -- the close and the post are one call again"

# ---- 2. the pair, in order -------------------------------------------------
pair=$(awk '/^static void bus_request_work\(/,/^}/' "$f")
close_at=$(printf '%s\n' "$pair" | grep -n 'bus_close_fx_gate(' | head -1 | cut -d: -f1 || true)
post_at=$(printf '%s\n' "$pair" | grep -n 'bus_post_work(' | head -1 | cut -d: -f1 || true)
if [ -z "$close_at" ] || [ -z "$post_at" ]; then
  say "bus_request_work no longer closes AND posts"
elif [ "$close_at" -ge "$post_at" ]; then
  say "bus_request_work posts before it closes"
fi

# ---- 3. every posting call site --------------------------------------------
# The definitions themselves are not call sites, and neither is a comment.
# bus_post_work's own body is the handover itself, not a call site with a
# decision to make -- it is reached only through the two above.
wrap_from=$(grep -n '^static void bus_post_work(' "$f" | cut -d: -f1)
wrap_to=$(awk -v s="$wrap_from" 'NR>=s && /^}/ {print NR; exit}' "$f")
sites=$(grep -n -E '^[^*/]*\b(chain_bus_request_alloc|chain_bus_post_work|bus_post_work)\(inst, b\);' "$f" | cut -d: -f1)
[ -n "$sites" ] || say "no posting call sites found at all -- this test is asserting nothing"
for ln in $sites; do
  if [ -n "$wrap_from" ] && [ "$ln" -ge "$wrap_from" ] && [ "$ln" -le "$wrap_to" ]; then continue; fi
  from=$(( ln > 15 ? ln - 15 : 1 ))
  ctx=$(sed -n "${from},${ln}p" "$f")
  if ! printf '%s\n' "$ctx" | grep -q -e 'bus_close_fx_gate(' -e 'No FX change'; then
    say "line $ln posts the worker with no close before it and no reason given (see the header of this test)"
  fi
done

[ "$fail" = 0 ] || exit 1
echo "PASS: the bus FX gate is closed before the worker is posted"
