#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# install.sh asserted the boot selector's three payload files unconditionally,
# which made it unable to install ANY release older than the selector: a
# rollback to v1.2.0 died with "Payload missing: schwung-entry.sh". Worse, it
# died AFTER extraction, leaving a half-installed tree whose /opt/move/Move was
# still the newer selector with no boot_target_lib.sh under /data — the
# lib-missing fallback, i.e. Schwung up with no sidecars and no manager on
# :7700. Rolling back is what you want an installer for when a release is bad.
#
# The rule is all-or-nothing: none = an older payload (install it), all = a
# selector payload, SOME = corrupt (fail, and fail before extraction).
#
# The function is LIFTED from install.sh, not restated here: a copy would drift
# from the shipped one and go on passing.

fails=0
say_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
say_pass() { echo "PASS: $*"; }

# awk, not `grep -n | head -1`: head exits after the first line, grep takes
# SIGPIPE and pipefail turns that into a failed assignment.
first_line_matching() {
    awk -v pat="$1" '!n && index($0, pat) == 1 { n = NR } END { if (n) print n }' "$2"
}
start=$(first_line_matching 'BOOT_SELECTOR_FILES=' scripts/install.sh)
end=$(first_line_matching '# ---- end BOOT_SELECTOR_PAYLOAD_RULE' scripts/install.sh)
if [ -z "$start" ] || [ -z "$end" ] || [ "$start" -ge "$end" ]; then
    echo "FAIL: could not slice BOOT_SELECTOR_PAYLOAD_RULE out of scripts/install.sh"
    exit 1
fi
eval "$(sed -n "${start},$((end - 1))p" scripts/install.sh)"

check() { # check <expected> <label> <present...>
    expected="$1"; label="$2"; shift 2
    got=$(boot_selector_payload_state "$@")
    if [ "$got" = "$expected" ]; then
        say_pass "$label -> $got"
    else
        say_fail "$label -> [$got], expected [$expected]"
    fi
}

# ---- 1. a current payload: all three --------------------------------------
check full "all three present" schwung-entry.sh host/boot_target_lib.sh bin/boot-select

# ---- 2. a pre-selector payload: none of them ------------------------------
# This is the v1.2.0 tarball, and it MUST be installable.
check none "pre-selector payload (none present)"

# ---- 3. corrupt payloads: some but not all -------------------------------
check "partial host/boot_target_lib.sh bin/boot-select" \
    "only the entry script" schwung-entry.sh
check "partial bin/boot-select" \
    "entry + lib, no boot-select" schwung-entry.sh host/boot_target_lib.sh
check "partial schwung-entry.sh" \
    "lib + boot-select, no entry script" host/boot_target_lib.sh bin/boot-select

# ---- 4. order does not matter --------------------------------------------
check full "all three, reversed" bin/boot-select host/boot_target_lib.sh schwung-entry.sh

# ---- 5. a near-miss name is not a match ----------------------------------
# Substring matching would let bin/boot-selector or schwung-entry.sh.bak stand
# in for the real file; the rule pads with spaces and matches whole words.
check "partial bin/boot-select" "bin/boot-selector is not bin/boot-select" \
    schwung-entry.sh host/boot_target_lib.sh bin/boot-selector

# ---- 6. the shipped script actually USES the rule -------------------------
# The function existing is not the fix; the three call sites are. Assert the
# post-extraction asserts and the entry chmod are gated on it, and that the
# tarball check happens BEFORE extraction (that is the half-install half of
# the bug).
if awk '/^tarball_present=/ { t = NR } /tar -xzo?vf|tar -xzof|tar -xzvof/ { if (!x) x = NR } END { exit !(t && x && t < x) }' scripts/install.sh; then
    say_pass "the tarball is judged before it is extracted"
else
    say_fail "the boot-selector payload check does not precede extraction"
fi

if grep -q 'if \[ "\$boot_selector_state" = "full" \]; then' scripts/install.sh; then
    say_pass "call sites are gated on the computed state"
else
    say_fail "no call site gates on boot_selector_state"
fi

# An unguarded assert would re-break rollback. There must be none outside a gate.
# index(), not a regex: awk eats the backslash in \$ inside a regex literal,
# turning it into an end-anchor, and the gate then never matches — the check
# passes by never seeing the gate rather than by the gate being there.
if awk '
    /^[ \t]*#/ { next }          # the rule\047s own comment quotes the old message
    index($0, "boot_selector_state") && index($0, "full") { gate = 1 }
    $0 == "fi" { gate = 0 }
    index($0, "Payload missing: schwung-entry") && !gate { bad = 1 }
    index($0, "Payload missing: host/boot_target_lib") && !gate { bad = 1 }
    index($0, "Payload missing: bin/boot-select") && !gate { bad = 1 }
    END { exit bad }
' scripts/install.sh; then
    say_pass "no ungated boot-selector payload assert remains"
else
    say_fail "an ungated 'Payload missing' assert for the selector trio is back"
fi

if [ "$fails" -eq 0 ]; then
    echo "PASS: all install.sh boot-selector payload checks passed"
    exit 0
fi
echo "$fails check(s) failed"
exit 1
