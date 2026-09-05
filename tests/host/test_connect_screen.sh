#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# src/shared/connect_screen.mjs, rendered into the real 128x64 framebuffer.
#
# Every number this screen depends on is a MEASUREMENT, and measuring it is the
# whole content of this file. The 5x7 atlas is proportional, so "fifteen
# characters" is not a width; the QR plate is 58px against a 64px panel, so
# "there is room" is not a fact; and the footer is drawn by a different module
# that has never heard of this one, so "they do not collide" is an assumption
# until something checks it.
#
# The failure being guarded against is specific and silent: a line printed
# wider than the panel is CLIPPED BY THE FRAMEBUFFER, and what survives is a
# shorter address that still looks like an address. On the one screen whose
# entire purpose is telling the user a number they do not know, a plausible
# wrong number is the worst thing that can happen -- worse than a blank.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const H = await import(R + "/tools/param-pages/harness.mjs");
const C = await import(R + "/src/shared/connect_screen.mjs");
const QR = await import(R + "/src/shared/qr.mjs");
const GEO = await import(R + "/src/shared/list_geometry.mjs");
const MOVY = await import(R + "/src/shared/param_pages/render_page_movy.mjs");

const PANEL = { x: 0, y: 0, w: 128, h: 64 };
const IPS = ["192.168.1.42", "10.0.0.2", "172.16.254.199", "255.255.255.255", "8.8.8.8"];

/* ---- 1. the URL, and the one place the port is spelled ------------------- */
{
  if (C.managerUrl("192.168.1.42") !== "http://192.168.1.42:7700") {
    fail("managerUrl: got " + C.managerUrl("192.168.1.42"));
  }
  /* An address the device does not have must not become a URL. Every caller
   * branches on the empty string to draw a sentence instead. */
  for (const empty of ["", "   ", null, undefined]) {
    if (C.managerUrl(empty) !== "") fail("managerUrl(" + JSON.stringify(empty) + ") must be empty");
  }
  if (C.MANAGER_PORT !== 7700) fail("MANAGER_PORT moved: " + C.MANAGER_PORT);
}

/* ---- 2. FOOTER_TOP is list_geometry.RULE_Y ------------------------------
 *
 * connect_screen keeps the number locally so it stays free of the UI layout
 * graph and testable on its own. That is only safe while something asserts the
 * two are the same: a footer band that moved down would leave this screen
 * centring its text into a gap, and one that moved up would put the address
 * under the hint pills.
 */
if (C.FOOTER_TOP !== GEO.RULE_Y) {
  fail("connect_screen.FOOTER_TOP is " + C.FOOTER_TOP + " but list_geometry.RULE_Y is " + GEO.RULE_Y);
}

/* ---- 3. hostLines: split only when it must, and always at a dot ---------- */
{
  const fb = H.createFramebuffer();
  const col = (w) => (t) => fb.textWidth(t) <= w;

  /* A wide column takes anything whole. */
  for (const ip of IPS) {
    const one = C.hostLines(ip, col(128));
    if (one.length !== 1 || one[0] !== ip) fail("hostLines(" + ip + ", wide) = " + JSON.stringify(one));
  }

  /* The real column, derived the way the drawing derives it. */
  const symbol = QR.encode(C.managerUrl("192.168.1.42"));
  const colW = 128 - (2 + C.plateSize(symbol) + 3) - 1;
  for (const ip of IPS) {
    const lines = C.hostLines(ip, col(colW));
    if (lines.length > 2) fail(ip + " split into " + lines.length + " lines; two is the cap");
    if (lines.join("") !== ip) fail(ip + " did not survive the split: " + JSON.stringify(lines));
    for (const l of lines) {
      if (fb.textWidth(l) > colW) {
        fail(ip + ": line " + JSON.stringify(l) + " is " + fb.textWidth(l) + "px in a " + colW + "px column");
      }
    }
    if (lines.length === 2 && !lines[0].endsWith(".")) {
      /* The trailing dot stays on the first line so two halves of one address
       * cannot read as two addresses. */
      fail(ip + ": the split dropped the dot: " + JSON.stringify(lines));
    }
  }
}

