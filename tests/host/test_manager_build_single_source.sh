#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# scripts/build-manager.sh is THE builder for the manager binary, and it is
# LOUD: local go, else a golang container, else a non-zero exit.
#
# It exists because there were THREE builders and they did not agree.
# install.sh's was guarded on `command -v go`, so on a machine with Docker and
# no local Go the whole block evaluated to false and was skipped IN SILENCE --
# the only warning sat on the build-FAILED branch, inside an `if` that never
# ran. `install.sh local` then uploaded whatever schwung-manager happened to be
# in the tarball already and reported success, so a manager fix could be
# deployed, confirmed deployed, and still not be running. An afternoon went
# into a Remote UI fix that provably never reached the device.
#
# This pins the shape that cannot regress quietly: one builder, no `go` guard
# in front of it, every caller through it, and a HARD check that the binary
# reached the tarball -- the rules the link sidecar earned the same way.

status=0
ok()  { printf 'PASS: %s\n' "$1"; }
bad() { printf 'FAIL: %s\n' "$1" >&2; status=1; }

# A comment is prose about the bug: it must neither satisfy a pin nor trip one.
# awk on a file argument, never a pipeline -- a `grep -q` closing a pipe early
# under `set -o pipefail` is how the first draft of this test silently stopped
# scanning after its first hit.
# Exit 0 = matched, 1 = did not. Anything else is awk refusing the pattern,
# which must NOT read as "did not match" -- a broken regex reporting PASS is
# how a pin comes to measure nothing.
has() {
  local rc=0
  awk -v pat="$2" '/^[[:space:]]*#/ { next } $0 ~ pat { found = 1 } END { exit !found }' "$1" || rc=$?
  if [ "$rc" -gt 1 ]; then
    printf 'FAIL: awk rejected the pattern %s (test bug, not a source finding)\n' "$2" >&2
    exit 2
  fi
  return "$rc"
}

scanned_files() { git ls-files 'scripts/*.sh' '.github/workflows/*.yml'; }

# ----------------------------------------------------------------- 1. builder
if [ -x scripts/build-manager.sh ]; then
  ok "scripts/build-manager.sh exists and is executable"
else
  bad "scripts/build-manager.sh must exist and be executable"
fi

# -------------------------------------------------- 2. it is the ONLY builder
# ci.yml's `go build ./...` is a compile check, not the artifact -- the rule is
# about a build naming schwung-manager as its OUTPUT.
offenders=""
for f in $(scanned_files); do
  case "$f" in scripts/build-manager.sh) continue ;; esac
  if has "$f" 'go build.*-o[[:space:]]+[^[:space:]]*schwung-manager'; then
    offenders="$offenders $f"
  fi
done
if [ -z "$offenders" ]; then
  ok "one builder: nothing but build-manager.sh compiles the manager binary"
else
  bad "a second manager builder is back in:$offenders -- call scripts/build-manager.sh instead"
fi

# -------------------------------------------------- 3. every caller uses it
for f in scripts/build.sh scripts/install.sh .github/workflows/release.yml; do
  if has "$f" 'build-manager\.sh'; then
    ok "$f builds the manager through the shared builder"
  else
    bad "$f does not call scripts/build-manager.sh"
  fi
done

# --------------------------------------------- 4. no `go` guard in install.sh
# THE GUARD WAS THE BUG: `command -v go` in front of the block is what made a
# Docker-only machine skip it without a word.
if has scripts/install.sh 'command -v go'; then
  bad "install.sh gates the manager rebuild on 'command -v go' again -- that guard IS the silent skip"
else
  ok "install.sh does not gate the manager rebuild on a local 'go'"
fi

# ------------------------------------- 5. a missing binary FAILS, never warns
# ci.yml says it in its own words, about link-subscriber: "A soft check on the
# one file that can rot invisibly is no check."
for f in .github/workflows/release.yml .github/workflows/ci.yml; do
  if has "$f" 'schwung-manager.*\\|\\|[[:space:]]*echo'; then
    bad "$f warns instead of failing when schwung-manager is missing from the tarball"
  else
    ok "$f does not soft-warn about a missing schwung-manager"
  fi
done

# ----------------------------------- 6. CI actually looks inside the tarball
# Without this the fix ships unpinned: cross-compile builds through Docker,
# where build.sh's manager block is skipped by /.dockerenv, so the CI tarball
# never carried a manager for anything to check.
if has .github/workflows/ci.yml 'schwung/schwung-manager\\$'; then
  ok "CI verifies schwung-manager reached the packaged tarball"
else
  bad "CI does not verify schwung-manager reached the tarball"
fi

exit $status
