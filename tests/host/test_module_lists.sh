#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The module-lists model, unit-tested with no device and no globals.
#
# Everything that can be a rule lives here rather than in shadow_ui.js, for
# one reason: this file can be imported by node and that one cannot. A rule
# that is only reachable through a 21k-line UI file is a rule with no test.
#
# Two of these assertions are load-bearing beyond their own line:
#
#  - a CORRUPT file must be reported as corrupt, not silently replaced by the
#    seeded default. The caller declines to write over a file it could not
#    read, because a future version might read it. Collapsing "could not read"
#    into "was empty" is the same tri-state mistake the param channel made.
#  - filterIds answers NULL for a list that does not exist, never the identity.
#    The identity would silently show every module under a filter name that no
#    longer means anything, which reads as the filter being broken.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
import("./src/shared/module_lists.mjs").then((M) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };
  const ok = (m) => { console.error("ok: " + m); };
  const eq = (a, b, m) => { JSON.stringify(a) === JSON.stringify(b) ? ok(m) : fail(m + " -- got " + JSON.stringify(a) + " want " + JSON.stringify(b)); };

  const io = (raw) => ({ readFile: () => raw, writeFile: () => true });

  /* ---- 1. seeding ------------------------------------------------------ */
  let r = M.loadLists(io(null));
  eq(r.state.lists.map(l => l.name), ["Favorites"], "missing file seeds Favorites only");
  eq(r.corrupt, false, "missing file is not corrupt");
  eq(r.state.lists[0].modules, [], "seeded Favorites is empty");

  r = M.loadLists(io("{not json"));
  eq(r.state.lists.map(l => l.name), ["Favorites"], "corrupt file still yields a usable state");
  eq(r.corrupt, true, "corrupt file is REPORTED corrupt, so the caller can decline to overwrite it");

  r = M.loadLists(io(JSON.stringify({ version: 1, lists: [ { name: "Live", modules: ["dx7"] } ] })));
  eq(r.state.lists.map(l => l.name), ["Favorites", "Live"], "a file without Favorites gets it inserted at 0");

  r = M.loadLists(io(JSON.stringify({ version: 1, lists: [ { name: "Live", modules: [] }, { name: "Favorites", modules: ["braids"] } ] })));
  eq(r.state.lists.map(l => l.name), ["Favorites", "Live"], "Favorites is moved to index 0");
  eq(r.state.lists[0].modules, ["braids"], "moving Favorites keeps its members");

  /* ---- 2. create ------------------------------------------------------- */
  let s = M.emptyState();
  eq(M.createList(s, "").ok, false, "createList rejects an empty name");
  eq(M.createList(s, "   ").ok, false, "createList rejects whitespace only");
  eq(M.createList(s, "Live").ok, true, "createList accepts a fresh name");
  eq(M.createList(s, "live").ok, false, "createList rejects a case-insensitive duplicate");
  eq(M.createList(s, "FAVORITES").ok, false, "createList cannot shadow Favorites");
  eq(s.lists.map(l => l.name), ["Favorites", "Live"], "create appends in order");

  /* ---- 3. rename / delete / clear -------------------------------------- */
  eq(M.renameList(s, "Favorites", "Faves").ok, false, "Favorites cannot be renamed");
  eq(M.deleteList(s, "Favorites").ok, false, "Favorites cannot be deleted");
  eq(M.renameList(s, "Live", "LIVE").ok, true, "renaming a list to its own name in another case is not a collision");
  eq(s.lists[1].name, "LIVE", "the rename took");
  eq(M.renameList(s, "LIVE", "").ok, false, "rename rejects an empty name");
  eq(M.renameList(s, "Nope", "X").ok, false, "rename rejects an unknown list");

  M.toggleMembership(s, "Favorites", "braids");
  eq(M.clearList(s, "Favorites").ok, true, "Favorites CAN be cleared");
  eq(s.lists[0].modules, [], "clear empties the members");
  eq(M.deleteList(s, "LIVE").ok, true, "an ordinary list deletes");
  eq(s.lists.map(l => l.name), ["Favorites"], "delete removes it");

  /* ---- 4. membership --------------------------------------------------- */
  s = M.emptyState();
  M.createList(s, "Live");
  eq(M.toggleMembership(s, "Favorites", "braids"), true, "toggle on returns true");
  eq(M.isMember(s, "Favorites", "braids"), true, "isMember sees it");
  eq(M.toggleMembership(s, "Favorites", "braids"), false, "toggle off returns false");
  eq(M.isMember(s, "Favorites", "braids"), false, "isMember agrees");
  M.toggleMembership(s, "Favorites", "braids");
  M.toggleMembership(s, "Live", "braids");
  eq(M.listsContaining(s, "braids"), ["Favorites", "Live"], "listsContaining reports both, in list order");
  eq(M.listsContaining(s, "nope"), [], "listsContaining is empty for a stranger");

  /* ---- 5. filtering ---------------------------------------------------- */
  s = M.emptyState();
  M.createList(s, "FX");
  M.toggleMembership(s, "Favorites", "braids");
  M.toggleMembership(s, "Favorites", "cloudseed");
  M.toggleMembership(s, "FX", "cloudseed");
  const synths = ["braids", "hera", "dx7"];
  eq(M.filterIds(s, synths, null), synths, "a null filter is the identity");
  eq(M.filterIds(s, synths, "Favorites"), ["braids"], "filterIds intersects and keeps input order");
  eq(M.filterIds(s, synths, "Gone"), null, "filterIds answers NULL for a list that does not exist");
  eq(M.listsWithAnyOf(s, synths), ["Favorites"], "a list with no synth member is not offered to a synth picker");
  eq(M.listsWithAnyOf(s, ["cloudseed"]), ["Favorites", "FX"], "both lists are offered where both have a member");
  eq(M.listsWithAnyOf(s, ["zzz"]), [], "nothing is offered when nothing matches");

  /* ---- 6. cycle order -------------------------------------------------- */
  const elig = ["Favorites", "FX"];
  eq(M.nextFilter(null, elig), "Favorites", "All goes to the first eligible list");
  eq(M.nextFilter("Favorites", elig), "FX", "then to the next");
  eq(M.nextFilter("FX", elig), null, "then wraps to All");
  eq(M.nextFilter("Gone", elig), null, "a filter no longer eligible falls back to All");
  eq(M.nextFilter(null, []), null, "with nothing eligible the cycle stays on All");

  if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
  console.log("PASS");
}).catch((e) => { console.error("FAIL: " + e); process.exit(1); });
'
