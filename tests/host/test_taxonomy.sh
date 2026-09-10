#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE TAXONOMY VOCABULARY, and the two ways a category system rots.
#
#   - A SUBCATEGORY THAT IS NOT IN THE VOCABULARY produces a filter chip of one
#     module, or no chip at all and a module reachable from nothing. Neither
#     looks like an error from the catalog file, so it is caught here.
#
#   - A DERIVED TAG WRITTEN BY HAND is a fact that can go stale against the
#     thing it describes. `needs-assets` restates `requires`; if a module drops
#     its ROM dependency and clears `requires`, a hand-written tag keeps
#     filtering it out of a list it now belongs in. Both directions fail.
#
# Every rule below is also MUTATED to prove it can fail. A rule asserted only
# against clean data is a rule that passes because nothing exercised it.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { loadJson, validate, derivedTags, embedBlock } from "./tools/catalog/taxonomy.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const taxonomy = loadJson("taxonomy.json");
const catalog  = loadJson("module-catalog.json");

// 1. The real catalog validates clean.
const errs = validate(taxonomy, catalog);
if (errs.length) fail("real catalog does not validate:\n  " + errs.join("\n  "));

// A deep copy per mutation, so one mutation cannot leak into the next.
const clone = () => JSON.parse(JSON.stringify(catalog));
const expectError = (what, mutated, needle) => {
  const e = validate(taxonomy, mutated);
  if (!e.some((s) => s.includes(needle)))
    fail(what + ": expected an error containing \"" + needle + "\", got [" + e.join(" | ") + "]");
};

// 2. A subcategory outside the vocabulary fails.
{
  const c = clone();
  c.modules[0].subcategory = "not-a-real-subcategory";
  expectError("unknown subcategory", c, "is not one of");
}

// 3. A subcategory belonging to a DIFFERENT component_type fails. This is the
//    one a human gets wrong, because the value looks entirely plausible.
{
  const c = clone();
  const sg = c.modules.find((m) => m.component_type === "sound_generator");
  sg.subcategory = "reverb";
  expectError("cross-type subcategory", c, "is not one of");
}

// 4. An unknown tag fails.
{
  const c = clone();
  c.modules[0].tags = ["definitely-not-a-tag"];
  expectError("unknown tag", c, "unknown tag");
}

// 5. Unsorted tags fail (the artifact must be deterministic).
{
  const c = clone();
  c.modules[0].requires = undefined;
  delete c.modules[0].requires;
  c.modules[0].tags = ["vocal", "bass"];
  expectError("unsorted tags", c, "not sorted");
}

// 6. A module with `requires` and no needs-assets tag fails.
{
  const c = clone();
  const m = c.modules.find((x) => typeof x.requires === "string" && x.requires.trim() !== "");
  if (!m) fail("fixture: no catalog module has a requires field");
  else { m.tags = []; expectError("missing derived tag", c, "missing derived tag"); }
}

// 7. needs-assets written by hand on a module with no `requires` fails.
{
  const c = clone();
  const m = c.modules.find((x) => !x.requires || String(x.requires).trim() === "");
  if (!m) fail("fixture: every catalog module has a requires field");
  else { m.tags = ["needs-assets"]; expectError("hand-written derived tag", c, "written by hand"); }
}

// 8. A stale embedded block fails.
{
  const c = clone();
  c.taxonomy = { taxonomy_version: 0, subcategories: {}, tags: [] };
  expectError("stale embed", c, "out of date");
}

// 9. derivedTags is exactly the restatement of `requires`, both ways.
if (JSON.stringify(derivedTags({ requires: "a ROM" })) !== JSON.stringify(["needs-assets"]))
  fail("derivedTags: a non-empty requires must derive needs-assets");
if (derivedTags({ requires: "   " }).length !== 0)
  fail("derivedTags: whitespace-only requires must derive nothing");
if (derivedTags({}).length !== 0)
  fail("derivedTags: absent requires must derive nothing");

// 10. embedBlock carries the whole vocabulary, not a summary of it.
{
  const b = embedBlock(taxonomy);
  const types = Object.keys(b.subcategories || {});
  if (types.length !== Object.keys(taxonomy.subcategories).length)
    fail("embedBlock dropped a component_type");
  if (!Array.isArray(b.tags) || b.tags.length !== taxonomy.tags.length)
    fail("embedBlock dropped tags");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: taxonomy");
'
