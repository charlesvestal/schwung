#!/usr/bin/env bash
# Pins the clip-phase seam: it is a dlsym'd chain export, NOT a host_api field,
# and it is pushed ahead of the idle gate.
set -euo pipefail
cd "$(dirname "$0")/../.."
fail() { echo "FAIL: $1"; exit 1; }

# 1. The host ABI is untouched. The front of `reserved` IS +120 -- the offset a
#    shipped breakbeat build calls as get_project_bpm() -- so consuming it puts
#    a live pointer there and boot-loops the device. test_host_api_reserved_tail
#    cannot see that (it inspects a zeroed struct), so the rule is pinned here.
grep -q 'void \*reserved\[8\];' src/host/plugin_api_v1.h \
  || fail "host_api_v1_t's reserved tail changed — see the plan's ABI note"

# 2. The phase seam is a dlsym, like chain_take_midi_tick_wake and
#    move_plugin_render_split, and NOT a host_api field.
grep -q 'dlsym(shadow_dsp_handle, "chain_set_clip_phase")' \
  src/host/shadow_chain_mgmt.c || fail "chain_set_clip_phase is not dlsym'd"
if grep -q 'clip_phase' src/host/plugin_api_v1.h; then
  fail "clip phase leaked into host_api_v1_t"
fi

# 3. A NULL dlsym must degrade to "phase unknown", not crash: the call site is
#    guarded on the pointer, the way every other optional chain export is.
grep -q 'if (shadow_chain_set_clip_phase)' src/schwung_shim.c \
  || fail "the clip-phase push is not guarded on a successful dlsym"

# 4. A failed resolution is visible. An optional export that resolves to NULL
#    and says nothing is a feature that silently does not exist.
grep -q 'clip_phase=%p' src/host/shadow_chain_mgmt.c \
  || fail "a failed chain_set_clip_phase dlsym is not logged"

# 5. The push precedes the idle gate, or a silent slot's lane freezes: the shim
#    skips render_block on a silent slot for 171 frames in 172.
push=$(grep -n 'shadow_chain_set_clip_phase(shadow_chain_slots' src/schwung_shim.c | head -1 | cut -d: -f1)
gate=$(grep -n 'if (shadow_slot_idle\[s\]) {' src/schwung_shim.c | head -1 | cut -d: -f1)
[ -n "$push" ] && [ -n "$gate" ] && [ "$push" -lt "$gate" ] \
  || fail "clip phase is pushed after the idle gate (push=$push gate=$gate)"

# 6. Identity and phase are separately valid (clip_state.h). The resolver fills
#    the clip slot and the fingerprint before the anchor check, so a refreshed
#    but unanchored track reaches the chain as identity-known, phase-unknown.
ident=$(grep -n '\*fp_valid = 1;' src/host/shadow_chain_mgmt.c | head -1 | cut -d: -f1)
anchor=$(grep -n 'if (!tr->anchor_valid) return 0;' src/host/shadow_chain_mgmt.c | head -1 | cut -d: -f1)
[ -n "$ident" ] && [ -n "$anchor" ] && [ "$ident" -lt "$anchor" ] \
  || fail "identity/fingerprint is not published before the anchor check (ident=$ident anchor=$anchor)"

# 7. Phase is measured from the LOOP START. clip_phase_beats() adds loop_start
#    back on (despite its header comment), so the subtraction is what makes the
#    value 0..loop_len, which is the only thing a lane can index with.
grep -q 'ph - r->loop_start' src/host/shadow_chain_mgmt.c \
  || fail "phase is not rebased onto the clip's loop start"

echo "PASS: lane phase seam"
