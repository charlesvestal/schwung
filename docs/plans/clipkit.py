"""Drive Move's clip gestures and read back lane state — the harness for the
new-clip permutation matrix.

A p-lock is "a component write made while a step is HELD", so it is driven as
note-on(step) / SET_PARAM / note-off(step) on one connection. Everything is
verified by reading `lanes:diag` back rather than by assuming the gesture took.
"""
import json, os, re, socket, subprocess, sys, time

D = "/data/UserData/schwung"
SONG = None   # resolved at import

def ensure_testd():
    """Start schwung-testd if it is not up.

    Every deploy restarts the service and kills it, so "connection refused"
    is the normal state after any install — and it aborts a run halfway,
    which is worse than not starting. Started here so no script has to
    remember."""
    r = subprocess.run(["pgrep", "-x", "schwung-testd"], capture_output=True)
    if r.returncode == 0: return False
    subprocess.Popen(["nohup", os.path.join(D, "schwung-testd")], cwd=D,
                     stdout=open(os.path.join(D, "testd.log"), "a"),
                     stderr=subprocess.STDOUT,
                     start_new_session=True)
    for _ in range(20):
        time.sleep(0.25)
        if subprocess.run(["pgrep","-x","schwung-testd"],
                          capture_output=True).returncode == 0:
            time.sleep(0.5); return True
    raise SystemExit("schwung-testd would not start")


def arm_lanes(on=True):
    """Arm or disarm the lanes kill switch.

    Lanes are OFF by default now, so every harness run must arm them or every
    scenario legitimately measures a disabled feature."""
    f = os.path.join(D, "lanes_on")
    if on:
        open(f, "w").close()
    else:
        try: os.remove(f)
        except FileNotFoundError: pass
    time.sleep(1.5)     # the worker polls ~1 Hz, then the shim pushes on change


def _autoarm():
    try: ensure_testd()
    except Exception: pass
    try: ensure_inject()
    except Exception: pass

def _song():
    global SONG
    if SONG: return SONG
    out = subprocess.run(["sh","-c","cat %s/active_set.txt" % D],
                         capture_output=True, text=True).stdout.strip().split("\n")
    uuid = out[0].strip()
    base = "/data/UserData/UserLibrary/Sets/" + uuid
    for name in os.listdir(base):
        p = os.path.join(base, name, "Song.abl")
        if os.path.exists(p): SONG = p; return p
    raise SystemExit("no Song.abl for " + uuid)

# ONE CONNECTION FOR ALL INJECTION.
#
# /schwung-midi-inject is an MPSC ring, and a packet pushed while ANOTHER
# producer is mid-write is refused ("prior producer stranded"). seq.py and
# tap.py each opened their own connection, so any injection made while a
# Testd() was open was silently dropped — every gesture simply did nothing,
# which reads as "the feature is broken" and wasted several measurements here.
_inj = None
def _conn(tries=4):
    """The shared injection connection, re-established if it dies.

    The daemon is killed by every deploy and can refuse or time out under
    load; an exception here aborts a matrix run halfway, which is worse than
    the pause of restarting it."""
    global _inj
    for attempt in range(tries):
        if _inj is not None: return _inj
        try:
            _inj = socket.create_connection(("127.0.0.1", 47777), timeout=5)
            _inj.settimeout(0.8)
            return _inj
        except Exception:
            _inj = None
            try: ensure_testd()
            except Exception: pass
            time.sleep(0.6)
    raise RuntimeError("schwung-testd is not reachable")

INJECT_DROPS = []

