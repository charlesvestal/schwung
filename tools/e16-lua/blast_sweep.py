#!/usr/bin/env python3
"""Sweep outbound SysEx size against corruption, and name the SHAPE of the loss.

WHY THIS EXISTS, AND WHAT IT DECIDES

Schwung's E16 screen garbles whenever Move sends note data on the same cable.
Isolated on hardware 2026-09-11 with the transport STOPPED both ways: notes with
Move's MIDI out ON garbles, MIDI out OFF is clean -- while 559 packets/sec with
no foreign traffic stayed perfectly clean. So it is not bandwidth and not our
rate. Two mechanisms remain, and they are NOT distinguishable from the screen:

  1. PROTOCOL ABORT. Only System Realtime bytes (0xF8-0xFF) may appear inside a
     SysEx. A Note On spliced into one makes a conformant receiver discard the
     message. Predicts: failure is independent of message SIZE, and the
     corruption offset is wherever the note happened to land -- i.e. RANDOM.

  2. BUFFER DEPTH. docs/SYSEX.md measured the INBOUND ceiling as a fixed
     truncation at 381 bytes regardless of message size (400/512/632 B all cut
     at 381), with two 316 B messages 100 ms apart both arriving whole. That is
     a fixed-size buffer, not a rate. If the outbound path has the same shape,
     failure scales with BYTES IN FLIGHT and the offset is CONSTANT.

The remedy differs completely. (1) is the receiver's parser and is OXI's to fix;
(2) is ours, and means the answer is fewer bytes -- the Lua path, where a page
change is ~60 packets instead of 394 and a value change is 5 bytes.

This sweep separates them, and produces a report meant to be pasted to OXI.

HOW IT WORKS

`e16_blast` emits a self-verifying message every 250 ms, independent of the
surface and its ACK gate, so nothing here depends on the E16 agreeing to talk:

    F0 00 21 5B 02 01 7F <seq> <ramp...> F7

`seq` catches a message lost whole; the ramp (byte i == i & 0x7F) catches any
altered or missing byte AND NAMES THE OFFSET. The offset is the entire point --
it is what tells a buffer boundary from a splice.

RUN IT TWICE:

    python3 blast_sweep.py --port WIDI --label notes-off     # hands off, silent
    python3 blast_sweep.py --port WIDI --label notes-on      # play throughout

THE INSTRUMENT CAN LIE, AND IT ALREADY HAS. A Bluetooth adapter saturating
merges messages: this investigation nearly reported a "5785-byte message"
(longer than anything sent) as Move dropping packets. Any arrival LONGER than
what was sent is the adapter, not the device, so those rows are marked VOID
rather than averaged in.
"""
import argparse
import collections
import subprocess
import sys
import time

try:
    import rtmidi
except ImportError:
    sys.exit("pip install python-rtmidi")

HDR = [0x00, 0x21, 0x5B, 0x02, 0x01]
BLAST = "/data/UserData/schwung/e16_blast"

# Spanning tiny -> framebuffer. 381 is deliberate: it is the INBOUND truncation
# boundary docs/SYSEX.md measured, and the first thing to check is whether the
# outbound path has the same one.
SIZES = [12, 46, 92, 184, 381, 600, 1171]


