#!/usr/bin/env python3
"""Score a capture of Schwung's transport-test SysEx.

Feed it a text dump from any MIDI monitor that prints hex bytes (SysEx
messages one per line, or a whole stream -- it splits on F0/F7 itself).

The message is self-verifying by construction:

    F0 00 21 5B 02 01 7F <seq> <ramp...> F7

    seq   increments per message, so a WHOLE message lost shows up as a gap
    ramp  byte i is (i & 0x7F), so any missing or altered byte breaks the
          sequence at an offset this prints

Which is the point: "did it arrive intact" must not depend on anybody
eyeballing 1180 bytes.

    python3 verify_blast.py capture.txt
"""
import re
import sys


def messages(text):
    """Every F0..F7 run in the dump, as byte lists."""
    nybbles = re.findall(r'\b[0-9A-Fa-f]{2}\b', text)
    stream = [int(b, 16) for b in nybbles]
    out, cur = [], None
    for b in stream:
        if b == 0xF0:
            cur = [b]
        elif cur is not None:
            cur.append(b)
            if b == 0xF7:
                out.append(cur)
                cur = None
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    text = open(sys.argv[1]).read()
    msgs = [m for m in messages(text)
            if len(m) > 8 and m[1:6] == [0x00, 0x21, 0x5B, 0x02, 0x01] and m[6] == 0x7F]

    if not msgs:
        print("no transport-test messages found "
              "(expected F0 00 21 5B 02 01 7F ... F7)")
        return 1

    print(f"{len(msgs)} test message(s)")
    lengths = {len(m) for m in msgs}
    print(f"lengths seen: {sorted(lengths)}")

    bad, prev_seq = 0, None
    for m in msgs:
        seq = m[7]
        payload = m[8:-1]
        # Whole messages lost show up as a gap in seq (mod 128).
        if prev_seq is not None and seq != (prev_seq + 1) % 128:
            print(f"  seq gap: {prev_seq} -> {seq}  "
                  f"({(seq - prev_seq - 1) % 128} message(s) lost whole)")
        prev_seq = seq
        # A ramp break names the offset, which is what makes this diagnostic
        # rather than merely a pass/fail.
        for i, b in enumerate(payload):
            if b != (i & 0x7F):
                print(f"  seq {seq}: ramp breaks at offset {i} "
                      f"(got 0x{b:02x}, want 0x{i & 0x7F:02x}), "
                      f"message is {len(m)} bytes")
                bad += 1
                break

    print(f"\n{len(msgs) - bad} intact, {bad} corrupt "
          f"({100.0 * bad / len(msgs):.1f}% loss)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