def inject(pkt, tries=6):
    """Push one packet, and CHECK THE REPLY.

    INJECT_MIDI answers ERR when the ring is full or a prior producer is
    stranded, and this used to discard the reply — so a dropped packet looked
    exactly like a landed one and the gesture simply did nothing. Several
    measurements were scored against gestures that never happened. Retries,
    and records anything it could not place."""
    s = _conn()
    for attempt in range(tries):
        s.sendall(("INJECT_MIDI %s\n" % pkt).encode())
        rep = b""
        try: rep = s.recv(4096)
        except socket.timeout: pass
        r = rep.decode(errors="replace").strip()
        if r.startswith("OK"): return True
        if not r:                      # no answer: reopen and retry
            try: s.close()
            except Exception: pass
            globals()["_inj"] = None
            s = _conn()
        time.sleep(0.12)
    INJECT_DROPS.append(pkt)
    return False

def seq(*a):
    for x in a:
        if isinstance(x, str) and x.startswith("s") and x[1:].isdigit():
            time.sleep(int(x[1:]) / 1000.0)
        else:
            inject(x)

def tap(press, ms, release):
    inject(press); time.sleep(ms / 1000.0); inject(release)

class Testd:
    """A handle on the SHARED connection, not a second one.

    schwung-testd serves ONE connection at a time, so opening a param
    connection while injection holds its own makes the second one hang until
    it times out — which presented as "the daemon is up but unreachable" and
    aborted several runs. One socket, both jobs: the same arrangement that
    made the first successful scripted p-lock work."""
    _current_slot = None      # the SHARED connection's sticky slot

    def __init__(self, slot=0):
        self.slot = slot
        self._select()

    def _select(self):
        """`SLOT n` is STICKY on the connection, and the connection is shared.
        So creating a handle for another slot silently re-pointed every handle
        that already existed — a scenario that touched track 1 sent every
        later command for track 0 to chain slot 1, which reads as "that slot
        has no FX" rather than as a harness fault."""
        if Testd._current_slot != self.slot:
            s = _conn()
            s.sendall(("SLOT %d\n" % self.slot).encode())
            try: s.recv(4096)
            except socket.timeout: pass
            Testd._current_slot = self.slot

    def cmd(self, line):
        self._select()
        s = _conn()
        s.sendall((line + "\n").encode())
        buf = b""
        try:
            while True:
                b = s.recv(8192)
                if not b: break
                buf += b
                if len(b) < 8192: break
        except socket.timeout:
            pass
        return buf.decode(errors="replace").strip()

    def close(self):
        pass          # the shared connection outlives any one handle


# ---- gestures (from docs/MOVE_UI_MAP.md) --------------------------------
BACK, MENU, SHIFT = "0BB03300", "0BB03200", "0BB03100"
def reset(track_cc=0x2B):
    for _ in range(4): tap("0BB0337F", 90, BACK); time.sleep(0.3)
    tap("0BB0%02X7F" % track_cc, 110, "0BB0%02X00" % track_cc); time.sleep(1.2)
def shift_step(n):
    nn = 0x10 + (n-1)
    seq("0BB0317F","s80","0990%02X77"%nn,"s120","0980%02X00"%nn,"s80","0BB03100")
def step_tap(n):
    nn = 0x10 + (n-1)
    tap("0990%02X7F"%nn, 120, "0980%02X00"%nn)
def new_clip():   shift_step(14)
def double_loop(): shift_step(15)

def plock(td, step, key, value, hold_ms=900):
    """note-on(step) -> SET_PARAM -> note-off(step). The write must land while
    the step is still down or it is an ordinary parameter change."""
    nn = 0x10 + (step-1)
    seq("0990%02X7F" % nn)                 # press and HOLD
    time.sleep(0.35)
    r = td.cmd("SET_PARAM %s %s" % (key, value))
    time.sleep(hold_ms/1000.0)
    seq("0980%02X00" % nn)                 # release
    time.sleep(0.4)
    return r

import mmap as _mmap
def _ctrl():
    f=os.open("/dev/shm/schwung-control", os.O_RDWR)
    return f, _mmap.mmap(f,256)