def arm(host, n):
    """Set the blast payload length on the device (0 disarms)."""
    cmd = f"echo {n} > {BLAST}" if n else f"rm -f {BLAST}"
    subprocess.run(["ssh", host, cmd], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def watch(midi_in, secs, sent_len):
    """Score arrivals for `secs`. Returns a dict of what happened."""
    seen = intact = corrupt = lost = overlong = 0
    offsets = []
    lengths = collections.Counter()
    prev = None
    t_end = time.time() + secs
    while time.time() < t_end:
        msg = midi_in.get_message()
        if not msg:
            time.sleep(0.001)
            continue
        data, _ = msg
        if len(data) < 9 or data[0] != 0xF0 or list(data[1:6]) != HDR or data[6] != 0x7F:
            continue
        seen += 1
        lengths[len(data)] += 1
        # The adapter merging two messages reports MORE bytes than were sent.
        # That is the instrument failing, not the device.
        if len(data) > sent_len + 9:
            overlong += 1
        seq = data[7]
        if prev is not None and seq != (prev + 1) % 128:
            lost += (seq - prev - 1) % 128
        prev = seq
        payload = data[8:-1]
        broke = next((i for i, b in enumerate(payload) if b != (i & 0x7F)), None)
        if broke is None:
            intact += 1
        else:
            corrupt += 1
            offsets.append(broke)
    return dict(seen=seen, intact=intact, corrupt=corrupt, lost=lost,
                overlong=overlong, offsets=offsets, lengths=lengths)


def offset_shape(offsets):
    """FIXED offsets mean a buffer boundary; SCATTERED means a splice.

    This is the whole discriminator, so it says "too few to tell" rather than
    guessing from one or two samples -- a shape inferred from a single point is
    how this bug spent three sessions being called a rate problem.
    """
    if not offsets:
        return "-", ""
    if len(offsets) < 3:
        return "?", f"only {len(offsets)} sample(s) -- too few to tell"
    lo, hi = min(offsets), max(offsets)
    spread = hi - lo
    common = collections.Counter(offsets).most_common(1)[0]
    if spread <= 4:
        return "FIXED", f"all within {spread} of {lo} -- a buffer boundary"
    if common[1] >= 0.6 * len(offsets):
        return "MOSTLY FIXED", f"{common[1]}/{len(offsets)} at {common[0]}"
    return "SCATTERED", f"{lo}..{hi} -- consistent with a splice"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="MIDI input name or index")
    ap.add_argument("--host", default="ableton@move.local")
    ap.add_argument("--label", default="run", help="notes-off / notes-on")
    ap.add_argument("--secs", type=float, default=20.0, help="per size")
    ap.add_argument("--sizes", default="", help="comma list, overrides default")
    args = ap.parse_args()

    sizes = [int(x) for x in args.sizes.split(",")] if args.sizes else SIZES

    m = rtmidi.MidiIn()
    ports = m.get_ports()
    idx = int(args.port) if args.port.isdigit() else next(
        (i for i, p in enumerate(ports) if args.port.lower() in p.lower()), None)
    if idx is None:
        sys.exit(f"no MIDI input matching {args.port!r}; have: {ports}")
    m.open_port(idx)
    # SysEx is OFF by default in rtmidi -- the one setting that makes this
    # print "nothing arrived" on a perfectly good link.
    m.ignore_types(sysex=False, timing=True, active_sense=True)

    print(f"# E16 outbound SysEx sweep -- {args.label}")
    print(f"# port {ports[idx]!r}, {args.secs:.0f}s per size\n")
    print(f"{'bytes':>6} {'arrived':>8} {'intact':>7} {'corrupt':>8} "
          f"{'lost':>5} {'corrupt%':>9}  offsets")
    print("-" * 78)

    rows = []
    try:
        for n in sizes:
            arm(args.host, n)
            time.sleep(1.5)          # the device re-reads the file once a second
            while m.get_message():   # drop anything queued at the old size
                pass
            r = watch(m, args.secs, n)
            #
            # NOTHING ARRIVING IS A DEAD RIG, NOT A CLEAN LINK.
            #
            # A BLE MIDI port stays listed in CoreMIDI after the adapter
            # disconnects, so the sweep opens it happily and scores silence. On
            # 2026-09-11 that produced a full table of 0.0% corrupt -- which
            # reads as a PASS -- while the device was verifiably sending 2766
            # packets/sec. Two and a half minutes to measure a disconnected
            # adapter, and the output looked like the best result of the day.
            #
            # Stop on the FIRST size instead. Anything else buys a page of
            # zeros that someone will eventually quote.
            if r["seen"] == 0:
                print(f"\n!! nothing arrived at {n} bytes -- the receiver is not "
                      f"hearing the device.")
                print("   A Bluetooth adapter that has dropped its link still "
                      "appears in CoreMIDI, so an open port proves nothing.")
                print("   Check the link is UP (clock/notes should be visible), "
                      "then rerun. Not scoring the remaining sizes: a table of "
                      "zeros from a dead rig reads exactly like a pass.")
                return 1
            total = r["intact"] + r["corrupt"]
            pct = (100.0 * r["corrupt"] / total) if total else 0.0
            shape, note = offset_shape(r["offsets"])
            flag = "  VOID(adapter merged)" if r["overlong"] else ""
            print(f"{n:>6} {r['seen']:>8} {r['intact']:>7} {r['corrupt']:>8} "
                  f"{r['lost']:>5} {pct:>8.1f}%  {shape} {note}{flag}")
            rows.append((n, r, pct, shape, bool(r["overlong"])))
    finally:
        arm(args.host, 0)

    print("\n## What this says")
    #
    # A VOID ROW IS NOT DATA, AND MUST NOT REACH A VERDICT.
    #
    # The first run of this sweep classified 600 and 1171 bytes as SCATTERED and
    # concluded "size DEPENDENT" -- from the two rows it had ITSELF just marked
    # VOID because the Bluetooth adapter merged messages. Marking a row void and
    # then averaging it in is worse than not marking it, because the label makes
    # the output look careful. Everything below reads `good` only.
    good = [(n, r, p, sh) for n, r, p, sh, void in rows if not void]
    voided = [n for n, _, _, _, void in rows if void]
    if voided:
        print(f"  EXCLUDED (adapter saturated, not the device): {voided}")
        print("  Those sizes are unmeasurable on this rig -- BLE MIDI merges")
        print("  messages well below a framebuffer at 4/sec. A smaller rate or")
        print("  a wired receiver is needed to say anything about them.")
    if not good:
        print("  No valid rows. Nothing can be concluded.")
        return 0

    shapes = [sh for _, _, _, sh in good if sh in ("FIXED", "MOSTLY FIXED")]
    scattered = [sh for _, _, _, sh in good if sh == "SCATTERED"]
    any_corrupt = any(p > 0 for _, _, p, _ in good)

    if not any_corrupt:
        print("  Nothing corrupted. If this is the notes-on pass, the fault was")
        print("  NOT reproduced -- check notes were actually playing and that")
        print("  Move's MIDI out is ON (sync being off is not enough; that")
        print("  mistake voided an earlier experiment).")
    elif shapes and not scattered:
        print("  Corruption at a CONSISTENT offset => a fixed-size buffer on the")
        print("  outbound path, the same shape as the 381-byte inbound ceiling.")
        print("  Failure scales with bytes in flight, so the remedy is FEWER")
        print("  BYTES (the Lua path: ~60 packets a page, 5 bytes a value).")
    elif scattered and not shapes:
        # Deliberately does NOT say "because of foreign traffic": this script is
        # told a LABEL, not a condition, and cannot verify what was playing. The
        # comparison between the two passes is the evidence; one pass is not.
        print("  Corruption at SCATTERED offsets => consistent with a foreign")
        print("  packet spliced into the run, NOT with a buffer boundary.")
        print("  Compare against the other pass before concluding: a single pass")
        print("  cannot tell you what CAUSED it, only what shape it has.")
    else:
        print("  Mixed: both a consistent boundary and scattered breaks. Likely")
        print("  BOTH mechanisms; report the per-size table rather than a verdict.")

    small = [(n, p) for n, _, p, _ in good if n <= 92]
    large = [(n, p) for n, _, p, _ in good if n >= 600]
    if small and large:
        s_avg = sum(p for _, p in small) / len(small)
        l_avg = sum(p for _, p in large) / len(large)
        print(f"\n  small (<=92B) {s_avg:.1f}% corrupt vs large (>=600B) {l_avg:.1f}%")
        if l_avg > s_avg * 2:
            print("  -> size DEPENDENT: bytes in flight matter.")
        elif s_avg > 0 and abs(l_avg - s_avg) < max(5.0, s_avg * 0.5):
            print("  -> size INDEPENDENT: a short message fails as readily as a")
            print("     long one, which a buffer limit cannot explain.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