/* ---- 4. the drawn symbol IS the encoded symbol --------------------------
 *
 * drawQr merges horizontal runs into one fillRect each, which is a real
 * transformation of the drawing and not just a speed-up: an off-by-one in the
 * run boundary yields a symbol that still looks like a QR. So the pixels are
 * read back and compared to the encoder module for module, and the quiet zone
 * is checked to be lit -- a missing quiet zone is invisible on screen and is
 * the difference between scanning and not.
 */
{
  const ip = "192.168.1.42";
  const fb = H.createFramebuffer();
  C.drawConnectBody(fb, PANEL, { ip });
  const symbol = QR.encode(C.managerUrl(ip));
  const plate = C.plateSize(symbol);
  const px = fb.pixels;
  const lit = (x, y) => px[y * 128 + x];

  const qx = 2;
  const qy = Math.floor((64 - plate) / 2);
  const ox = qx + C.QR_QUIET * C.QR_SCALE;
  const oy = qy + C.QR_QUIET * C.QR_SCALE;

  let wrong = 0, firstWrong = null;
  for (let my = 0; my < symbol.size; my++) {
    for (let mx = 0; mx < symbol.size; mx++) {
      /* Dark module -> unlit pixel. The symbol is drawn in NEGATIVE: a lit
       * plate with the modules punched out of it, so a phone sees the polarity
       * every reader expects instead of a white-on-black inversion. */
      const want = symbol.modules[my][mx] ? 0 : 1;
      for (let dy = 0; dy < C.QR_SCALE; dy++) {
        for (let dx = 0; dx < C.QR_SCALE; dx++) {
          if (lit(ox + mx * C.QR_SCALE + dx, oy + my * C.QR_SCALE + dy) !== want) {
            wrong++;
            if (!firstWrong) firstWrong = mx + "," + my;
          }
        }
      }
    }
  }
  if (wrong) fail("the drawn symbol differs from the encoded one at " + wrong + " pixels (first module " + firstWrong + ")");

  let quietDark = 0;
  for (let y = qy; y < qy + plate; y++) {
    for (let x = qx; x < qx + plate; x++) {
      const insideX = x >= ox && x < ox + symbol.size * C.QR_SCALE;
      const insideY = y >= oy && y < oy + symbol.size * C.QR_SCALE;
      if (insideX && insideY) continue;
      if (!lit(x, y)) quietDark++;
    }
  }
  if (quietDark) fail("the quiet zone has " + quietDark + " unlit pixels; it must be solid");
  if (C.QR_QUIET < 1) fail("QR_QUIET is " + C.QR_QUIET + "; a symbol with no quiet zone does not scan");
}

/* ---- 5. nothing is clipped, for any address ----------------------------- */
for (const ip of IPS.concat([""])) {
  const fb = H.createFramebuffer();
  C.drawConnectBody(fb, PANEL, { ip });
  if (fb.clipped() !== 0) {
    fail("ip " + JSON.stringify(ip) + ": " + fb.clipped() + " pixels drawn off the panel " +
         "(a clipped address is a WRONG address that still looks like one)");
  }
}

/* ---- 6. the body and the hint footer do not meet ------------------------
 *
 * Two modules that have never heard of each other, drawing into the same
 * panel. The plate is on the left and the pills are laid out from the right,
 * and that is the whole reason this screen can keep a footer at all -- so it
 * is measured rather than assumed.
 */
{
  const bodyFb = H.createFramebuffer();
  C.drawConnectBody(bodyFb, PANEL, { ip: "255.255.255.255" });
  const footFb = H.createFramebuffer();
  MOVY.drawFooter(footFb, [["BACK", "Settings"]]);

  const at = (fb, x, y) => fb.pixels[y * 128 + x];
  let overlap = 0;
  for (let y = C.FOOTER_TOP; y < 64; y++) {
    for (let x = 0; x < 128; x++) if (at(bodyFb, x, y) && at(footFb, x, y)) overlap++;
  }
  if (overlap) fail(overlap + " pixels are painted by BOTH the Connect body and the hint footer");
}

/* ---- 7. no address draws WORDS, and draws no symbol ---------------------- */
{
  const fb = H.createFramebuffer();
  C.drawConnectBody(fb, PANEL, { ip: "" });
  const on = fb.countLit();
  if (on < 100) fail("the no-network screen drew " + on + " lit pixels; a near-blank panel reads as a crash");

  /* A solid 58x58 plate would be ~3400 lit pixels. Text is far fewer, and the
   * point is that no QR is drawn for a URL we do not have. */
  if (on > 1500) fail("the no-network screen drew " + on + " lit pixels; it must not draw a symbol");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: connect screen — url and port, FOOTER_TOP pinned to RULE_Y, address splitting " +
            "measured against the real font, the drawn symbol compared to the encoder module for " +
            "module with a solid quiet zone, nothing clipped, and no overlap with the hint footer");
'