def ensure_inject():
    """Arm inject_as_hardware, and do it on EVERY run.

    It lives in /dev/shm/schwung-control, which is recreated whenever the shim
    restarts — so every deploy silently resets it to 0, and without it injected
    input never reaches the shim's own scans (shadow_steps_held_mask, the
    long-press tracker). The failure mode is not an error: gestures simply do
    nothing, which reads as "the fix didn't work" and invalidated several
    measurements today before it was noticed."""
    f,m=_ctrl()
    was = m[108]
    m[108] = 1
    m.close(); os.close(f)
    return was


def open_grid(track_cc=0x2B, tries=3):
    """Raise the Schwung grid, and KEEP CHECKING until it is actually up.

    A p-lock needs shadow_display_mode (shim_plock_held_step returns -1
    without it). A single long-press is not reliable when injected -- the hold
    has to outlast the 500 ms threshold AND land while Move is not busy -- so
    this retries and verifies, rather than assuming one press worked and then
    reporting a p-lock failure that was really a setup failure."""
    f,m=_ctrl()
    try:
        for _ in range(tries):
            if m[0] == 1: return True
            tap("0BB0%02X7F" % track_cc, 900, "0BB0%02X00" % track_cc)
            for _ in range(20):
                time.sleep(0.15)
                if m[0] == 1: return True
        return m[0] == 1
    finally:
        m.close(); os.close(f)

def close_grid():
    f,m=_ctrl()
    if m[0] == 1:
        tap("0BB0327F", 90, MENU); time.sleep(1.4)
    ok = (m[0] == 0)
    m.close(); os.close(f)
    return ok

def selected(track, secs=1.6):
    """What the SHIM has decoded as the selected clip on `track`.

    Read from the clip-state readout (`sel[...]`, 1-based, E = an empty slot,
    ? = not known) rather than inferred here, so the harness is checking the
    thing under test and not a second guess at it. Returns an int slot index,
    "E", or None."""
    log = os.path.join(D, "clip_state.log")
    try: os.remove(log)
    except FileNotFoundError: pass
    open(os.path.join(D, "clip_state_on"), "w").close()
    time.sleep(secs)
    os.remove(os.path.join(D, "clip_state_on"))
    time.sleep(0.3)
    last = None
    try:
        for line in open(log):
            m = re.search(r"sel\[([^\]]*)\]", line)
            if m and len(m.group(1)) > track: last = m.group(1)[track]
    except FileNotFoundError:
        return None
    if last is None or last == "?": return None
    if last == "E": return "E"
    return int(last) - 1


# Move's pad mode, as the SHIM labels it (src/host/move_ui_mode_label.h).
# THERE ARE THREE MODES, NOT TWO, and this harness modelled two.
UI_UNKNOWN, UI_SESSION, UI_NOTE, UI_SET_OVERVIEW = 0, 1, 2, 3
UI_NAME = {0: "UNKNOWN", 1: "SESSION", 2: "NOTE", 3: "SET_OVERVIEW"}

def ui_mode(secs=1.4):
    """Move's pad mode as the SHIM sees it: see UI_* above.

    Read back rather than assumed, because Menu CYCLES — so a blind tap puts
    you in a given mode only if you were in the right one to start with.

    THE VALUES ARE NOT A BOOLEAN. This said "1 = Session, 0 = not" and
    ensure_note() waited for 0, which is UNKNOWN; NOTE is 2. So ensure_note()
    could never succeed: it toggled four times, returned False, and every
    caller ignored the return and tapped steps anyway -- in SESSION view,
    where a step button does not add a note. A whole afternoon of "create a
    clip" gestures created nothing, and the silence was read as the code under
    test being broken. 2026-09-18."""
    log = os.path.join(D, "clip_state.log")
    try: os.remove(log)
    except FileNotFoundError: pass
    open(os.path.join(D, "clip_state_on"), "w").close()
    time.sleep(secs)
    os.remove(os.path.join(D, "clip_state_on"))
    time.sleep(0.25)
    last = None
    try:
        for line in open(log):
            m = re.search(r"ui=(\d+)", line)
            if m: last = int(m.group(1))
    except FileNotFoundError:
        return None
    return last


