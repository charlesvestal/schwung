#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A SysEx fragment reaches an opted-in slot ONCE per physical event.
#
# Channel-voice events are protected by event_dedup_check_and_record, and the
# comment above the MIDI_IN walk says exactly why that is load-bearing:
#
#     Don't break on zero -- Move's firmware partially consumes MIDI_IN and
#     events at higher offsets may shift down or PERSIST ACROSS FRAMES; we rely
#     on the timestamp-keyed dedup to skip duplicates regardless of where the
#     event lives.
#
# Two ways SysEx can escape that, and neither is loud:
#
#   1. DISPATCHED BEFORE THE DEDUP. A `continue` that runs ahead of
#      event_dedup_check_and_record hands the same fragment to the module again
#      on every frame the MIDI_IN slot survives.
#
#   2. DISPATCHED FROM BOTH WALKERS. shadow_dispatch_direct_external_midi and
#      shadow_dispatch_cable2_channeled_slots read the SAME buffer in the SAME
#      frame; the first returns early only when no slot is receive=All +
#      forward=THRU. Configure a slot for MPE -- which the manual recommends --
#      and both walk it, so a fragment dispatched from each arrives twice. The
#      two per-dispatcher rings cannot see each other, so moving the call after
#      the dedup does NOT fix this one.
#
# Duplication is worse than loss here. A module reassembles for itself, and its
# first rule is "start on 0xF0" -- so a repeated F0 silently RESTARTS the
# message and a repeated body byte corrupts it. The result is a plausible
# message that is fiction, not an obvious gap.
#
# A probe that echoes on seeing an F0 cannot detect either: it proves reception,
# and one delivery and six look identical to it.

fail() { echo "FAIL: $1" >&2; exit 1; }

host="src/host/shadow_midi.c"
[ -f "$host" ] || fail "$host is gone"

grep -q "shadow_chain_dispatch_sysex_to_slots" "$host" || \
  fail "shadow_chain_dispatch_sysex_to_slots is gone -- slots cannot receive SysEx"

# ---------------------------------------------------------------------------
# 0. The cleanest fix is a dedup INSIDE the dispatcher: it covers both walkers
#    at once and cannot be reintroduced by a third call site added later. If
#    that is how it is done, the ordering questions below do not arise.
# ---------------------------------------------------------------------------
dispatch_blk=$(awk '/^void shadow_chain_dispatch_sysex_to_slots/,/^}/' "$host")
if grep -q "event_dedup_check_and_record\|sysex_dedup" <<<"$dispatch_blk"; then
    deduped_inside=1
else
    deduped_inside=0
fi

# ---------------------------------------------------------------------------
# 1. Otherwise: within each walker, the dispatch must come AFTER the dedup.
# ---------------------------------------------------------------------------
check_order() {
    [ "$deduped_inside" = 1 ] && return 0
    local fn="$1"
    local body
    body=$(awk "/^void ${fn}\\(void\\)/,/^}/{ print NR\": \"\$0 }" "$host")
    [ -n "$body" ] || fail "$fn is gone from $host"

    # Not every walker has to dispatch SysEx; but one that does must dedup first.
    local disp dedup
    disp=$(grep -n "shadow_chain_dispatch_sysex_to_slots" <<<"$body" | head -n 1 | cut -d: -f2 || true)
    [ -n "${disp:-}" ] || return 0

    dedup=$(grep -n "event_dedup_check_and_record" <<<"$body" | head -n 1 | cut -d: -f2 || true)
    [ -n "${dedup:-}" ] || \
      fail "$fn dispatches SysEx but never dedups at all -- every frame the MIDI_IN slot survives re-delivers it"

    [ "$disp" -gt "$dedup" ] || \
      fail "$fn dispatches SysEx at line $disp, BEFORE its dedup at line $dedup -- a fragment in a slot that persists across frames is handed to the module again every frame"
}

check_order shadow_dispatch_direct_external_midi
check_order shadow_dispatch_cable2_channeled_slots

# ---------------------------------------------------------------------------
# 2. Two walkers, two rings: dispatching from both delivers twice regardless.
# ---------------------------------------------------------------------------
sites=$(grep -c "^[^*]*shadow_chain_dispatch_sysex_to_slots(&" "$host" || true)
if [ "$deduped_inside" = 0 ] && [ "${sites:-0}" -gt 1 ]; then
    grep -q "g_sysex_dedup" "$host" || \
      fail "SysEx is dispatched from $sites walkers, which both read the same MIDI_IN buffer in the same frame, and there is no shared SysEx dedup ring -- with a receive=All + forward=THRU slot present, every fragment is delivered twice"
fi

# ---------------------------------------------------------------------------
# 3. The properties that are already right, so they stay right.
# ---------------------------------------------------------------------------

# Opt-in. SysEx payload bytes are < 0x80, so a module switching on
# `msg[0] & 0xF0` reads data as a status type it half-recognises.
blk=$(awk '/^void shadow_chain_dispatch_sysex_to_slots/,/^}/' "$host")
grep -q "wants_sysex" <<<"$blk" || \
  fail "the dispatcher no longer gates on wants_sysex -- every chain module would start receiving SysEx"

# Length comes from the CIN. There is no length byte to trust, and the trailing
# bytes of an end-packet are padding, not data.
grep -q "case 0x05: n = 1" <<<"$blk" || \
  fail "the CIN-to-length mapping is gone; a 1-byte tail would be read as 3 and pad the message with junk"

# A per-position array that does not permute is the documented way to make a
# chain shape edit silently mis-attribute state.
reorder="src/modules/chain/dsp/chain_reorder.c"
[ -f "$reorder" ] || fail "$reorder is gone"
grep -q "wants_sysex" "$reorder" || \
  fail "wants_sysex is not permuted in $reorder -- an fx:insert/remove/move would move the capability to the wrong position"

echo "PASS: a SysEx fragment reaches an opted-in slot once per physical event"
