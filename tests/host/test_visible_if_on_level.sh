#!/usr/bin/env bash
# `visible_if` in chain_params, and not on the level entry, hides NOTHING.
#
# The planner reads it off the level's own params (page_plan.mjs isHiddenParam)
# and nowhere else. Declaring it beside a param's other metadata -- where every
# other field lives -- is the natural mistake, and it fails silently: every
# gated cell is drawn, no error, nothing logged. A microQ editor shipped that
# way with eight effects' controls on screen at once.
set -euo pipefail
cd "$(dirname "$0")/../.."

out=$(node --input-type=module -e '
import { validateContract } from "./src/shared/param_pages/validate_contract.mjs";
const gate = { key: "b", equals: 1 };
const cp = [{ key: "a", name: "A", type: "int", min: 0, max: 9, visible_if: gate },
            { key: "b", name: "B", type: "int", min: 0, max: 9 }];
const ids = (f) => (Array.isArray(f) ? f : (f.findings || [])).map((x) => x.id || x.rule)
                     .filter((i) => /visible-if-not-on-level/.test(i));

const orphaned = validateContract({ id: "t",
  hierarchy: { levels: { root: { knobs: ["a", "b"], params: [{ key: "a" }, { key: "b" }] } } },
  chainParams: cp });
const mirrored = validateContract({ id: "t",
  hierarchy: { levels: { root: { knobs: ["a", "b"],
                                 params: [{ key: "a", visible_if: gate }, { key: "b" }] } } },
  chainParams: cp });

if (ids(orphaned).length !== 1) { console.log("FAIL: an orphaned visible_if was not reported"); process.exit(1); }
if (ids(mirrored).length !== 0) { console.log("FAIL: a mirrored visible_if was reported anyway"); process.exit(1); }
console.log("PASS: a visible_if that only exists in chain_params is reported");
')
echo "$out"
case "$out" in *PASS*) exit 0 ;; *) exit 1 ;; esac
