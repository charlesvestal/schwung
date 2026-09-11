#!/usr/bin/env python3
"""Drive the E16's framebuffer from this Mac -- the control for the whole
question.

Schwung instruments all four of its own outbound buffers and measures ZERO
loss under load while the E16's screen visibly corrupts. That puts the fault
downstream of Move's mailbox, and there are only two candidates left: the
Move's USB-A host path, or the E16 itself. A Mac is a known-good host, so if
it can hammer full framebuffers at the E16 without corruption, the E16 is
fine and the Move is dropping packets.

It draws MOVING STRIPES on purpose: a static image can be corrupted and look
deliberate, while a stripe that should march one pixel per frame makes a bad
frame obvious at a glance. Counts ACKs too, so a silent link is distinguished
from a corrupt one.

    python3 e16_drive.py                 # list ports
    python3 e16_drive.py "OXI E16" 20    # drive it for 20 seconds
    python3 e16_drive.py "OXI E16" 20 --labels   # 92-byte messages instead

`--labels` is the other half of the per-packet question: if loss scales with
packet count rather than starting above some size, a labels-sized message
should corrupt far less often on the same link.
"""
import random
import sys
import threading
import time

import rtmidi

MFG = [0x00, 0x21, 0x5B, 0x02, 0x01]
W, H = 128, 64


def pack7(raw):
    """8-to-7: one MSB byte per group of <=7, low bit first. The packing the
    device expects; getting it wrong looks exactly like a corrupt link."""
    out = []
    for i in range(0, len(raw), 7):
        group = raw[i:i + 7]
        msb = 0
        for k, b in enumerate(group):
            msb |= ((b >> 7) & 1) << k
        out.append(msb)
        out.extend(b & 0x7F for b in group)
    return out


def framebuffer(phase):
    """SSD1306 page/column: 8 pages of 128 columns, bit n of a byte is row n
    of that page. Vertical stripes that march with `phase`."""
    buf = bytearray(1024)
    for page in range(8):
        for col in range(W):
            buf[page * W + col] = 0xFF if ((col + phase) // 8) % 2 == 0 else 0x00
    return list(buf)


def msg(mid, payload):
    return [0xF0] + MFG + mid + pack7(payload) + [0xF7]


def main():
    out = rtmidi.MidiOut()
    inp = rtmidi.MidiIn()
    ports = out.get_ports()
    if len(sys.argv) < 2:
        print("MIDI outputs:")
        for i, p in enumerate(ports):
            print(f"  {i}: {p}")
        return 0

    want = sys.argv[1]
    secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith("--") else 20.0
    labels_mode = "--labels" in sys.argv
    clock_mode = "--clock" in sys.argv

    idx = int(want) if want.isdigit() else next(
        (i for i, p in enumerate(ports) if want.lower() in p.lower()), None)
    if idx is None:
        print(f"no output matching {want!r}; have: {ports}")
        return 1
    out.open_port(idx)

    iports = inp.get_ports()
    iidx = next((i for i, p in enumerate(iports) if want.lower() in p.lower()), None)
    if iidx is not None:
        inp.open_port(iidx)
        inp.ignore_types(sysex=False, timing=True, active_sense=True)

    print(f"entering remote mode on {ports[idx]!r} ...")
    out.send_message([0xF0] + MFG + [0x06, 0x55, 0xF7])
    time.sleep(0.3)

    acks = 0
    if iidx is not None:
        while True:
            m = inp.get_message()
            if not m:
                break
            d = m[0]
            if len(d) >= 8 and d[0] == 0xF0 and list(d[1:6]) == MFG and d[6:8] == [0x06, 0x53]:
                acks += 1
    print(f"  ACKs: {acks}" + ("" if acks else "  (none -- is it in remote mode?)"))

    sent = 0
    phase = 0
    clocks = 0
    t_end = time.time() + secs

    # --------------------------------------------------------------------
    # TRAFFIC ON A SECOND THREAD, which is the only faithful way to do this.
    #
    # Sending clock BETWEEN SysEx messages -- the obvious approach, and the
    # first thing tried -- does not reproduce what the Move does. A Schwung
    # message spans several SPI frames, and Move writes its own packets into
    # the shared mailbox DURING them, so the foreign bytes land INSIDE a
    # message the device is still assembling. Measured at ~20 per second while
    # our own message was in flight.
    #
    # From a host we cannot split a SysEx ourselves -- rtmidi sends one message
    # -- so the interleaving is handed to CoreMIDI: a thread hammering short
    # messages while another sends the big one gets them packetised together on
    # the wire, which is the same shape.
    # --------------------------------------------------------------------
    stop = threading.Event()
    counted = {"clock": 0, "note": 0, "pressure": 0}

    def traffic():
        chans = [0, 1, 2, 9]              # several channels, as asked
        while not stop.is_set():
            out.send_message([0xF8])                       # clock
            counted["clock"] += 1
            ch = random.choice(chans)
            if random.random() < 0.5:
                n = random.randint(36, 84)
                out.send_message([0x90 | ch, n, random.randint(1, 127)])
                out.send_message([0x80 | ch, n, 0])
                counted["note"] += 2
            if random.random() < 0.6:
                # Channel pressure AND poly pressure: both are the dense,
                # bursty kind of traffic a played performance actually makes.
                out.send_message([0xD0 | ch, random.randint(0, 127)])
                counted["pressure"] += 1
                if random.random() < 0.3:
                    out.send_message([0xA0 | ch, random.randint(36, 84),
                                      random.randint(0, 127)])
                    counted["pressure"] += 1
            time.sleep(0.02)              # ~50 Hz of mixed traffic

    if clock_mode:
        threading.Thread(target=traffic, daemon=True).start()
        print("  interleaving clock + notes + pressure on 4 channels, from a")
        print("  second thread, so the bytes land INSIDE the SysEx rather than")
        print("  between messages -- which is what the Move actually does.")
        print("  The MIDI spec PERMITS real-time bytes inside a SysEx, so a")
        print("  conforming receiver must cope.")
    while time.time() < t_end:
        if labels_mode:
            raw = [ord(c) for c in f"MAC {sent:>11}"[:16].ljust(16)]
            for i in range(16):
                raw += [ord(c) for c in f"{(sent + i) % 10000:>4}"[:4].ljust(4)]
            out.send_message(msg([0x06, 0x03], raw))
        else:
            out.send_message(msg([0x06, 0x02], framebuffer(phase)))
        sent += 1
        phase = (phase + 1) % 16
        time.sleep(0.1)

    stop.set()
    time.sleep(0.05)
    print(f"sent {sent} {'labels' if labels_mode else 'framebuffer'} message(s) "
          f"over {secs:.0f}s")
    if clock_mode:
        print(f"  interleaved: {counted['clock']} clock, {counted['note']} note, "
              f"{counted['pressure']} pressure")
    print("\nWatch the E16: the stripes should march smoothly and stay clean."
          if not labels_mode else
          "\nWatch the E16: the numbers should count up cleanly.")
    print("Any tearing, noise or frozen frame is corruption on a link driven "
          "by a known-good host -- which would mean the E16, not the Move.")
    out.send_message([0xF0] + MFG + [0x06, 0x00, 0xF7])   # leave remote mode
    return 0


if __name__ == "__main__":
    sys.exit(main())
