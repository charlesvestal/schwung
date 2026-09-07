#!/usr/bin/env python3
"""Mod routes, verified on a REAL Move.

Not a unit test and deliberately not named test_*: it needs a device with this
build installed and schwung-testd tunnelled, so pytest must not collect it in
CI. Run it by hand:

    ssh ableton@move.local /data/UserData/schwung/bin/schwung-testd &
    ssh -N -L 47777:127.0.0.1:47777 ableton@move.local &
    python3 tools/pytest-schwung/tests/device_mod_routes.py

It expects 9w9 in slot 0 (it loads it) and leaves the slot at its base values.


Every number below is one the DEVICE produced: injected MIDI in, chain-host
param reads out. Nothing is inferred from source.

MIDI GOES IN ON CABLE 2, not cable 0. Cable 0 is Move's own control surface, so
an injected cable-0 note is treated as a pad press and never reaches the slot;
cable 2 is routed by channel to the track instrument AND echoed back out
MIDI_OUT cable 2, which is what feeds a chain slot. Measured, not assumed: the
identical route reads effective=64 (unmoved) on cable 0 and 121 on cable 2.
"""
import sys
import time

sys.path.insert(0, "tools/pytest-schwung/src")
from schwung_bus.client import SchwungBus  # noqa: E402

KEY = "bd_c_tune"      # 9w9, int 0..127
LO, HI = 0.0, 127.0
BASE = 64

fails, oks = [], 0


def check(cond, msg):
    global oks
    if cond:
        print(f"  ok  {msg}")
        oks += 1
    else:
        print(f"FAIL: {msg}")
        fails.append(msg)


