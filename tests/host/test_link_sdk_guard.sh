#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# build.sh's Link SDK guard, RUN rather than grepped.
#
# The guard is a hard failure on purpose: link-subscriber is the only reception
# path for Move->Schwung audio, package.sh adds it only "if it was built", and
# install.sh never installs one -- so a missing sidecar rides through weeks of
# deploys as the one component nobody is varying.
#
# But it asked only `[ -d libs/link/include/ableton ]`, and libs/link carries a
# nested submodule of its own. `git submodule update --init libs/link` without
# --recursive leaves Ableton's headers present and asio absent, which is
# neither "present" nor "absent" as far as that test could tell: it took the
# present branch, the g++ line ran, and link_subscriber.cpp died on a missing
# asio.hpp. Under `set -e` that is BEFORE the chain DSP rule, so the artifact
# under test was never built and the failure named a file nobody had touched.
# SCHWUNG_ALLOW_NO_LINK_SDK=1 could not rescue it either, because the branch
# that reads it was never reached.
#
# A grep pin cannot see that -- the buggy guard and the fixed one are both
# `[ -d ... ]` lines. So this test EXTRACTS link_sdk_state from build.sh and
# runs it against real directory trees, including the partial one that was
# waved through.

status=0
ok()  { printf 'PASS: %s\n' "$1"; }
bad() { printf 'FAIL: %s\n' "$1" >&2; status=1; }

# ------------------------------------------------- extract the real predicate
# From the function's own line to its closing brace at column 0. Extracting
# rather than restating is the point: a copy here could agree with itself
# while build.sh drifted.
src="$(awk '/^link_sdk_state\(\) \{/ { grab = 1 } grab { print } grab && /^\}/ { exit }' scripts/build.sh)"
if ! printf '%s' "$src" | grep -q '^}'; then
  bad "could not extract link_sdk_state() from scripts/build.sh (renamed or reshaped?)"
  exit 1
fi
eval "$src"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# ------------------------------------------------------------ a COMPLETE tree
full="$tmp/full"
mkdir -p "$full/include/ableton" "$full/modules/asio-standalone/asio/include"
: > "$full/include/ableton/LinkAudio.hpp"
: > "$full/modules/asio-standalone/asio/include/asio.hpp"
out="$(link_sdk_state "$full")" || true
if [ "$out" = "ok" ]; then
  ok "a complete SDK answers ok"
else
  bad "a complete SDK answered '$out' -- the guard would refuse a working checkout"
fi

# -------------------------------------------------------------- ABSENT: no dir
out="$(link_sdk_state "$tmp/nothing-here")" || true
case "$out" in
  absent*) ok "an uninitialised submodule is reported absent" ;;
  *)       bad "a missing libs/link answered '$out', want absent" ;;
esac

# ------------------------------------------------- ABSENT: present but EMPTY
# What `git clone` without any submodule init leaves behind.
mkdir -p "$tmp/empty"
out="$(link_sdk_state "$tmp/empty")" || true
case "$out" in
  absent*) ok "an empty libs/link directory is reported absent" ;;
  *)       bad "an empty libs/link answered '$out', want absent" ;;
esac

# ------------------------------------- PARTIAL: the case that killed the build
# `--init` without `--recursive`: Ableton's headers land, asio does not.
part="$tmp/partial"
mkdir -p "$part/include/ableton"
: > "$part/include/ableton/LinkAudio.hpp"
out="$(link_sdk_state "$part")" || true
case "$out" in
  partial*asio*) ok "a partial SDK (no asio) is reported partial, naming asio" ;;
  ok)            bad "THE ORIGINAL BUG IS BACK: a partial SDK answers ok, so the compile runs and dies on asio.hpp" ;;
  *)             bad "a partial SDK answered '$out', want 'partial <path to asio>'" ;;
esac

# ------------------------- PARTIAL: an SDK pinned before the public audio API
part2="$tmp/old-pin"
mkdir -p "$part2/include/ableton" "$part2/modules/asio-standalone/asio/include"
: > "$part2/modules/asio-standalone/asio/include/asio.hpp"
out="$(link_sdk_state "$part2")" || true
case "$out" in
  partial*LinkAudio.hpp) ok "an SDK without LinkAudio.hpp is reported partial, naming it" ;;
  *)                     bad "an SDK missing LinkAudio.hpp answered '$out', want 'partial <path to LinkAudio.hpp>'" ;;
esac

# --------------------------------- a non-ok answer must be a NON-ZERO return
# The caller's `|| true` depends on nothing, but a future caller branching on
# the status must not be handed success for a broken tree.
if link_sdk_state "$part" >/dev/null 2>&1; then
  bad "link_sdk_state returned 0 for a partial tree"
else
  ok "link_sdk_state returns non-zero for a partial tree"
fi

# ------------------------------------------- the guard still HARD-FAILS
# Softening it back to a warning is the defect this whole block exists to
# prevent, and the opt-out must cover the partial case as well as the absent
# one -- it did not, because the branch reading it was unreachable.
if awk '/^else$/ { in_else = 1 }
        in_else && /SCHWUNG_ALLOW_NO_LINK_SDK/ { saw_optout = 1 }
        in_else && saw_optout && /exit 1/ { ok = 1 }
        END { exit !ok }' scripts/build.sh; then
  ok "the else branch still exits 1 unless SCHWUNG_ALLOW_NO_LINK_SDK is set"
else
  bad "build.sh no longer hard-fails on a missing Link SDK -- a warning is not a check"
fi

# The predicate's answer is what selects the branch, so the `if` must consult
# it and nothing else. A bare `-d` test back in front of the block is the
# regression, and it would not fail any check above.
if grep -q '^if \[ "\$link_sdk" = "ok" \]; then' scripts/build.sh; then
  ok "the sidecar block branches on link_sdk_state's answer"
else
  bad "the sidecar block no longer branches on link_sdk_state -- a bare -d test cannot see a partial SDK"
fi

exit $status
