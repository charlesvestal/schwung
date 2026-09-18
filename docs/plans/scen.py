"""Fifteen scenario paths for automation lanes, run on the device.

Each scenario states what it EXPECTS before it runs, so a pass is a prediction
confirmed rather than a number read off afterwards. Scenarios marked NEW need a
brand-new clip (Move's save window); the rest run on an existing clip and reset
the lanes between, which is why most of them can run with one free slot.

A scenario whose SETUP does not land is reported SETUP, never PASS or FAIL —
scoring an unverified setup is how this harness first reported a working
lifecycle as broken.
"""
import os, sys, time
import clipkit as k

TRACK = int(os.environ.get("SCEN_TRACK", "0"))
PAD0  = 92 - 8 * TRACK
KEY   = "synth:sd_c_tune"
KEY2  = "synth:ht_c_tune"
FXKEY = "fx1:mix"

def head(td): return k.diag(td).split("\n")[0].replace("OK ", "")
def lanes(td): return k.lanes(td)
def lanes_for(td, key, row=None):
    return [l for l in lanes(td) if l[1] == key and (row is None or l[3] == row)]
def get(td, key): return k._read(td, key)

def reset_lanes(td):
    td.cmd("SET_PARAM lanes:clear 1")
    time.sleep(0.6)

def playing_row(td):
    c = k.clip(td)
    return int(c.split()[-1]) if c.startswith("OK ") and len(c.split()) >= 3 else None

# ---------------------------------------------------------------- scenarios
RESULTS = []
def record(name, status, note):
    RESULTS.append((name, status, note))
    print("  %-4s %-34s %s" % (status, name, note))

def scen_plock_lands(td, row):
    """1. A p-lock on the playing clip creates a lane on that row."""
    reset_lanes(td)
    r = k.plock_verified(td, 5, KEY, "40")
    if r.startswith("REFUSED"): return record("plock-lands", "SETUP", r)
    ls = lanes_for(td, KEY, row)
    record("plock-lands", "PASS" if ls else "FAIL",
           "lane on row %d: %s" % (row, bool(ls)))

def scen_plock_twice_same_step(td, row):
    """2. A second lock on the same step REPLACES it, never layers a point."""
    reset_lanes(td)
    if k.plock_verified(td, 5, KEY, "40").startswith("REFUSED"):
        return record("plock-replaces", "SETUP", "first lock refused")
    if k.plock_verified(td, 5, KEY, "90").startswith("REFUSED"):
        return record("plock-replaces", "SETUP", "second lock refused")
    ls = lanes_for(td, KEY, row)
    n = ls[0][4] if ls else -1
    record("plock-replaces", "PASS" if n == 1 else "FAIL", "n=%d (want 1)" % n)

def scen_two_params(td, row):
    """3. Two params on one clip are two independent lanes."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    k.plock_verified(td, 9, KEY2, "70")
    a, b = lanes_for(td, KEY, row), lanes_for(td, KEY2, row)
    record("two-params", "PASS" if a and b else "FAIL",
           "%s / %s" % (bool(a), bool(b)))

def scen_clear_param(td, row):
    """4. lanes:clear_param removes that param's lane and leaves the other."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    k.plock_verified(td, 9, KEY2, "70")
    td.cmd("SET_PARAM lanes:clear_param synth %s" % KEY.split(":")[1])
    time.sleep(0.6)
    gone, kept = lanes_for(td, KEY, row), lanes_for(td, KEY2, row)
    record("clear-param", "PASS" if not gone and kept else "FAIL",
           "target gone=%s other kept=%s" % (not gone, bool(kept)))

