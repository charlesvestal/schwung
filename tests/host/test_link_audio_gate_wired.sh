#!/usr/bin/env bash
#
# The Link Audio frame gate (la_rebuild_gate, link_audio_conceal.h) is unit
# tested in test_link_audio_conceal.c -- but a unit test of a header proves
# nothing about the shim that must call it. The defect it fixes lived in the
# CALL SITE: a bare `any_la_valid` let four concealed tracks keep the rebuild
# zeroing Move's mailbox through a shared stall, then cut into native at full
# level with no ramp. So pin the wiring:
#   - the reader tells a concealed block from a real one,
#   - the shim decides through the gate, not through "any track filled",
#   - both edges get their ramp (native in on fallback, native out on resume),
#   - the rebuild engage edge forgets stale concealment state.

set -u
cd "$(dirname "$0")/../.."
fail=0
shim=src/schwung_shim.c
la=src/host/shadow_link_audio.c

need() {  # file pattern message
    if ! grep -qE "$2" "$1"; then echo "FAIL: $3"; fail=1; fi
}

need "$la"   'return LA_READ_CONCEALED;' \
     "the reader no longer reports a concealed block as LA_READ_CONCEALED"
need "$shim" 'la_rebuild_gate\(&shim_la_gate, la_real, la_concealed\)' \
     "the shim does not decide rebuild-vs-native through la_rebuild_gate"
need "$shim" 'LA_GATE_FALLBACK_RAMP_IN\)' \
     "the fallback edge no longer ramps Move's native mix in"
need "$shim" 'la_ramp_in\(mailbox_audio, FRAMES_PER_BLOCK\)' \
     "the fallback edge no longer ramps the mailbox"
need "$shim" 'la_mix_ramp_out\(mailbox_audio, la_native_xfade, FRAMES_PER_BLOCK\)' \
     "the resume edge no longer crossfades native out"
need "$shim" 'link_audio_conceal_reset\(\);' \
     "the rebuild engage edge no longer resets concealment state"

if grep -qE 'any_la_valid' "$shim"; then
    echo "FAIL: any_la_valid is back -- 'any track filled' cannot tell a shared stall from a single one"
    fail=1
fi

# The ramp-out must come AFTER master volume and speaker EQ: native audio is
# already at master volume and through Move's own enhancer.
eq_line=$(grep -n 'speaker_eq_process(mailbox_audio' "$shim" | head -1 | cut -d: -f1)
xf_line=$(grep -n 'la_mix_ramp_out(mailbox_audio' "$shim" | head -1 | cut -d: -f1)
if [ -z "$eq_line" ] || [ -z "$xf_line" ] || [ "$xf_line" -le "$eq_line" ]; then
    echo "FAIL: the native crossfade must be added after speaker EQ (eq=$eq_line xfade=$xf_line)"
    fail=1
fi

[ $fail -eq 0 ] && echo "test_link_audio_gate_wired: all passed"
exit $fail
