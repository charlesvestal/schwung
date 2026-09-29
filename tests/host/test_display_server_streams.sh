#!/usr/bin/env bash
# THE MIRROR STREAM SURVIVES A BAD LINK. display-server is built for the host
# with its SHM paths pointed at plain files, fed a changing screen and a
# control-surface page, and read by raw-socket clients:
#
#   1. /stream-auto?v=2 carries frames, `surface`, `e16` and the `hb`
#      heartbeat the page's watchdog keys on -- through a reader STALL, with
#      not one corrupt event. Built with a 2 KB socket buffer and an 8 KB
#      queue, so the stall really does back the queue up and exercise the
#      short-write path (loopback buffers are otherwise large enough to hide
#      it: a 20 s stall on the default build never touched the queue).
#      Before: one non-blocking write() per event -- EAGAIN dropped the
#      client, and a short write spliced half an event into the next.
#   2. A reader that stays stalled past STALL_DROP_MS IS dropped (a dead peer
#      must not hold a slot).
#   3. Bare /stream-auto (an old page) sees only default events and `hb`.
#   4. Twenty idle streams against sixteen slots: the newest is still served
#      (evict-oldest), where it used to be refused and the proxy said 502.
#
# Mutation check, done by hand: making client_flush treat EAGAIN as an error
# (the old behaviour) fails case 1.
set -euo pipefail
cd "$(dirname "$0")/../.."
command -v python3 >/dev/null || { echo "SKIP: no python3"; exit 0; }
CC=${CC:-cc}
T=$(mktemp -d "${TMPDIR:-/tmp}/ds_streams.XXXXXX")
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT
"$CC" -O1 -Isrc/host \
    -DSHM_PATH="\"$T/live\"" -DNORNS_SHM_PATH="\"$T/norns\"" \
    -DSURFACE_LIVE_SHM_PATH="\"$T/surface\"" -DE16_MIRROR_SHM_PATH="\"$T/e16\"" \
    -DDISPLAY_SERVER_TEST_SNDBUF=2048 -DOUT_CAP=8192 -DSTALL_DROP_MS=3000 \
    -o "$T/ds" src/host/display_server.c src/host/unified_log.c -lpthread

T="$T" python3 - <<'EOF'
import json, mmap, os, random, socket, struct, subprocess, sys, threading, time
T = os.environ["T"]; PORT = 17000 + os.getpid() % 2000
files = []
def mk(path, size):
    with open(path, "wb") as f: f.write(b"\0" * size)
    files.append(open(path, "r+b"))           # the mapping outlives nothing it needs
    return mmap.mmap(files[-1].fileno(), size)
live = mk(T + "/live", 1024)
surf = mk(T + "/surface", 24 + 15 * 128 + 32 + 32 * 8)
surf[0:8] = b"SURFLV1\0"; surf[8:12] = struct.pack("<I", 1)
stop = False
def writer():
    n = 0
    while not stop:
        live[:] = random.randbytes(1024); n += 1
        surf[24 + 68] = n % 127
        time.sleep(0.02)
threading.Thread(target=writer, daemon=True).start()
srv = subprocess.Popen([T + "/ds", str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try: socket.create_connection(("127.0.0.1", PORT)).close(); break
    except OSError: time.sleep(0.1)

def stream(path):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    s.connect(("127.0.0.1", PORT)); s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    return s
def read(s, secs, stall_at=None, stall=0):
    s.settimeout(0.3); buf = b""; t0 = time.time(); closed = False; done = False
    while time.time() - t0 < secs:
        if stall_at is not None and not done and time.time() - t0 > stall_at:
            done = True; time.sleep(stall)
        try: d = s.recv(1 << 16)
        except socket.timeout: continue
        except OSError: closed = True; break
        if not d: closed = True; break
        buf += d
    return buf, closed
def events(buf):
    body = buf.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in buf else b""
    out, bad = [], 0
    for raw in body.split(b"\n\n")[:-1]:       # the tail is unterminated: our read stopped
        name, data = b"message", None
        for line in raw.split(b"\n"):
            if line.startswith(b"event: "): name = line[7:]
            elif line.startswith(b"data: "): data = line[6:]
        if data is None: bad += 1; continue
        try: json.loads(data)
        except ValueError: bad += 1; continue
        out.append(name.decode())
    return out, bad

fails = 0
def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    if not cond: fails += 1

s = stream("/stream-auto?v=2")
buf, closed = read(s, 6, stall_at=1.0, stall=2.0)
ev, bad = events(buf); s.close()
check(not closed, "a 2 s stall does not drop the client")
check(bad == 0, f"no corrupt event across the stall ({bad})")
check(ev.count("surface") > 5 and ev.count("message") > 5, "frames and surface keep coming after it")
check(ev.count("hb") >= 2 and ev.count("e16") >= 1, "heartbeat and e16 ride the same stream")

s = stream("/stream-auto?v=2")
buf, closed = read(s, 9, stall_at=0.5, stall=6.0); s.close()
check(closed, "a reader stalled past STALL_DROP_MS is dropped")

s = stream("/stream-auto")
buf, _ = read(s, 2.5); ev, bad = events(buf); s.close()
check(bad == 0 and set(ev) <= {"message", "hb"} and "message" in ev, "an old client sees only frames and hb")

socks = [stream("/stream-auto?v=2") for _ in range(20)]
time.sleep(1.0)
buf, closed = read(socks[-1], 1.5); ev, _ = events(buf)
check(len(ev) > 0 and not closed, "past sixteen streams the newest is still served")
for x in socks: x.close()

stop = True; srv.terminate(); srv.wait()
sys.exit(1 if fails else 0)
EOF