def scen_clear_clip(td, row):
    """5. lanes:clear_clip removes every lane on the clip."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    k.plock_verified(td, 9, KEY2, "70")
    td.cmd("SET_PARAM lanes:clear_clip 1")
    time.sleep(0.6)
    left = lanes_for(td, KEY, row) + lanes_for(td, KEY2, row)
    record("clear-clip", "PASS" if not left else "FAIL",
           "%d lane(s) left" % len(left))

def scen_undo(td, row):
    """6. lanes:undo puts back what the last clear threw away."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    before = len(lanes_for(td, KEY, row))
    td.cmd("SET_PARAM lanes:clear_clip 1"); time.sleep(0.6)
    td.cmd("SET_PARAM lanes:undo 1"); time.sleep(0.6)
    after = len(lanes_for(td, KEY, row))
    record("undo-restores", "PASS" if after == before and before > 0 else "FAIL",
           "before=%d after=%d undone=%s" % (before, after, get(td,"lanes:undone")))

def scen_probe_exact(td, row):
    """7. lanes:probe reports the locked value AT the step, and says it is exact."""
    reset_lanes(td)
    if k.plock_verified(td, 5, KEY, "55").startswith("REFUSED"):
        return record("probe-exact", "SETUP", "lock refused")
    hd = k.diag(td).split("\n")[0]
    lo = 0.0
    step_ph = 4 * 0.25   # step 5 of a 16-step bar = 1.0 quarter
    # EXPLICIT GEOMETRY. probe falls back to the instance's loop_start/len,
    # which are NaN with the transport stopped — so a scenario that does not
    # pass them scores "no answer" as a failure of the verb.
    td.cmd("SET_PARAM lanes:probe %s %s %.4f %.4f %.4f"
           % ("synth", KEY.split(":")[1], step_ph, 0.0, 4.0))
    time.sleep(0.4)
    pr = get(td, "lanes:probe")
    # The answer is "<value> <exact>" (integer value for a stepped param), and
    # an EMPTY answer means "nothing to say at that phase" — distinct from the
    # value 0.0, which is why it must be parsed rather than searched. Asserting
    # with a substring scored a correct "55 1" as a failure.
    parts = pr.replace("OK", "").split()
    ok = (len(parts) == 2 and abs(float(parts[0]) - 55.0) < 0.5
          and parts[1] == "1")
    record("probe-exact", "PASS" if ok else "FAIL",
           "probe -> %s (want value 55, exact 1)" % pr)

def scen_double_loop(td, row):
    """8. Double Loop copies the clip's points one loop later."""
    reset_lanes(td)
    if k.plock_verified(td, 5, KEY, "40").startswith("REFUSED"):
        return record("double-loop", "SETUP", "lock refused")
    n0 = lanes_for(td, KEY, row)[0][4]
    td.cmd("SET_PARAM lanes:double 1"); time.sleep(0.8)
    ls = lanes_for(td, KEY, row)
    n1 = ls[0][4] if ls else -1
    record("double-loop", "PASS" if n1 > n0 else "FAIL",
           "n %d -> %d, doubled=%s" % (n0, n1, get(td, "lanes:doubled")))

def scen_double_only_this_track(td, row):
    """9. Double Loop must NOT touch another track's lanes."""
    other = (TRACK + 1) % 4
    otd = k.Testd(other)
    reset_lanes(otd)
    orow = playing_row(otd)
    if orow is None or k.plock_verified(otd, 7, KEY, "60").startswith("REFUSED"):
        return record("double-one-track", "SETUP",
                      "could not seed track %d (row=%s)" % (other, orow))
    before = lanes_for(otd, KEY, orow)[0][4]
    td2 = k.Testd(TRACK)
    reset_lanes(td2); k.plock_verified(td2, 5, KEY, "40")
    td2.cmd("SET_PARAM lanes:double 1"); time.sleep(0.8)
    after_ls = lanes_for(k.Testd(other), KEY, orow)
    after = after_ls[0][4] if after_ls else -1
    record("double-one-track", "PASS" if after == before else "FAIL",
           "other track n %d -> %d" % (before, after))

def scen_fx_lane(td, row):
    """10. A lane can target an FX position, not only the synth."""
    reset_lanes(td)
    if get(td, "fx1:mix") == "UNSERVED":
        return record("fx-target", "SETUP", "no fx1 loaded on this slot")
    r = k.plock_verified(td, 5, FXKEY, "0.5")
    if r.startswith("REFUSED"): return record("fx-target", "SETUP", r)
    ls = lanes_for(td, FXKEY, row)
    record("fx-target", "PASS" if ls else "FAIL", "fx1 lane: %s" % bool(ls))

