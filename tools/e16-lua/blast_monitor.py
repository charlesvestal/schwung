#!/usr/bin/env python3
"""Score Schwung's transport-test SysEx live, on whatever receives it.

Half of the experiment that separates the E16 from the link. Schwung emits a
self-verifying message every 250 ms (arm it with
`echo 1171 > /data/UserData/schwung/e16_blast`); this opens a MIDI input on
this machine -- a WIDI adapter paired to the Move, say -- and reports whether
the bytes survive the trip.

    F0 00 21 5B 02 01 7F <seq> <ramp...> F7

    seq   increments per message: a whole message lost is a gap
    ramp  byte i is (i & 0x7F): any missing or altered byte breaks it, at an
          offset this names

Usage:
    python3 blast_monitor.py                # list inputs
    python3 blast_monitor.py "WIDI" 30      # watch that port for 30 s
"""
import sys
import time

import rtmidi

HDR = [0x00, 0x21, 0x5B, 0x02, 0x01]


def main():
    m = rtmidi.MidiIn()
    ports = m.get_ports()
    if len(sys.argv) < 2:
        print("MIDI inputs:")
        for i, p in enumerate(ports):
            print(f"  {i}: {p}")
        print('\nthen: python3 blast_monitor.py "<name or index>" [seconds]')
        return 0

    want = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    idx = int(want) if want.isdigit() else next(
        (i for i, p in enumerate(ports) if want.lower() in p.lower()), None)
    if idx is None:
        print(f"no input matching {want!r}; have: {ports}")
        return 1

    # SysEx is OFF by default in rtmidi -- the one setting that makes this
    # print "nothing arrived" on a perfectly good link.
    m.open_port(idx)
    m.ignore_types(sysex=False, timing=True, active_sense=True)
    print(f"watching {ports[idx]!r} for {secs:.0f}s ...")

    seen = intact = corrupt = 0
    lost = 0
    prev = None
    lengths = set()
    t_end = time.time() + secs
    while time.time() < t_end:
        msg = m.get_message()
        if not msg:
            time.sleep(0.001)
            continue
        data, _ = msg
        if len(data) < 9 or data[0] != 0xF0 or list(data[1:6]) != HDR or data[6] != 0x7F:
            continue
        seen += 1
        lengths.add(len(data))
        seq = data[7]
        if prev is not None and seq != (prev + 1) % 128:
            gap = (seq - prev - 1) % 128
            lost += gap
            print(f"  seq gap {prev} -> {seq}: {gap} message(s) lost whole")
        prev = seq
        payload = data[8:-1]
        broke = next((i for i, b in enumerate(payload) if b != (i & 0x7F)), None)
        if broke is None:
            intact += 1
        else:
            corrupt += 1
            print(f"  seq {seq}: ramp breaks at offset {broke} "
                  f"(got 0x{payload[broke]:02x}, want 0x{broke & 0x7F:02x}); "
                  f"message {len(data)} bytes")

    total = intact + corrupt
    print(f"\n{seen} test message(s), lengths {sorted(lengths) or '-'}")
    print(f"  intact       {intact}")
    print(f"  corrupt      {corrupt}")
    print(f"  lost whole   {lost}")
    if total:
        print(f"  -> {100.0 * corrupt / total:.1f}% of ARRIVING messages corrupt")
    if seen == 0:
        print("  nothing arrived: is the blast armed, and is this the right port?")
    return 0


if __name__ == "__main__":
    sys.exit(main())
