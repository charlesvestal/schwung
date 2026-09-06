#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# src/shared/qr.mjs -- the QR encoder behind the Connect page.
#
# WHAT A TEST HERE CAN AND CANNOT SEE. A QR symbol has three independent ways to
# be wrong and only one of them is visible: a misplaced function pattern looks
# wrong, a wrong data walk looks like noise (and so does a correct one), and
# WRONG ERROR-CORRECTION CODEWORDS LOOK PERFECT. The last one is not
# hypothetical -- the first cut of this file's subject had its Reed-Solomon
# generator polynomial built with its two terms swapped, and produced symbols
# with every finder, every timing module, every data module and every format bit
# correct, that no reader on earth would decode.
#
# So the real oracle is external and lives in tools/qr/verify_qr.py: it compares
# against segno module-for-module AND renders the symbol and decodes it with
# OpenCV. That script is not run from here (it needs a python venv with two
# large wheels to check twenty-five bytes of URL), and this file is the pin that
# keeps its verdict from silently expiring:
#
#   - the two golden symbols below were produced by the encoder at the commit
#     where verify_qr.py passed, so any change to placement, masking, padding or
#     error correction moves them;
#   - the error-correction vector is an INDEPENDENT one, taken from segno's own
#     output rather than from ours, so it fails on exactly the bug above even if
#     the goldens were regenerated carelessly;
#   - the format bits are read back OUT of the finished matrix and checked to
#     name the mask the encoder says it chose, which no golden can do.
#
# Regenerate the goldens only after running tools/qr/verify_qr.py green.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const Q = await import(R + "/src/shared/qr.mjs");

/* ---- 1. golden symbols -------------------------------------------------- */

const GOLDEN = {
  "http://192.168.1.42:7700": { version: 2, mask: 5, rows: [
    "1111111001110100101111111","1000001000011010001000001","1011101001110000101011101",
    "1011101010110111101011101","1011101010101010001011101","1000001001100100101000001",
    "1111111010101010101111111","0000000001001000000000000","1100011101101101100011000",
    "0000010100100101110011110","0111011011011101001101011","0001000010110001100011001",
    "0110001011100010111000001","1011000010101011000000010","1010001010000111010101011",
    "1010100101101000100010101","1001001101001100111110100","0000000010100011100010100",
    "1111111010001000101011001","1000001011010010100010001","1011101000111101111111100",
    "1011101001101000001101011","1011101001001000010000101","1000001011101001101110001",
    "1111111011010011101001001"] },
  "A": { version: 1, mask: 0, rows: [
    "111111100101101111111","100000100111001000001","101110101101101011101",
    "101110100101001011101","101110100010101011101","100000100000101000001",
    "111111101010101111111","000000001101100000000","111011111111011000100",
    "101100001000001000110","010111100110100010001","010110001100001000100",
    "001101101000101010101","000000001001010101010","111111101011011101111",
    "100000101111110111000","101110101101011101101","101110100110001000110",
    "101110101100100010001","100000101000001000110","111111101110101010111"] },
};

for (const [text, want] of Object.entries(GOLDEN)) {
  const got = Q.encode(text);
  const rows = got.modules.map((r) => Array.from(r).join(""));
  if (got.version !== want.version) fail(JSON.stringify(text) + ": version " + got.version + ", want " + want.version);
  if (got.mask !== want.mask) fail(JSON.stringify(text) + ": mask " + got.mask + ", want " + want.mask);
  if (got.size !== want.rows.length) fail(JSON.stringify(text) + ": size " + got.size);
  else {
    let bad = -1;
    for (let y = 0; y < want.rows.length; y++) if (rows[y] !== want.rows[y]) { bad = y; break; }
    if (bad >= 0) {
      fail(JSON.stringify(text) + ": row " + bad + " differs from the golden symbol\n" +
           "        want " + want.rows[bad] + "\n        got  " + rows[bad] +
           "\n        (re-run tools/qr/verify_qr.py before regenerating these)");
    }
  }
}

/* ---- 2. error correction, against a vector we did not produce ------------
 *
 * Data codewords and the ten error-correction codewords segno emits for them at
 * version 2, level L. This is the ONE assertion here that a regenerated golden
 * cannot launder: a wrong generator polynomial reproduces itself into any
 * golden taken from the same build, and fails this outright.
 */
{
  const data = "41 46 87 47 47 03 a2 f2 f3 13 02 e3 02 e3 02 e3 23 a3 73 73 03 00 00 ec 11 ec 11 ec 11 ec 11 ec 11 ec"
      .split(" ").map((h) => parseInt(h, 16));
  const want = "96 f8 4e d8 c8 d0 76 56 62 e2";
  const got = Q.ecCodewords(data, 10).map((b) => b.toString(16).padStart(2, "0")).join(" ");
  if (got !== want) fail("Reed-Solomon: want [" + want + "], got [" + got + "]");
}

