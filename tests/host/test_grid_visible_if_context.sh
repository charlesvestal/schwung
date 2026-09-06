#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# visible_if on the knob grid read the WRONG slot and failed open.
#
# evaluateVisibilityCondition resolved its condition against hierEditorSlot /
# hierEditorComponent -- the LIST editor's identity, which enterParamPages
# never sets. From the grid that is slot -1: the read answers null, the
# evaluator fails open, and every visible_if condition is true. A send level
# meant to collapse to the armed type's cells showed all twenty, three pages
# deep, with nothing logged. Reported from the device.
#
# Pinned: on PARAM_PAGES the evaluator takes the grid's own slot/component,
# resolving a per-instance key through the grid's child index by level NAME.

fail() { echo "FAIL: $*" >&2; exit 1; }
ui="src/shadow/shadow_ui.js"
pp="src/shadow/shadow_ui_param_pages.mjs"

body=$(sed -n '/^function evaluateVisibilityCondition(condition, levelDef) {/,/^}/p' "$ui")
[ -n "$body" ] || fail "evaluateVisibilityCondition is gone"
echo "$body" | command grep -q 'paramPagesActive()' || fail "evaluateVisibilityCondition never asks whether the grid is up -- it reads the list editor's slot from the grid and fails open"
echo "$body" | command grep -q 'paramPagesSlot()' || fail "on the grid the evaluator does not use the grid's slot"
echo "$body" | command grep -q 'paramPagesComponent()' || fail "on the grid the evaluator does not use the grid's component"
echo "$body" | command grep -q 'paramPagesLevelNameOf(levelDef)' || fail "a per-instance condition cannot find its instance: the level name is not resolved from its definition"
command grep -q '^export function paramPagesLevelNameOf' "$pp" || fail "paramPagesLevelNameOf is not exported by $pp"
command grep -q 'paramPagesLevelNameOf,' "$ui" || fail "shadow_ui.js does not import paramPagesLevelNameOf"
# A re-plan follows every detent of a gating knob; a blocking read per condition froze the OLED.
echo "$body" | command grep -q 'paramPagesCachedValue(k)' || fail "the grid evaluator does not consult the controller's own values first -- every re-plan pays a blocking read per condition"
echo "$body" | command grep -q 'getSlotParamCached(' || fail "a cache miss on the grid goes to an uncached blocking read"
if echo "$body" | command grep -q '[^a-zA-Z]getSlotParam(' ; then fail "the grid branch still reads through the uncached getSlotParam"; fi
command grep -q '^export function paramPagesCachedValue' "$pp" || fail "paramPagesCachedValue is not exported by $pp"
echo "  ok  visible_if on the knob grid resolves against the grid's own slot and component"
echo "PASS: test_grid_visible_if_context"
