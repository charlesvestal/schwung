#!/usr/bin/env bash
# THE SHIM'S DEPENDENCY LIST IS DERIVED, NEVER ENUMERATED.
#
# scripts/build.sh skips a target whose file is newer than every source it is
# told about. That list was hand-written, and five headers had fallen out of
# it: ui_midi_out_carry.h, ui_midi_out_ring.h, recall_quantize.h,
# transport_grid.h and chain_idle_tick.h.
#
# Editing any of them left the shim NOT rebuilt while package.sh repackaged
# regardless -- a fresh tarball timestamp around an unchanged binary, with
# install.sh reporting success. The device then runs code nobody compiled.
#
# On 2026-09-11 that cost three deploys and, far worse, INVERTED AN EXPERIMENT:
# a fix was reported from hardware as "even worse" and reverted on that
# evidence, when the binary under test had never contained it. A build step
# that can be skipped silently defeats every measurement that follows -- the
# same lesson as build.sh skipping the Link sidecar and defeating a
# three-version bisect.
#
# Enumerating fixes it once and re-breaks it the next time somebody adds a
# header, which is exactly how it reached five. So this pins the DERIVATION,
# not any particular list.
set -euo pipefail
cd "$(dirname "$0")/../.."

BUILD=scripts/build.sh
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$BUILD" ] || { echo "FAIL: $BUILD missing" >&2; exit 1; }

# The derived variable must exist and must be built from a glob, not a list.
if ! grep -qE '^SRC_HEADERS="\$\(ls .*\*\.h' "$BUILD"; then
    fail "SRC_HEADERS is not derived from a glob -- a hand-written header list rots, and a stale shim inverts every hardware measurement taken against it"
fi

# It must cover the three trees whose headers the shim actually compiles.
for tree in 'src/host/\*\.h' 'src/lib/\*\.h' 'src/modules/chain/dsp/\*\.h'; do
    grep -qE "^SRC_HEADERS=.*$tree" "$BUILD" ||
        fail "SRC_HEADERS does not cover $tree"
done

# And the shim's staleness check must actually USE it.
shim_line=$(grep -n 'needs_rebuild build/schwung-shim.so' "$BUILD" | head -1 | cut -d: -f1)
[ -n "$shim_line" ] || fail "cannot find the shim's needs_rebuild call; this pin needs rewriting"
if [ -n "$shim_line" ]; then
    # The invocation runs until the line ending in `; then`.
    end=$(awk -v s="$shim_line" 'NR>=s && /; then/ {print NR; exit}' "$BUILD")
    [ -n "$end" ] || fail "the shim needs_rebuild call has no terminator"
    if [ -n "$end" ]; then
        body=$(sed -n "${shim_line},${end}p" "$BUILD")
        case "$body" in
            *'$SRC_HEADERS'*) ;;
            *) fail "the shim's needs_rebuild does not include \$SRC_HEADERS -- editing a header would leave the deployed shim stale, silently" ;;
        esac
    fi
fi

# The specific headers that were missing, as a canary: each must now be covered
# by the glob. This does not re-enumerate the list (the glob is the contract) --
# it asserts the trees are the right ones by checking the known casualties land
# inside them.
for h in src/host/ui_midi_out_carry.h src/host/ui_midi_out_ring.h \
         src/host/recall_quantize.h src/host/transport_grid.h; do
    [ -f "$h" ] || continue
    case "$h" in
        src/host/*) ;;
        *) fail "$h is outside every tree SRC_HEADERS globs" ;;
    esac
done

[ "$fails" -eq 0 ] || { echo "$fails check(s) failed" >&2; exit 1; }
echo "PASS: the shim rebuilds on any header change"