def _ensure_ui(target, tries=6, track_cc=0x2B):
    """Drive Move to `target` and CONFIRM it, or RAISE.

    THE TWO MODES ARE REACHED BY DIFFERENT BUTTONS, which is why cycling one
    of them could not work. Menu (CC 50) puts up SESSION. NOTE is not on the
    other side of that toggle -- the shim infers NOTE from a TRACK press
    (CC 40-43, reversed: 0x2B is Track 1), because a track press is what puts
    an instrument under the pads, and no announcement reports a track
    selection. See src/host/move_ui_mode_label.h.

    Tapping Menu six times to "get to Note view" therefore sat in SESSION
    forever, which is exactly what it did.

    Raising rather than returning False is the point. Every caller of the old
    pair ignored the return value, so a failure to reach the mode became a
    gesture delivered to the WRONG mode -- silently, and the results were then
    scored as if the setup had held. A setup that did not land must stop the
    run, not colour it."""
    seen = []
    for _ in range(tries):
        m = ui_mode()
        seen.append(m)
        if m == target: return True
        if target == UI_NOTE:
            tap("0BB0%02X7F" % track_cc, 110, "0BB0%02X00" % track_cc)
        else:
            tap("0BB0327F", 90, MENU)
        time.sleep(2.2)
    raise RuntimeError("could not reach %s: saw %s" %
                       (UI_NAME[target], [UI_NAME.get(x, x) for x in seen]))

def ensure_session(tries=6): return _ensure_ui(UI_SESSION, tries)
def ensure_note(tries=6, track_cc=0x2B): return _ensure_ui(UI_NOTE, tries, track_cc)

def assert_pads_safe():
    """Refuse to touch pads or steps in SET OVERVIEW.

    In that mode the pads and the steps SWAP THE LOADED SET -- so a stray tap
    does not merely do nothing, it throws away what the user was working on.
    The harness spent an afternoon tapping in modes it had not confirmed; this
    is the guard that makes that impossible rather than unlikely."""
    m = ui_mode()
    if m == UI_SET_OVERVIEW:
        raise RuntimeError("SET OVERVIEW is up -- a pad or step tap here SWAPS "
                           "THE LOADED SET. Refusing.")
    return m


def delete_clip(track, slot, pad0=92, tries=3):
    """Select the clip in Session view and press Delete (CC 119), then CONFIRM.

    Delete alone deletes the CLIP (docs/MOVE_UI_MAP.md 7.4), so the pad press
    that selects it must come first and be released before Delete. The gesture
    does not always land, and an unverified delete leaves the next test picking
    an occupied slot and scoring a skip — which reads as flaky behaviour rather
    than a setup that did not happen. Waits for Song.abl to agree."""
    for _ in range(tries):
        if not slots(track)[slot]: return True
        ensure_session()
        pad = pad0 - 8*track + slot
        tap("0990%02X7F" % pad, 120, "0980%02X00" % pad); time.sleep(1.2)
        tap("0BB0777F", 140, "0BB07700")                # CC 119
        # Move writes the file ~8-12 s later; poll rather than guess.
        for _ in range(9):
            time.sleep(2)
            if not slots(track)[slot]: return True
    return not slots(track)[slot]

def launch(track, slot, pad0=92):
    close_grid()
    tap("0BB0327F", 90, MENU); time.sleep(1.4)
    pad = pad0 - 8*track + slot
    tap("0990%02X7F" % pad, 120, "0980%02X00" % pad); time.sleep(2.5)