def main():
    bus = SchwungBus()
    bus.connect()
    print(f"connected: {bus.ping()}\n")

    def s(k, v):
        try:
            bus.set_param(k, str(v), overtake=False)
        except Exception:
            print(f"      (set {k} failed)")

    def g(k):
        try:
            return bus.get_param(k, overtake=False)
        except Exception:
            return "<err>"

    def num(x):
        try:
            return float(x)
        except Exception:
            return None

    def note(vel, ch=0):
        bus.inject_midi(bytes([0x29, 0x90 | ch, 60, vel]))
        bus.wait_frame(30)
        time.sleep(0.35)

    def note_off(ch=0):
        bus.inject_midi(bytes([0x28, 0x80 | ch, 60, 0]))
        bus.wait_frame(5)
        time.sleep(0.15)

    def cc(num_, val, ch=0):
        bus.inject_midi(bytes([0x2B, 0xB0 | ch, num_, val]))
        bus.wait_frame(30)
        time.sleep(0.35)

    def pressure(val, ch=0):
        bus.inject_midi(bytes([0x2D, 0xD0 | ch, val, 0]))
        bus.wait_frame(30)
        time.sleep(0.35)

    def arm(src, depth="0.9", polarity="1", slew="0", cc_num=None):
        for n in range(1, 9):
            s(f"mod{n}:enabled", "0")
        time.sleep(0.2)
        s("synth:" + KEY, BASE)
        for k, v in (("mod1:src", src), ("mod1:target", "synth"),
                     ("mod1:target_param", KEY), ("mod1:depth", depth),
                     ("mod1:polarity", polarity), ("mod1:slew", slew)):
            s(k, v)
        if cc_num is not None:
            s("mod1:cc_num", cc_num)
        s("mod1:enabled", "1")
        time.sleep(0.3)

    eff = lambda: num(g(f"synth:{KEY}:effective"))
    base = lambda: num(g(f"synth:{KEY}"))

    print("=== 1. VELOCITY drives the target, and the base does not move ===")
    arm("velocity")
    note(127); hard, hb = eff(), base(); note_off()
    s("synth:" + KEY, BASE); time.sleep(0.2)
    note(1);   soft, sb = eff(), base(); note_off()
    print(f"  vel 127 -> effective {hard}, base {hb}")
    print(f"  vel   1 -> effective {soft}, base {sb}")
    check(hard is not None and soft is not None and hard > soft,
          f"a hard note drives it higher than a soft one ({hard} > {soft})")
    check(hard is not None and soft is not None and (hard - soft) > 80,
          f"the sweep spans most of the 0..127 range ({hard - soft:.0f})")
    check(hb == BASE and sb == BASE,
          f"the BASE never moves -- #276 ({hb}, {sb}, set {BASE})")
    check(g(f"synth:{KEY}:modulated") == "1", ":modulated reports 1")

    print("\n=== 2. PRESSURE (channel aftertouch) ===")
    arm("pressure")
    pressure(127); p_hi = eff()
    pressure(0);   p_lo = eff()
    print(f"  pressure 127 -> {p_hi};  pressure 0 -> {p_lo}")
    check(p_hi is not None and p_lo is not None and p_hi > p_lo,
          f"pressure drives the target ({p_hi} > {p_lo})")

    print("\n=== 3. CC, on the number the route names ===")
    arm("cc", cc_num=74)
    cc(74, 127); c_hi = eff()
    cc(74, 0);   c_lo = eff()
    cc(75, 127); c_other = eff()      # a DIFFERENT controller must not move it
    print(f"  CC74=127 -> {c_hi};  CC74=0 -> {c_lo};  then CC75=127 -> {c_other}")
    check(c_hi is not None and c_lo is not None and c_hi > c_lo,
          f"CC 74 drives the target ({c_hi} > {c_lo})")
    check(c_other == c_lo,
          f"CC 75 does NOT move a route listening to CC 74 ({c_other} == {c_lo})")

    print("\n=== 4. NOTE (key track) ===")
    arm("note")
    note(100); note_off()
    bus.inject_midi(bytes([0x29, 0x90, 120, 100])); bus.wait_frame(30); time.sleep(0.35)
    n_hi = eff()
    bus.inject_midi(bytes([0x28, 0x80, 120, 0])); bus.wait_frame(5)
    bus.inject_midi(bytes([0x29, 0x90, 12, 100])); bus.wait_frame(30); time.sleep(0.35)
    n_lo = eff()
    bus.inject_midi(bytes([0x28, 0x80, 12, 0])); bus.wait_frame(5)
    print(f"  note 120 -> {n_hi};  note 12 -> {n_lo}")
    check(n_hi is not None and n_lo is not None and n_hi > n_lo,
          f"a high note tracks higher than a low one ({n_hi} > {n_lo})")

    print("\n=== 5. SLEW smooths, and 0 does not ===")
    #
    # THE ROUTE MUST BE PRIMED LOW FIRST, or there is no glide to see. A
    # note-off latches nothing (release velocity is a different control), so
    # velocity survives it -- arm a route after a hard note and its first block
    # SEEDS straight onto the target, which is correct and looks identical to a
    # slew that does not work. Prime with a soft note, then jump.
    #
    for slew, label in (("0", "jump"), ("0.99", "glide")):
        arm("velocity", slew=slew)
        bus.inject_midi(bytes([0x29, 0x90, 60, 1]))     # prime LOW
        bus.wait_frame(60); time.sleep(0.6)
        note_off()
        start = eff()
        bus.inject_midi(bytes([0x29, 0x90, 60, 127]))   # then jump HIGH
        traj = [eff() for _ in range(6)]
        time.sleep(1.2)
        settled = eff()
        note_off()
        print(f"  slew {slew:4s}: start {start} -> {traj} -> settled {settled}")
        if slew == "0":
            check(traj[0] is not None and traj[0] > 100,
                  f"slew 0 arrives on the first read ({traj[0]})")
        else:
            check(traj[0] is not None and start is not None and traj[0] < start + 8,
                  f"slew 0.99 has barely moved on the first read ({traj[0]} from {start})")
            check(all(a is not None and b is not None and b >= a
                      for a, b in zip(traj, traj[1:])),
                  f"...and climbs monotonically: {traj}")
            check(settled is not None and settled > 100,
                  f"...arriving at the target once settled ({settled})")

    print("\n=== 6. Disabling restores the base ===")
    arm("velocity")
    note(127); note_off()
    check(eff() != BASE, "the route was driving before the clear")
    s("mod1:enabled", "0")
    time.sleep(0.3)
    check(base() == BASE, f"the parameter is back at its base ({base()})")
    check(g(f"synth:{KEY}:modulated") == "0", ":modulated reports 0")

    print("\n=== 7. EIGHT routes live, all summing ===")
    s("synth:" + KEY, 0)
    for n in range(1, 9):
        for k, v in ((f"mod{n}:src", "velocity"), (f"mod{n}:target", "synth"),
                     (f"mod{n}:target_param", KEY), (f"mod{n}:depth", "0.1"),
                     (f"mod{n}:polarity", "1"), (f"mod{n}:slew", "0"),
                     (f"mod{n}:enabled", "1")):
            s(k, v)
    time.sleep(0.4)
    active = [g(f"mod{n}:active") for n in range(1, 9)]
    check(all(a == "1" for a in active), f"all eight report active: {active}")
    note(127); e8 = eff(); note_off()
    print(f"  eight routes x depth 0.1, base 0 -> effective {e8}")
    # 8 x (1.0 * 0.1) * 0.5 * 127 = 50.8
    check(e8 is not None and abs(e8 - 50.8) < 4,
          f"eight contributions SUM to ~50.8, got {e8}")

    print("\n=== 8. the device survived it ===")
    check(g("mod1:active") in ("0", "1"), "the chain host is still answering")
    for n in range(1, 9):
        s(f"mod{n}:enabled", "0")
    s("synth:" + KEY, BASE)

    print(f"\n{oks} ok, {len(fails)} failed")
    for f in fails:
        print(f"  FAILED: {f}")
    bus.close()
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
