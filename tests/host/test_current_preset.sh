#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The `*` on the User Presets page: does the live state still match the preset
# that was loaded? The state blob carries no name, so the association is
# bookkeeping we keep ourselves — and it must never guess. A read that FAILED
# is not a diff.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const CP = await import(R + "/src/shared/param_pages/current_preset.mjs");

/* ---- hashState --------------------------------------------------------- */
if (CP.hashState("abc") !== CP.hashState("abc")) fail("hashState must be stable");
if (CP.hashState("abc") === CP.hashState("abd")) fail("hashState must separate blobs");
if (CP.hashState("") === CP.hashState(null)) {
  fail("empty string (declares no state) and null (the read FAILED) are different answers");
}
if (CP.hashState(null) !== null) fail("a failed read hashes to null, not a value");
if (CP.hashState("") === null) fail("an empty declared state must still hash to a real value");

/* Weak-hash guards: differ only at the end, differ only in length. */
const longA = "x".repeat(500) + "A";
const longB = "x".repeat(500) + "B";
if (CP.hashState(longA) === CP.hashState(longB)) {
  fail("hashState must separate blobs differing only in the last character");
}
const short = "same";
const padded = short + "x";
if (CP.hashState(short) === CP.hashState(padded)) {
  fail("hashState must separate blobs of different length (no zero-suffix collision)");
}
/* Two different-length blobs whose naive checksum would collide without the
 * length suffix: "AB" (65+66=131) vs "A" + chr(131-65) = "A\x42" is not
 * distinct content-wise, so instead just confirm length is mixed in for a
 * same-content-different-length pair. */
if (CP.hashState("aa") === CP.hashState("aaa")) {
  fail("hashState must separate blobs of different repeated-content length");
}

/* ---- makeRecord / isModified ------------------------------------------- */
const rec = CP.makeRecord("Fat Brass", "BLOB");
if (rec.name !== "Fat Brass") fail("record keeps the name");
if (CP.isModified(rec, "BLOB")) fail("same blob is not modified");
if (!CP.isModified(rec, "OTHER")) fail("different blob is modified");

/* A record with no hash cannot answer, so it must not claim a diff. */
const blind = { name: "Fat Brass", hash: null };
if (CP.isModified(blind, "ANYTHING")) fail("an unknown hash must not report modified");
/* Neither can a live read that failed. */
if (CP.isModified(rec, null)) fail("a failed live read must not report modified");
if (CP.isModified(null, "BLOB")) fail("no loaded preset is never modified");

/* makeRecord with a null/empty name should still be usable but never render
 * as a real preset name. */
const noName = CP.makeRecord(null, "BLOB");
if (noName.name !== "") fail("makeRecord with a null name should normalize to empty string");
const emptyName = CP.makeRecord("", "BLOB");
if (CP.presetRowValue(emptyName, "BLOB") !== "(none)") {
  fail("a record with an empty name should render as (none), not a bare star");
}

/* ---- presetRowValue ---------------------------------------------------- */
if (CP.presetRowValue(null, "BLOB") !== "(none)") fail("no preset reads (none)");
if (CP.presetRowValue(rec, "BLOB") !== "Fat Brass") fail("unmodified reads the bare name");
if (CP.presetRowValue(rec, "OTHER") !== "Fat Brass *") fail("modified appends a space-star");
if (CP.presetRowValue(blind, "ANYTHING") !== "Fat Brass") {
  fail("a record with a null hash must render the bare name, never a star");
}

/* ---- purity ------------------------------------------------------------ */
const src = await (await import("node:fs/promises")).readFile(
  R + "/src/shared/param_pages/current_preset.mjs", "utf8");
if (/shadow_ui_ctx|host_[a-z_]+\(|getSlotParam/.test(src)) {
  fail("current_preset.mjs must stay pure — no ctx, no host calls");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