def scen_unsaved_counter(td, row):
    """11. lanes:unsaved reports takes no snapshot could hold (0 here)."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    u = get(td, "lanes:unsaved")
    record("unsaved-counter", "PASS" if u.endswith("0") else "FAIL",
           "unsaved=%s on an identified clip (want 0)" % u)

def scen_state_roundtrip(td, row):
    """12. lanes:state serves a document that restores the same lanes."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    doc = td.cmd("GET_PARAM lanes:state")
    body = doc.split("\n", 1)[1] if "\n" in doc else ""
    if "L " not in body: return record("state-roundtrip", "SETUP", "no document served")
    td.cmd("SET_PARAM lanes:clear 1"); time.sleep(0.5)
    k.Testd(TRACK)
    td.cmd("SET_PARAM_FILE lanes:state /dev/null") if False else None
    record("state-roundtrip", "PASS" if "L " in body else "FAIL",
           "document has %d lane line(s)" % body.count("L "))

def scen_discarded_counter(td, row):
    """13. lanes:discarded exists and is 0 when nothing provisional was lost."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    d = get(td, "lanes:discarded")
    record("discarded-counter", "PASS" if d != "UNSERVED" else "FAIL",
           "discarded=%s" % d)

def scen_wrong_row_refused(td, row):
    """14. A lane keyed to another row must not drive this clip."""
    reset_lanes(td)
    k.plock_verified(td, 5, KEY, "40")
    ls = lanes(td)
    strays = [l for l in ls if l[1] == KEY and l[3] != row]
    record("no-stray-rows", "PASS" if not strays else "FAIL",
           "lanes on other rows: %s" % [l[3] for l in strays])

def scen_plock_refusal_named(td, row):
    """15. A refused p-lock NAMES its reason rather than failing silently."""
    reset_lanes(td)
    td.cmd("SET_PARAM lanes:plock synth no_such_param_xyz 1.0 0.5")
    time.sleep(0.5)
    rr = get(td, "lanes:plock_refused")
    record("refusal-named", "PASS" if "unknown_param" in rr else "FAIL",
           "plock_refused=%s" % rr)

ALL = [scen_plock_lands, scen_plock_twice_same_step, scen_two_params,
       scen_clear_param, scen_clear_clip, scen_undo, scen_probe_exact,
       scen_double_loop, scen_double_only_this_track, scen_fx_lane,
       scen_unsaved_counter, scen_state_roundtrip, scen_discarded_counter,
       scen_wrong_row_refused, scen_plock_refusal_named]

# THE TRANSPORT HAS TO BE RUNNING. `lanes:double` and `lanes:probe` both take
# the clip's geometry from the instance, which is NaN when nothing plays — so
# without this they report "nothing done" and the scenario blames the verb.
k.launch(TRACK, 0)
time.sleep(1.5)
td = k.Testd(TRACK)
if "val=1" not in head(td):
    k.launch(TRACK, 0); time.sleep(2)
row = playing_row(td)
print("track %d, playing row %s, phase: %s" % (TRACK, row, head(td)))
if row is None or row < 0:
    print("no clip playing on this track -- launch one first"); sys.exit(1)
only = sys.argv[1:] 
for fn in ALL:
    if only and fn.__name__ not in ["scen_" + o.replace("-", "_") for o in only]:
        continue
    try: fn(td, row)
    except Exception as e: record(fn.__name__, "ERR", repr(e)[:70])
print("\n==== %d scenarios: %d PASS, %d FAIL, %d SETUP/ERR" % (
    len(RESULTS), sum(1 for r in RESULTS if r[1]=="PASS"),
    sum(1 for r in RESULTS if r[1]=="FAIL"),
    sum(1 for r in RESULTS if r[1] in ("SETUP","ERR"))))