def plock_verified(td, step, key, value):
    """A p-lock, with BOTH preconditions checked at the moment they matter:
    the grid is up, and the shim actually sees the step as held. Injection goes
    over td's own connection -- a second connection is refused by the inject
    ring as a stranded producer, which is what silently ate several attempts."""
    if not open_grid(): return "REFUSED: grid did not open (display_mode != 1)"
    f,m=_ctrl()
    td.cmd("INJECT_MIDI 0990%02X77" % (0x10 + step - 1))
    time.sleep(0.45)
    held = m[117]
    if held != step - 1:
        td.cmd("INJECT_MIDI 0980%02X00" % (0x10 + step - 1))
        m.close(); os.close(f)
        return "REFUSED: held_step=%d, wanted %d" % (held, step-1)
    r = td.cmd("SET_PARAM %s %s" % (key, value))
    time.sleep(0.5)
    td.cmd("INJECT_MIDI 0980%02X00" % (0x10 + step - 1))
    time.sleep(1.0)
    m.close(); os.close(f)
    # AND PUT THE GRID BACK DOWN BEFORE ANYONE READS.
    #
    # /schwung-param has ONE request slot and shadow_ui is the other producer
    # on it, so while the grid is up a GET_PARAM is starved and comes back
    # EMPTY -- not an error, just "". A reader that does not know this scores
    # the empty answer as state. The p-lock needs the grid; reading needs it
    # gone; so the gesture owns both halves.
    close_grid()
    return "ok (held_step=%d) %s" % (held, r)

def _read(td, key, tries=14):
    """A param read that distinguishes "served, empty" from "not served".

    /schwung-param has ONE request slot shared with shadow_ui, so a read can
    simply be starved and come back "OK" with no value. That is NOT the same
    fact as the key having no value, and scoring it as state is how this
    harness reported an empty clip row as though the chain had said so.
    Retries, and says plainly when it never got an answer."""
    for _ in range(tries):
        r = td.cmd("GET_PARAM %s" % key)
        body = r.split("\n", 1)[0]
        if body.startswith("OK") and body[2:].strip():
            return r
        time.sleep(0.35)
    return "UNSERVED"

def diag(td): return _read(td, "lanes:diag")
def clip(td):  return _read(td, "lanes:clip")

def slots(track=0):
    d = json.load(open(_song()))
    cs = d["tracks"][track].get("clipSlots", [])
    return [bool(c.get("clip")) for c in cs]

def song_age(): return round(time.time() - os.stat(_song()).st_mtime, 1)

def lanes(td):
    """[(idx, target:param, track, row, n, drv, stale, orph)] from lanes:diag.

    Returns None when the read was NOT SERVED — which is a different fact from
    "this slot has no lanes", and collapsing the two turned a starved read into
    a wrong verdict three separate times in this harness. Callers must treat
    None as a setup failure, never as an empty store."""
    d = diag(td)
    if d == "UNSERVED":
        return None
    out=[]
    for line in d.split("\n"):
        m=re.match(r"(L\d) (\S+) (.*)", line.strip())
        if not m: continue
        g=dict(re.findall(r"(\w+)=([-\d.]+)", m.group(3)))
        out.append((m.group(1), m.group(2), int(g["t"]), int(g["row"]),
                    int(g["n"]), int(g["drv"]), int(g["stale"]), int(g["orph"])))
    return out

_autoarm()   # every import: a deploy resets the flag
# NOT ARMED HERE, deliberately.
#
# This module used to call arm_lanes(True) at import, so merely IMPORTING the
# harness -- to read a param, to check which pad mode Move is in, to look at
# anything at all -- switched the feature on and left it on. An instrument
# that changes the thing it measures is the same class of defect as the mode
# model that could not reach NOTE: the reading is real, and it is a reading of
# a world the instrument created.
#
# It also hides the kill switch's own behaviour. Disarming was tested by
# deleting the flag and reading `lanes:enabled` back through this module --
# which re-armed it between the delete and the read, and reported 1. That
# reads as "the switch cannot be turned off", which would have been a
# serious defect in the one mechanism that exists to turn the feature off
# when it misbehaves. Measured again without the import: it disarms correctly.
#
# A run that wants lanes asks for them: `k.arm_lanes(True)`.
