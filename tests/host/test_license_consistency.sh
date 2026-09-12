#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Schwung states ONE licence for its own code, and the GPL artifacts it ships
# carry their licence text with them.
#
# It did not, for five months. The 2026-03-22 commit "Schwung 0.8: rename, MIT
# license" switched LICENSE to MIT and left THIRD_PARTY_LICENSES.md asserting,
# four times, that Schwung's original code is CC BY-NC-SA 4.0 and that "the
# combined work is distributed under CC BY-NC-SA 4.0". Both cannot be true, and
# the CC one is the worse half to be wrong about: CC BY-NC-SA is not a software
# licence and its NC clause is incompatible with every GPL component below. A
# second, older copy of the file (extensionless THIRD_PARTY_LICENSES, last
# touched 2026-02-04) sat beside it listing a DIFFERENT set of components.
#
# The same document was also missing every copyleft dependency it had acquired
# -- Ableton Link, jack2, eSpeak NG -- while src/lib/jack2/shadow/
# JackShadowDriver.cpp declared itself "License: MIT" three lines above its own
# "Based on JackMoveDriver by Cycling '74 (GPL-2.0)". Its .h had the correct
# GPL block the whole time, which is what made the .cpp readable as a typo
# rather than as a claim.
#
# And none of it shipped: THIRD_PARTY_LICENSES.md was not in package.sh's
# ITEMS at all, so the tarball carried GPL-2.0 and GPL-3.0 binaries with no
# licence text and no attribution.
#
# What this pins is the shape that cannot regress quietly. It deliberately does
# NOT check the licence texts' contents -- the point is that the claims agree
# with each other and that the files reach the tarball.

status=0
ok()  { printf 'PASS: %s\n' "$1"; }
bad() { printf 'FAIL: %s\n' "$1" >&2; status=1; }

# Exit 0 = matched, 1 = did not. Anything else is awk refusing the pattern,
# which must NOT read as "did not match" -- a broken regex reporting PASS is
# how a pin comes to measure nothing. Comments are prose ABOUT the bug and must
# neither satisfy a pin nor trip one, so they are skipped for source files.
has() {
  local rc=0
  awk -v pat="$2" '/^[[:space:]]*#/ { next } $0 ~ pat { found = 1 } END { exit !found }' "$1" || rc=$?
  if [ "$rc" -gt 1 ]; then
    printf 'FAIL: awk rejected the pattern %s (test bug, not a source finding)\n' "$2" >&2
    exit 2
  fi
  return "$rc"
}

# ------------------------------------------------- 1. ONE licence for our code
if [ -f LICENSE ]; then
  ok "LICENSE exists"
else
  bad "LICENSE must exist"
fi

if grep -q 'MIT License' LICENSE 2>/dev/null; then
  ok "LICENSE is MIT"
else
  bad "LICENSE is no longer MIT -- if that is deliberate, update this test AND THIRD_PARTY_LICENSES.md together"
fi

# The contradiction itself.
#
# Scanned over the files that STATE Schwung's licence -- LICENSE, README, and
# the third-party doc -- because in those a CC BY-NC-SA mention IS the claim.
# It is deliberately NOT the whole tree: CLAUDE.md and this test describe the
# defect in order to prevent it, and a pin that cannot tell a claim from prose
# about the claim forces the documentation to be deleted to stay green.
cc_offenders=""
for f in $(git ls-files 'LICENSE*' 'THIRD_PARTY*' 'README*' | grep -v '^libs/'); do
  if grep -qiE 'CC[- ]BY[- ]NC|creative commons|BY-NC-SA' "$f" 2>/dev/null; then
    cc_offenders="$cc_offenders $f"
  fi
done
if [ -z "$cc_offenders" ]; then
  ok "no CC BY-NC-SA claim anywhere: nothing contradicts the MIT LICENSE"
else
  bad "CC BY-NC-SA claim is back in:$cc_offenders -- Schwung's own code is MIT (see LICENSE)"
fi

# ------------------------------------------- 2. exactly ONE third-party file
if [ -f THIRD_PARTY_LICENSES.md ]; then
  ok "THIRD_PARTY_LICENSES.md exists"
else
  bad "THIRD_PARTY_LICENSES.md must exist"
fi

if [ -e THIRD_PARTY_LICENSES ]; then
  bad "the extensionless THIRD_PARTY_LICENSES is back -- two divergent copies is what let the stale one rot for months; keep only the .md"
else
  ok "only one third-party licence document (no extensionless duplicate)"
fi

# README linked to the extensionless copy, so deleting that file silently broke
# the one licence pointer a visitor actually follows.
if grep -qE '\]\(THIRD_PARTY_LICENSES\)' README.md 2>/dev/null; then
  bad "README.md links to the deleted extensionless THIRD_PARTY_LICENSES -- point it at THIRD_PARTY_LICENSES.md"
else
  ok "README.md does not link the deleted extensionless file"
fi

# ----------------------------- 3. every shipped copyleft component is listed
# Keyed on what the tarball CARRIES, so a component added to the build without
# an entry here fails rather than going unnoticed.
for comp in "Ableton Link" "jack2" "eSpeak NG" "libsamplerate" "sonic" "Freeverb" "ablspi"; do
  if grep -qF "$comp" THIRD_PARTY_LICENSES.md; then
    ok "THIRD_PARTY_LICENSES.md documents $comp"
  else
    bad "THIRD_PARTY_LICENSES.md does not mention $comp -- it ships in the tarball"
  fi
done

for lic in "GPL-2.0-or-later" "GPL-3.0-or-later"; do
  if grep -qF "$lic" THIRD_PARTY_LICENSES.md; then
    ok "THIRD_PARTY_LICENSES.md names $lic"
  else
    bad "THIRD_PARTY_LICENSES.md must name $lic -- the tarball ships artifacts under it"
  fi
