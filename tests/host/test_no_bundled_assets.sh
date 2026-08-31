#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# NO PROPRIETARY ASSET IS EVER COMMITTED.
#
# The fleet audit wants modules captured with their real ROMs, soundfonts and
# sample banks loaded, because a module captured cold reports a contract that
# is not the one users see — sf2 with no soundfont present declares three
# parameters. The assets that fix that are JV-880 ROMs, commercial soundfonts
# and sample libraries: not ours to redistribute, and large.
#
# So they are NEVER COPIED INTO THE TREE. They are referenced by absolute path
# from tools/param-pages/assets.local.json, which is itself ignored, and pushed
# to the device by provision_assets.sh. Nothing under this repo ever holds one.
#
# That rule is worth exactly as much as its enforcement, which is why this is a
# test and not a paragraph in a README. A .gitignore does not protect against
# `git add -f`, against a pattern nobody updated when a new extension showed
# up, or against an asset landing somewhere the pattern does not reach. This
# checks what git is ACTUALLY TRACKING, which is the only thing that can end up
# pushed.
#
# If this fails, do not add an exclusion — remove the file:
#   git rm --cached <path>
# and if it has already been pushed, the history needs rewriting, because a
# blob stays fetchable long after the commit that removed it.

# Extensions that are asset payloads rather than source. Deliberately broad:
# a false positive costs one conversation, a false negative ships a ROM.
ASSET_RE='\.(sf2|sfz|rom|syx|bin|wav|aiff?|ogg|flac|mp3|m4a|nam|pcm|dat|rx2|rex|exl|sysex)$'

fail=0

tracked=$(git ls-files | grep -Ei "$ASSET_RE" || true)
if [ -n "$tracked" ]; then
  echo "FAIL: asset payload(s) are TRACKED BY GIT:"
  echo "$tracked" | sed 's/^/    /'
  echo "  Remove with: git rm --cached <path>   (do not add an ignore rule)"
  fail=1
else
  echo "PASS: no asset payloads tracked by git"
fi

# The local asset map holds absolute paths into someone's music library. It is
# not itself an asset, but it is nobody else's business and it is machine
# specific, so it must stay untracked too.
if git ls-files --error-unmatch tools/param-pages/assets.local.json >/dev/null 2>&1; then
  echo "FAIL: tools/param-pages/assets.local.json is tracked — it is machine-local by design"
  fail=1
else
  echo "PASS: assets.local.json is not tracked"
fi

# The ignore rules have to actually cover the extensions above, or the only
# thing standing between a ROM and a commit is whoever is typing. Checked with
# check-ignore against a probe path per extension rather than by reading the
# patterns, so it tests the behaviour git will have, not our reading of it.
missing=""
for ext in sf2 sfz rom syx bin wav aiff ogg flac mp3 nam pcm rx2; do
  if ! git check-ignore -q "probe-asset.$ext" 2>/dev/null; then
    missing="$missing $ext"
  fi
done
if [ -n "$missing" ]; then
  echo "FAIL: .gitignore does not cover:$missing"
  echo "  A tracked-file check alone catches the mistake only AFTER someone makes it."
  fail=1
else
  echo "PASS: .gitignore covers every asset extension the tracked-file check looks for"
fi

exit $fail