/* ---- 3. version selection ----------------------------------------------- */
{
  const BOUNDS = [[1, 17], [2, 32], [3, 53], [4, 78]];
  for (const [v, cap] of BOUNDS) {
    if (Q.chooseVersion(cap) !== v) fail(cap + " bytes should choose version " + v + ", got " + Q.chooseVersion(cap));
    if (Q.chooseVersion(cap + 1) === v) fail((cap + 1) + " bytes must not still fit version " + v);
  }
  if (Q.chooseVersion(79) !== 0) fail("79 bytes must not choose any version, got " + Q.chooseVersion(79));
}

/* ---- 4. refusals --------------------------------------------------------
 *
 * Each of these is a case where emitting SOMETHING would be worse than
 * emitting nothing: a truncated URL still scans, and it goes somewhere else.
 */
{
  const throws = (fn, what) => {
    try { fn(); fail(what + " must throw"); } catch (e) { /* expected */ }
  };
  throws(() => Q.encode(""), "an empty string");
  throws(() => Q.encode("x".repeat(79)), "79 bytes (past version 4)");
  throws(() => Q.encode("caféあ"), "non-ASCII input");
}

/* ---- 5. structure, read back out of the finished symbol ------------------
 *
 * A golden pins the whole picture but explains nothing; these name the
 * properties a reader depends on, so a break says WHICH one moved.
 */
for (const text of ["http://192.168.1.42:7700", "A"]) {
  const { size, modules, mask } = Q.encode(text);
  const at = (y, x) => modules[y][x];

  /* Finder patterns in three corners, and none in the fourth. */
  const finderAt = (oy, ox) => {
    for (let dy = 0; dy < 7; dy++) for (let dx = 0; dx < 7; dx++) {
      const ring = (dx === 0 || dx === 6 || dy === 0 || dy === 6);
      const core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
      if (at(oy + dy, ox + dx) !== ((ring || core) ? 1 : 0)) return false;
    }
    return true;
  };
  if (!finderAt(0, 0)) fail(text + ": no finder at top-left");
  if (!finderAt(0, size - 7)) fail(text + ": no finder at top-right");
  if (!finderAt(size - 7, 0)) fail(text + ": no finder at bottom-left");

  /* Timing patterns alternate, starting dark at the even coordinate. */
  for (let i = 8; i < size - 8; i++) {
    if (at(6, i) !== (i % 2 === 0 ? 1 : 0)) { fail(text + ": horizontal timing wrong at " + i); break; }
    if (at(i, 6) !== (i % 2 === 0 ? 1 : 0)) { fail(text + ": vertical timing wrong at " + i); break; }
  }

  /* The dark module is always set and always here. */
  if (at(size - 8, 8) !== 1) fail(text + ": the dark module at (" + (size - 8) + ",8) is light");

  /*
   * FORMAT BITS, DECODED. Read the fifteen modules of copy 1 back, undo the
   * 0x5412 mask, and check the five payload bits say "level L, mask <n>" for
   * the n the encoder reported. A golden cannot catch a format bug that is
   * consistent with itself; this can, and the placement of these fifteen
   * modules is transposable in a way that leaves the picture looking right.
   */
  let bits = 0;
  const put = (i, v) => { bits |= (v << i); };
  for (let i = 0; i <= 5; i++) put(i, at(i, 8));
  put(6, at(7, 8));
  put(7, at(8, 8));
  put(8, at(8, 7));
  for (let i = 9; i <= 14; i++) put(i, at(8, 14 - i));
  const payload = ((bits ^ 0x5412) >> 10) & 0x1f;
  const level = (payload >> 3) & 3;
  const maskBits = payload & 7;
  if (level !== 0b01) fail(text + ": format bits say EC level " + level + ", want 0b01 (L)");
  if (maskBits !== mask) fail(text + ": format bits say mask " + maskBits + ", encoder reported " + mask);

  /* And copy 2 must agree with copy 1 -- a reader uses whichever it can see. */
  let bits2 = 0;
  const put2 = (i, v) => { bits2 |= (v << i); };
  for (let i = 0; i <= 7; i++) put2(i, at(8, size - 1 - i));
  for (let i = 8; i <= 14; i++) put2(i, at(size - 15 + i, 8));
  if (bits2 !== bits) fail(text + ": the two format-information copies disagree");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: qr encoder — two golden symbols, an independent Reed-Solomon vector, " +
            "version bounds, three refusals, and the format bits decoded back out");
'