done

# -------------------------- 4. the JACK shadow driver is GPL, and says so
drv_c=src/lib/jack2/shadow/JackShadowDriver.cpp
drv_h=src/lib/jack2/shadow/JackShadowDriver.h
for f in "$drv_c" "$drv_h"; do
  if grep -qF 'GNU General Public License' "$f"; then
    ok "$(basename "$f") carries the GPL notice"
  else
    bad "$(basename "$f") lost its GPL notice -- it is a derivative of Cycling '74's JackMoveDriver and is built against jack2's GPL-2.0 server headers"
  fi
done

# The exact regression: an MIT declaration in a GPL-derived file. Matched on
# the licence-header form ("License: MIT") so that prose explaining the rule
# does not trip it.
if grep -qE '^[[:space:]]*License:[[:space:]]*MIT' "$drv_c" "$drv_h"; then
  bad "the JACK shadow driver declares 'License: MIT' again -- it is GPL-2.0-or-later and cannot be relicensed"
else
  ok "the JACK shadow driver makes no MIT claim"
fi

# --------------------------------- 5. the licence files reach the tarball
# This is the half that made the rest invisible: correct documents that nobody
# receiving the software ever sees.
if has scripts/build.sh 'cp \./THIRD_PARTY_LICENSES\.md \./build/'; then
  ok "build.sh stages THIRD_PARTY_LICENSES.md"
else
  bad "build.sh must copy THIRD_PARTY_LICENSES.md into build/"
fi

if has scripts/build.sh 'cp \./LICENSE \./build/'; then
  ok "build.sh stages LICENSE"
else
  bad "build.sh must copy LICENSE into build/"
fi

if has scripts/build.sh 'GPL-2\.0\.txt.*GPL-3\.0\.txt'; then
  ok "build.sh stages both GPL licence texts"
else
  bad "build.sh must copy licenses/GPL-2.0.txt and licenses/GPL-3.0.txt into build/licenses/"
fi

# Staged UNCONDITIONALLY. A `|| true` here is the link-subscriber failure shape
# exactly: a step that can be skipped in silence, shipping a non-compliant
# release that looks identical to a good one.
if awk '/cp \.\/(LICENSE|THIRD_PARTY_LICENSES\.md)|GPL-2\.0\.txt/ && /\|\| true/ { found = 1 } END { exit !found }' scripts/build.sh; then
  bad "a licence copy in build.sh is guarded with '|| true' -- these must fail loudly, not skip silently"
else
  ok "licence copies in build.sh are unconditional (no '|| true')"
fi

for item in LICENSE THIRD_PARTY_LICENSES.md; do
  if has scripts/package.sh "\./$item"; then
    ok "package.sh ships $item"
  else
    bad "package.sh ITEMS must include ./$item -- without it the tarball carries GPL binaries and no licence text"
  fi
done

if has scripts/package.sh 'ITEMS .*\./licenses"'; then
  ok "package.sh ships the licenses/ directory"
else
  bad "package.sh must ship ./licenses"
fi

# The licences directory used to be an "if it exists" -- the same silent-skip
# shape. It must be a hard check now.
if has scripts/package.sh 'refusing to package'; then
  ok "package.sh fails loudly when a licence file is missing"
else
  bad "package.sh must HARD-FAIL on a missing licence file, not package without it"
fi

# ------------------------- 6. the eSpeak LINKAGE is documented, not denied
#
# The first draft of THIRD_PARTY_LICENSES.md claimed "nothing copyleft is
# linked into schwung or schwung-shim.so". That was false: SHIM_LIBS carries
# -lespeak-ng under SCREEN_READER_ENABLED=1 (the default and shipping config),
# and libespeak-ng.so.1 is a NEEDED entry of the built shim. eSpeak NG is
# GPL-3.0-or-later, so that binary is conveyed under GPL-3.0-or-later.
#
# This pin is two-sided on purpose. If the linkage goes away, the docs must
# stop saying it exists; while it is there, they must not deny it.
if has scripts/build.sh '\-lespeak-ng'; then
  if grep -qF 'GPL-3.0-or-later' THIRD_PARTY_LICENSES.md && \
     grep -qiE 'schwung-shim\.so.*(linked|combined|GPL-3)' THIRD_PARTY_LICENSES.md; then
    ok "the shim's eSpeak linkage is documented as making a GPL-3.0 binary"
  else
    bad "build.sh links -lespeak-ng but THIRD_PARTY_LICENSES.md does not say schwung-shim.so is conveyed under GPL-3.0-or-later"
  fi
else
  ok "no -lespeak-ng in build.sh (shim links no copyleft)"
fi

# The exact false claim, in any of the places that state the licence.
denial=""
for f in THIRD_PARTY_LICENSES.md README.md; do
  if grep -qiE 'no copyleft component is linked into|nothing copyleft is linked into' "$f" 2>/dev/null; then
    denial="$denial $f"
  fi
done
if [ -z "$denial" ]; then
  ok "no blanket 'nothing copyleft is linked' denial"
else
  bad "a blanket 'nothing copyleft is linked' claim is back in:$denial -- schwung-shim.so links eSpeak NG (GPL-3.0+) in the default build"
fi

# ---------------------------------------- 7. the vendored texts are present
for t in licenses/GPL-2.0.txt licenses/GPL-3.0.txt; do
  if [ -s "$t" ]; then
    ok "$t is present and non-empty"
  else
    bad "$t is missing -- GPL-2.0 and GPL-3.0 both require the licence text to travel with the binaries"
  fi
done

exit "$status"
