#!/usr/bin/env bash
# "IS MOVE TRANSMITTING?" -- the signal, and the fix it did NOT buy.
#
# The E16 must enumerate as a SINGLE USB-MIDI jack for Move's XMOS to carry
# SysEx to it at all -- 2-jack and 3-jack images were both measured on hardware
# 2026-09-11 and failed on every one of cables 0-13. So there is exactly one
# stream, and our screen SysEx shares it with Move's own notes, aftertouch and
# clock. A 34-packet LABELS message spans ~5 SPI frames, and anything Move
# emits inside that window is spliced into it, which a conformant receiver must
# discard.
#
# The surface's 1.5 s self-heal restate is PURE REPAIR -- it carries no new
# information and exists only to fix a corruption we cannot detect. While Move
# is transmitting it is also the message most likely to BE corrupted, and at
# idle it is the ONLY traffic there is. Repeating it then does not repair
# anything; it manufactures a broken screen out of one that was sitting there
# correct.
#
# So it is suppressed while Move is busy. What must NOT be suppressed is a real
# CHANGE: a page turn while the transport runs has to arrive, or the panel
# shows labels belonging to a different page while the encoders drive this one.
# A stale screen that looks correct is a worse failure than a garbled one that
# obviously is not -- which is the property this file exists to pin, because
# it is the one a future "just don't send while busy" simplification breaks.
set -euo pipefail
cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { createDisplay, SCREEN_HEARTBEAT_MS, FOREIGN_QUIET_MS }
    from "./src/shared/e16_surface.mjs";

let fails = 0;
const ok  = (n) => console.log("ok   " + n);
const bad = (n, why) => { console.log("FAIL " + n + "\n  " + why); fails++; };
const check = (n, cond, why) => cond ? ok(n) : bad(n, why || "");

/* ------------------------------------------------------------------ *
 * The gate, lifted out of createSurface so it can be driven directly.
 * Kept byte-for-byte in step with the real one by the source pin below.
 * ------------------------------------------------------------------ */
function makeGate(foreignOf) {
    let seen = -1, at = -Infinity;
    return function busy(t) {
        let n = 0;
        try { n = foreignOf() | 0; } catch (e) { return false; }
        if (seen < 0) { seen = n; return false; }
        if (n !== seen) { seen = n; at = t; }
        return (t - at) < FOREIGN_QUIET_MS;
    };
}

/* --- the counter is FREE-RUNNING: only the delta may matter ---------- */
{
    let n = 1000000;                      /* large, never reset */
    const busy = makeGate(() => n);
    check("a large but STATIC counter is not busy", busy(0) === false,
          "a counter that has stopped rising IS the quiet we wait for; " +
          "reading its value instead of its delta makes a long session " +
          "permanently busy and the screen never repairs again");
    n += 3;
    check("a RISE marks busy", busy(10) === true);
}

/* --- first read must not latch busy --------------------------------- */
{
    const busy = makeGate(() => 77);
    check("the first read is never busy", busy(0) === false,
          "there is no previous sample to diff against, so treating the " +
          "first read as a change would park the heartbeat for FOREIGN_QUIET_MS " +
          "every time the surface starts");
}

/* --- quiet returns after the window --------------------------------- */
{
    let n = 0;
    const busy = makeGate(() => n);
    busy(0);
    n = 1;
    check("busy while traffic is arriving", busy(100) === true);
    check("still busy inside the window", busy(100 + FOREIGN_QUIET_MS - 1) === true);
    check("QUIET once the window passes", busy(100 + FOREIGN_QUIET_MS) === false,
          "the restate must come back on its own when Move stops, or the " +
          "text stays frozen until something else happens to change it");
}

/* --- a throwing host is never busy ---------------------------------- */
{
    const busy = makeGate(() => { throw new Error("no host"); });
    check("a throwing accessor is not busy", busy(0) === false,
          "failing closed would park the heartbeat forever on any host that " +
          "does not provide the binding -- the feature must degrade to its " +
          "old behaviour, not to a dead screen");
}

/* --- A CHANGE IS NEVER GATED ---------------------------------------- *
 * The load-bearing one. invalidate() is what a page turn calls, and it
 * must reach the device whatever Move is doing.
 */
{
    const d = createDisplay();
    const sent = [];
    const send = (p) => { sent.push(p.length); return true; };
    const screen = { kind: "labels", title: "T", labels: Array(16).fill("ab") };

    d.invalidate();
    const what = d.tick(send, () => new Uint8Array(1024), screen, 1000);
    check("a change SENDS", what === "labels" && sent.length === 1,
          "got " + what + " after " + sent.length + " send(s)");

    /* Nothing owed -> nothing goes out. This is what makes suppressing the
     * heartbeat free: a restate that is not requested costs nothing. */
    const idle = d.tick(send, () => new Uint8Array(1024), screen, 1100);
    check("an unchanged screen sends nothing", idle === null && sent.length === 1,
          "a restate carries no new information, so skipping it loses none");

    /* ...and the change path still works afterwards, i.e. suppression is not
     * sticky. */
    d.invalidate();
    const again = d.tick(send, () => new Uint8Array(1024), screen, 1200);
    check("a later change still sends", again === "labels" && sent.length === 2);
}

/* --- the two intervals must stay ordered ---------------------------- */
check("FOREIGN_QUIET_MS is shorter than the heartbeat",
      FOREIGN_QUIET_MS < SCREEN_HEARTBEAT_MS,
      "a quiet window longer than the heartbeat period means the restate is " +
      "skipped even in the gaps of ordinary playing, and the text never " +
      "catches up while a part is running");

if (fails) { console.log(fails + " check(s) failed"); process.exit(1); }
console.log("PASS: repair waits for quiet, a change does not");
'

# The gate above is a COPY, so pin the real one against it: the property is
# worthless if createSurface stops consulting it, or consults it on the wrong
# path. These two greps are what tie the model to the code.
SRC=src/shared/e16_surface.mjs
# THE HEARTBEAT IS NOT GATED, and that is the finding rather than an omission.
# Gating it was built, deployed and measured on 2026-09-11: the screen still
# garbled and the slot page was markedly worse, because suppressing the repair
# while the corruption remains means a garble persists instead of being fixed
# 1.5 s later. Re-introducing the gate without first removing the corruption
# repeats a measured regression, so this fails on its return.
grep -q "!display.ringsPending && !foreignBusy(t)" "$SRC" && {
    echo "FAIL: the heartbeat gate is back. It was measured on hardware and made" >&2
    echo "      the screen WORSE -- suppressing repair without removing the" >&2
    echo "      corruption leaves a garble on screen until something else" >&2
    echo "      repaints. See FOREIGN_QUIET_MS." >&2; exit 1; }

# Nothing consults it at all right now. Counting raw matches would count the
# PROSE too -- this file and the source both explain themselves at length, and
# a doc comment naming foreignBusy() is not a call site.
hits=$(grep "foreignBusy(" "$SRC" | grep -vcE '''^[[:space:]]*(\*|//|/\*)''')
[ "$hits" -eq 1 ] || {
    echo "FAIL: foreignBusy() has $hits non-comment references (want 1: its" >&2
    echo "      definition). It is kept as measured plumbing for the next idea" >&2
    echo "      that needs 'is Move transmitting', not as a live gate." >&2; exit 1; }

echo "PASS: the signal is plumbed, the heartbeat is not gated on it"
