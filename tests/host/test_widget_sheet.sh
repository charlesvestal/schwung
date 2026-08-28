#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# docs/WIDGETS.md IS GENERATED, AND MUST NOT DRIFT.
#
# The sheet is the one place that says what each widget means, which rule
# selects it, and how many cells in the fleet it accounts for. Every one of
# those is derived from the shipping code, so a widget change silently
# invalidates the page — and a documentation page that is confidently wrong is
# worse than none, because it is what the next person reads instead of the
# render.
#
# So the generator has a --check mode: it rebuilds the markdown and every
# swatch into memory and byte-compares. Change a widget and this fails with
# the command that fixes it.
#
# This is the same bargain as tests/fixtures/movy-geom-baseline.txt — a
# deliberate refresh is a reviewed diff, and an accidental one is a red test.
# It is stricter than the baseline in one way that matters: the baseline pins
# hashes, so a changed picture tells you only THAT it changed. Here the diff
# is the PNG itself and the prose beside it, so the review is the picture.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the widget sheet check" >&2
  exit 1
fi

if [ ! -f docs/WIDGETS.md ]; then
  echo "FAIL: docs/WIDGETS.md is missing — generate it with" >&2
  echo "      node tools/param-pages/widget_sheet.mjs" >&2
  exit 1
fi

# The generator prints its own failure, naming every stale file.
node tools/param-pages/widget_sheet.mjs --check

# --- the check must be able to FAIL -----------------------------------------
#
# A --check that passes unconditionally is the failure mode this whole file
# exists to prevent, and it is invisible: it prints PASS either way. So prove
# it by corrupting a byte of the committed markdown, re-running, and requiring
# a non-zero exit. Restored from a copy rather than regenerated, so a broken
# generator cannot leave the tree modified.
# mktemp, not $TMPDIR: it is unset on the CI runner, and "$TMPDIR/x" would
# then write to /x and fail the test for a reason unrelated to widgets.
BACKUP="$(mktemp)"
cp docs/WIDGETS.md "$BACKUP"
restore() { cp "$BACKUP" docs/WIDGETS.md; rm -f "$BACKUP"; }
trap restore EXIT

printf '\n<!-- deliberate corruption -->\n' >> docs/WIDGETS.md
if node tools/param-pages/widget_sheet.mjs --check >/dev/null 2>&1; then
  echo "FAIL: --check passed on a corrupted docs/WIDGETS.md — it is not comparing anything" >&2
  exit 1
fi
restore
trap - EXIT

echo "  ok  --check detects a stale sheet"
echo "PASS: docs/WIDGETS.md matches the widgets it documents"
