#!/usr/bin/env python3
"""Send an OSC message with a string argument via UDP."""
import socket, sys

def osc_string(s):
    b = s.encode() + b"\x00"
    while len(b) % 4:
        b += b"\x00"
    return b

if len(sys.argv) < 3:
    print("Usage: osc_send.py <path> <value>", file=sys.stderr)
    sys.exit(1)

path = sys.argv[1]
value = sys.argv[2]
msg = osc_string(path) + osc_string(",s") + osc_string(value)
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.sendto(msg, ("127.0.0.1", 1234))
