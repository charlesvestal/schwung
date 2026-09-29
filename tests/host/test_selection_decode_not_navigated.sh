#!/usr/bin/env bash
# THE SELECTED-CLIP DECODE IS A DIAGNOSTIC, AND THE RESOLVER MAY NOT READ IT.
#
# clip_state decodes which clip is selected from the session pads' base
# colour. It is measured, tested and correct, and it is the wrong input for
# deciding a lane's row for two reasons that are not going to change:
#
#   * it only updates in SESSION view, while p-locks are made in NOTE view,
#     so its answer is stale exactly when a writer wants it;
#   * its output has twice re-introduced wrong-clip contamination -- once
#     naming a row directly, once used only negatively -- and each time the
#     bug was silent and permanent.
#
# The rule it keeps losing to: a clip that cannot be identified from the file
# is the PENDING placeholder, never a guess at which row it is.
#
# Same instrument, and the same reason, as the pin that `synth:last_note` is
# never read for navigation: an answer that is available and tempting gets
# navigated on eventually, and the resulting defect looks like the lane
# feature being broken rather than like a lookup nobody should have made.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 - <<'PY'
import re, sys

# Comments are stripped before matching: this file DISCUSSES the decode at
# length, and a pin that reads prose is measuring the prose. That mistake has
# already been made once in this test suite.
def code_only(path):
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"\S", " ", m.group(0)), text, flags=re.S)
    text = re.sub(r"//[^\n]*", lambda m: " " * len(m.group(0)), text)
    return text

offenders = []
for path in ("src/host/shadow_chain_mgmt.c",
             "src/modules/chain/dsp/chain_lanes.c"):
    code = code_only(path)
    for i, line in enumerate(code.split("\n"), 1):
        if re.search(r"\bclip_state_selected_slot\b", line) or \
           re.search(r"\bCLIP_SEL_EMPTY\b", line) or \
           re.search(r"\bselected_slot\b", line):
            offenders.append("%s:%d: %s" % (path, i, line.strip()))

if offenders:
    print("FAIL: the selected-clip decode is being read where a lane's row is decided.")
    print("      It only updates in Session view, while p-locks happen in Note view,")
    print("      and consuming it has twice bound one clip's automation to another.")
    print("      An unidentifiable clip is the PENDING placeholder, not a guess:")
    for o in offenders:
        print("        " + o)
    sys.exit(1)

# The decode must still EXIST as a diagnostic -- this pin is about who reads
# it, not about deleting it. If it is gone, this test is measuring nothing.
if "clip_state_selected_slot" not in open("src/host/clip_state.c").read():
    sys.exit("FAIL: clip_state_selected_slot is gone, so this pin now guards nothing -- "
             "delete the pin with it, or restore the diagnostic")

print("PASS: the selected-clip decode stays a diagnostic; no lane row is decided from it")
PY
