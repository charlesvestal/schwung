#!/usr/bin/env python3
"""Attribute a lost MIDI_OUT message to the window it was lost in.

Reads the capture written by the shim's XMOS logger
(/data/UserData/schwung/xmos_sysex.txt, armed with log_xmos_sysex_on) and, for
every frame, compares the three views it records:

    PRE     early in shim_pre_transfer
    PREEND  its last statement, immediately before the library's shadow->hw copy
    POSThw  the hardware mailbox, after the ioctl

    PRE == PREEND != POSThw   lost during the IOCTL      (Move's other threads,
                                                          or the hardware)
    PRE != PREEND             lost in SCHWUNG'S own pre-transfer work
    PRE == PREEND == POSThw   survived

Why this exists: captured 2026-09-12, 9 of the 13 37-family XMOS control
messages Move emitted were replaced in the mailbox by an RGB LED SysEx (3b 10)
before reaching the wire, so Move's USB-C audio-out selection silently did
nothing on those frames — reported as "Main Out stops working until a reboot".
A two-view capture could see the loss but not say who caused it, which is
exactly the question that decides whether this is our bug.

Usage:
    tools/xmos/attribute_midi_out.py xmos_sysex.txt            # summary
    tools/xmos/attribute_midi_out.py xmos_sysex.txt --key 37   # only 37-family
    tools/xmos/attribute_midi_out.py xmos_sysex.txt --frame 13596
"""
import argparse
import collections
import re
import sys

LINE = re.compile(
    r"\[f(\d+)\]\s+(PRE|PREEND|POSThw)\s+slot=\s*(\d+)\s+cable=(\d)\s+cin=0x(\w)\s+:\s+"
    r"([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2}) ([0-9a-f]{2})"
)
VIEWS = ("PRE", "PREEND", "POSThw")

# USB-MIDI SysEx framing: CIN 0x04 carries three payload bytes and continues;
# 0x05/0x06/0x07 carry 1/2/3 and terminate.
PAYLOAD_LEN = {0x04: 3, 0x05: 1, 0x06: 2, 0x07: 3}
ENDS = {0x05, 0x06, 0x07}


def parse(path):
    """-> {(segment, frame): {view: {slot: (b0,b1,b2,b3)}}}, segment list."""
    frames = collections.defaultdict(lambda: collections.defaultdict(dict))
    segment = 0
    segments = []
    with open(path, errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            if "BOOT" in line:
                segment += 1
                segments.append((segment, lineno))
                continue
            m = LINE.match(line)
            if not m:
                continue
            frame = int(m.group(1))
            view = m.group(2)
            slot = int(m.group(3))
            pkt = tuple(int(m.group(i), 16) for i in (6, 7, 8, 9))
            frames[(segment, frame)][view][slot] = pkt
    return frames, segments


def messages(slots):
    """Reassemble cable-0 SysEx from a {slot: packet} map.

    -> [(first_slot, bytes)]. Order is slot order, which is wire order.
    """
    out = []
    buf = []
    first = None
    for slot in sorted(slots):
        cin = slots[slot][0] & 0x0F
        cable = (slots[slot][0] >> 4) & 0x0F
        if cable != 0 or cin not in PAYLOAD_LEN:
            continue
        payload = list(slots[slot][1 : 1 + PAYLOAD_LEN[cin]])
        if payload and payload[0] == 0xF0:
            buf, first = [], slot
        buf.extend(payload)
        if cin in ENDS and first is not None:
            out.append((first, bytes(buf)))
            buf, first = [], None
    return out


def label(msg):
    """A short name for an Ableton SysEx: F0 00 21 1D 01 01 <cmd> <key> ..."""
    if len(msg) >= 8 and msg[:6] == bytes([0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01]):
        cmd, key = msg[6], msg[7]
        val = msg[8] if len(msg) > 8 else None
        name = {0x37: "XMOS-ctl", 0x3B: "RGB-LED"}.get(cmd, f"cmd{cmd:02x}")
        if val is None:
            return f"{name} {cmd:02x} {key:02x}"
        return f"{name} {cmd:02x} {key:02x} {val:02x}"
    return "sysex(" + msg[:4].hex(" ") + "…)"


def classify(views):
    """-> (verdict, pre, preend, post) as label lists."""
    have = {v: (v in views) for v in VIEWS}
    pre = [label(m) for _, m in messages(views.get("PRE", {}))]
    end = [label(m) for _, m in messages(views.get("PREEND", {}))]
    post = [label(m) for _, m in messages(views.get("POSThw", {}))]

    if not have["PREEND"]:
        return "NO-PREEND (old shim?)", pre, end, post
    if pre == end == post:
        return "survived", pre, end, post
    if pre != end:
        return "LOST IN SCHWUNG PRE-TRANSFER", pre, end, post
    return "lost during the ioctl", pre, end, post


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--key", help="only frames whose PRE mentions this cmd byte, e.g. 37")
    ap.add_argument("--frame", type=int, help="dump one frame in full")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    frames, segments = parse(args.capture)
    if not frames:
        print(f"no parsable lines in {args.capture}", file=sys.stderr)
        return 1

    has_preend = any("PREEND" in v for v in frames.values())
    print(f"{len(frames)} frames, {len(segments)} capture segment(s), "
          f"PREEND view {'present' if has_preend else 'ABSENT — pre-#500 shim, cannot attribute'}")
    print()

    tally = collections.Counter()
    rows = []
    for key in sorted(frames):
        verdict, pre, end, post = classify(frames[key])
        tally[verdict] += 1
        if args.frame is not None and key[1] != args.frame:
            continue
        if args.key and not any(args.key in p for p in pre):
            continue
        if verdict == "survived" and not args.frame:
            continue
        rows.append((key, verdict, pre, end, post))

    for (seg, frame), verdict, pre, end, post in rows[: args.limit]:
        print(f"seg{seg} f{frame}  {verdict}")
        print(f"    PRE    {pre}")
        print(f"    PREEND {end}")
        print(f"    POSThw {post}")
    if len(rows) > args.limit:
        print(f"... {len(rows) - args.limit} more (raise --limit)")

    print()
    print("verdict tally over every frame:")
    for verdict, n in tally.most_common():
        print(f"  {n:7d}  {verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
