#!/usr/bin/env python3
"""One command to describe what the lanes feature is doing right now.

Run it on the device when something looks wrong, and hand the output over.
It answers the questions that otherwise take a round of back-and-forth: is the
feature even armed, which clip does each slot think it is on, what does the
store hold, and what did the shim say about the last blind window.

IT DOES NOT ARM ANYTHING and does not import the test harness, which arms
lanes as a side effect of being imported. A reporting tool that changes the
state it reports is worse than no tool -- that mistake cost a false "the kill
switch cannot be turned off" once already.

    ssh ableton@move.local 'python3 /data/UserData/schwung/lanes_report.py'
"""
import json, os, socket, subprocess, sys, time

D = "/data/UserData/schwung"

def testd(cmds):
    """Talk to schwung-testd over its own socket. Raw, so importing nothing."""
    try:
        s = socket.create_connection(("127.0.0.1", 47777), timeout=5)
    except Exception as e:
        return {c: "(testd not reachable: %s)" % e for c in cmds}
    s.settimeout(2.0)
    out = {}
    def one(c):
        s.sendall((c + "\n").encode())
        try:
            return s.recv(65536).decode(errors="replace").strip()
        except socket.timeout:
            return "(timeout)"
    for slot, cmd in cmds:
        one("SLOT %d" % slot)
        # The param channel has ONE request slot shared with the UI, so a read
        # can simply be starved. An empty answer is NOT the same fact as "no
        # value"; retry before reporting nothing.
        r = "(unserved)"
        for _ in range(8):
            r = one(cmd)
            if r.startswith("OK") and r[2:].strip():
                break
            time.sleep(0.3)
        out[(slot, cmd)] = r
    s.close()
    return out

def main():
    print("=" * 70)
    print("SCHWUNG LANES REPORT   %s" % time.strftime("%Y-%m-%d %H:%M:%S"))
    print("=" * 70)

    armed = os.path.exists(os.path.join(D, "lanes_on"))
    print("\nARMED: %s   (%s)" % (
        "yes" if armed else "NO",
        "touch %s/lanes_on to arm" % D if not armed else "rm %s/lanes_on to disarm" % D))
    print("logging: %s" % ("on" if os.path.exists(os.path.join(D, "debug_log_on"))
                           else "OFF -- touch %s/debug_log_on for the lines below" % D))

    keys = ["lanes:enabled", "lanes:clip", "lanes:pending", "lanes:armed",
            "lanes:recording", "lanes:plock_refused", "lanes:diag"]
    cmds = [(s, "GET_PARAM " + k) for s in range(4) for k in keys]
    ans = testd(cmds)
    for slot in range(4):
        head = ans.get((slot, "GET_PARAM lanes:clip"), "")
        diag = ans.get((slot, "GET_PARAM lanes:diag"), "")
        if "(not reachable" in head:
            print("\n%s" % head); break
        # A slot with no chain answers nothing useful; do not print four of them.
        if head in ("(unserved)", "OK", "") and "L0" not in diag:
            continue
        print("\n--- SLOT %d ---" % (slot + 1))
        for k in keys[:-1]:
            print("  %-22s %s" % (k, ans.get((slot, "GET_PARAM " + k), "").replace("OK ", "")))
        for line in diag.replace("OK ", "").split("\n"):
            print("  diag  %s" % line)

    # What Move itself has on disk, which is what a lane is keyed against.
    try:
        uuid = open(os.path.join(D, "active_set.txt")).read().split("\n")[0].strip()
        base = "/data/UserData/UserLibrary/Sets/" + uuid
        song = None
        for root, _, files in os.walk(base):
            if "Song.abl" in files:
                song = os.path.join(root, "Song.abl"); break
        if song:
            age = time.time() - os.stat(song).st_mtime
            d = json.load(open(song))
            print("\n--- Move's song (%s) ---" % os.path.basename(os.path.dirname(song)))
            print("  written %.0fs ago  (a clip made in the last ~20s is NOT in here yet)" % age)
            for t in range(4):
                row = []
                for i, cs in enumerate(d["tracks"][t].get("clipSlots", [])):
                    c = cs.get("clip")
                    if not c: continue
                    lo = c.get("region", {}).get("loop", {})
                    row.append("r%d(%.0fq%s)" % (i, lo.get("end", 0) - lo.get("start", 0),
                                                 ",playing" if c.get("isPlaying") else ""))
                if row: print("  track %d: %s" % (t + 1, " ".join(row)))
    except Exception as e:
        print("\n(could not read the song: %s)" % e)

    log = os.path.join(D, "debug.log")
    if os.path.exists(log):
        want = ("lane-blind", "lane-row", "param-slow", "lanes:")
        try:
            tail = subprocess.run(["tail", "-n", "400", log], capture_output=True,
                                  text=True).stdout.split("\n")
            hits = [l for l in tail if any(w in l for w in want)][-12:]
            if hits:
                print("\n--- recent shim lines ---")
                for l in hits: print("  " + l.strip()[:150])
        except Exception:
            pass
    print("\n" + "=" * 70)

main()
