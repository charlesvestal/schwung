#!/usr/bin/env bash
#
# test-safety-fixes.sh
#
# On-device safety fix verification test suite.
# Runs from Mac, SSHs to Move device to verify binary integrity,
# filesystem structure, and deploys a JS test module for path validation
# and API safety checks.
#
# Usage:
#   ./scripts/test-safety-fixes.sh
#
# Prerequisites:
#   - Move device accessible at move.local
#   - SSH key configured (run ./scripts/install.sh first)
#   - Move Anything deployed on device
#

set -euo pipefail

# ===== Configuration =====

HOSTNAME=move.local
USERNAME=ableton
SSH_OPTS="-o LogLevel=QUIET -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new -n"
SSH_CMD="ssh $SSH_OPTS $USERNAME@$HOSTNAME"
SSH_ROOT="ssh $SSH_OPTS root@$HOSTNAME"
SCP_CMD="scp -o ConnectTimeout=5 -o StrictHostKeyChecking=accept-new"

BASE_DIR="/data/UserData/move-anything"
MODULES_DIR="$BASE_DIR/modules"
RESULTS_FILE="$BASE_DIR/test-safety-results.txt"
TEST_MODULE_ID="test-safety"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
TEST_MODULE_SRC="$REPO_DIR/src/modules/$TEST_MODULE_ID"

# Counters
TOTAL_PASS=0
TOTAL_FAIL=0
TOTAL_SKIP=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

# ===== Helpers =====

pass() {
    TOTAL_PASS=$((TOTAL_PASS + 1))
    printf "  ${GREEN}PASS${NC}: %s\n" "$1"
}

fail() {
    TOTAL_FAIL=$((TOTAL_FAIL + 1))
    printf "  ${RED}FAIL${NC}: %s\n" "$1"
}

skip() {
    TOTAL_SKIP=$((TOTAL_SKIP + 1))
    printf "  ${YELLOW}SKIP${NC}: %s\n" "$1"
}

section() {
    printf "\n${BOLD}${BLUE}=== %s ===${NC}\n" "$1"
}

ssh_run() {
    $SSH_CMD "$1" 2>/dev/null
}

ssh_root_run() {
    $SSH_ROOT "$1" 2>/dev/null
}

# ===== Phase 1: SSH Connectivity =====

phase_ssh() {
    section "Phase 1: SSH Connectivity"

    if $SSH_CMD "echo ok" >/dev/null 2>&1; then
        pass "SSH to $USERNAME@$HOSTNAME"
    else
        fail "SSH to $USERNAME@$HOSTNAME"
        printf "\n${RED}Cannot connect to device. Ensure Move is on and SSH is configured.${NC}\n"
        printf "Run ./scripts/install.sh first to set up SSH keys.\n"
        exit 1
    fi

    if $SSH_ROOT "echo ok" >/dev/null 2>&1; then
        pass "SSH to root@$HOSTNAME"
    else
        skip "SSH to root@$HOSTNAME (some tests will be skipped)"
    fi
}

# ===== Phase 2: Binary Integrity =====

phase_binary() {
    section "Phase 2: Binary Integrity"

    local has_root=false
    $SSH_ROOT "echo ok" >/dev/null 2>&1 && has_root=true

    # Check move-anything binary
    if ssh_run "test -f $BASE_DIR/move-anything"; then
        pass "move-anything binary exists"
    else
        fail "move-anything binary exists"
        return
    fi

    # ELF check on move-anything
    if ssh_run "file $BASE_DIR/move-anything" | grep -q "ELF"; then
        pass "move-anything is valid ELF"
    else
        fail "move-anything is valid ELF"
    fi

    # Check shim
    if ssh_run "test -f $BASE_DIR/move-anything-shim.so"; then
        pass "move-anything-shim.so exists"
    else
        fail "move-anything-shim.so exists"
    fi

    if ssh_run "file $BASE_DIR/move-anything-shim.so" | grep -q "ELF"; then
        pass "shim is valid ELF"
    else
        fail "shim is valid ELF"
    fi

    # Check shadow_ui
    if ssh_run "test -f $BASE_DIR/shadow_ui"; then
        pass "shadow_ui binary exists"
        if ssh_run "file $BASE_DIR/shadow_ui" | grep -q "ELF"; then
            pass "shadow_ui is valid ELF"
        else
            fail "shadow_ui is valid ELF"
        fi
    else
        skip "shadow_ui binary (may not be deployed)"
    fi

    # ldd checks for missing libs (need root for ldd typically)
    if [ "$has_root" = true ]; then
        local missing
        missing=$(ssh_root_run "ldd $BASE_DIR/move-anything 2>&1" | grep "not found" || true)
        if [ -z "$missing" ]; then
            pass "move-anything: no missing libraries"
        else
            fail "move-anything: missing libraries: $missing"
        fi

        missing=$(ssh_root_run "ldd $BASE_DIR/move-anything-shim.so 2>&1" | grep "not found" || true)
        if [ -z "$missing" ]; then
            pass "shim: no missing libraries"
        else
            fail "shim: missing libraries: $missing"
        fi

        # Check libpthread linked (ring buffer atomics fix)
        if ssh_root_run "ldd $BASE_DIR/move-anything-shim.so 2>&1" | grep -q "libpthread"; then
            pass "shim links libpthread"
        else
            # On newer glibc, pthread is merged into libc
            if ssh_root_run "ldd $BASE_DIR/move-anything-shim.so 2>&1" | grep -q "libc.so"; then
                pass "shim links libc (pthread merged)"
            else
                fail "shim links libpthread or libc"
            fi
        fi
    else
        skip "ldd checks (need root)"
        skip "libpthread check (need root)"
    fi
}

# ===== Phase 3: Filesystem Structure =====

phase_filesystem() {
    section "Phase 3: Filesystem Structure"

    # Check key directories
    for dir in "$BASE_DIR" "$MODULES_DIR" "$BASE_DIR/host" "$BASE_DIR/shared"; do
        if ssh_run "test -d $dir"; then
            pass "Directory exists: $dir"
        else
            fail "Directory exists: $dir"
        fi
    done

    # Check built-in modules
    for mod in chain controller store; do
        local mod_json="$MODULES_DIR/$mod/module.json"
        if ssh_run "test -f $mod_json"; then
            pass "$mod module.json exists"
            # Validate JSON has id field
            if ssh_run "cat $mod_json" | grep -q '"id"'; then
                pass "$mod module.json has id field"
            else
                fail "$mod module.json has id field"
            fi
        else
            fail "$mod module.json exists"
        fi
    done

    # Check host menu
    if ssh_run "test -f $BASE_DIR/host/menu_ui.js"; then
        pass "host/menu_ui.js exists"
    else
        fail "host/menu_ui.js exists"
    fi
}

# ===== Phase 4: JS-Level Tests =====

phase_js_tests() {
    section "Phase 4: JS-Level Safety Tests"

    # Verify test module source exists locally
    if [ ! -f "$TEST_MODULE_SRC/module.json" ] || [ ! -f "$TEST_MODULE_SRC/ui.js" ]; then
        fail "Test module source files not found at $TEST_MODULE_SRC"
        return
    fi
    pass "Test module source files exist"

    # Deploy test module to device
    printf "  Deploying test module to device...\n"
    ssh_run "mkdir -p $MODULES_DIR/$TEST_MODULE_ID" || true
    $SCP_CMD "$TEST_MODULE_SRC/module.json" "$USERNAME@$HOSTNAME:$MODULES_DIR/$TEST_MODULE_ID/" >/dev/null 2>&1
    $SCP_CMD "$TEST_MODULE_SRC/ui.js" "$USERNAME@$HOSTNAME:$MODULES_DIR/$TEST_MODULE_ID/" >/dev/null 2>&1

    if ssh_run "test -f $MODULES_DIR/$TEST_MODULE_ID/ui.js"; then
        pass "Test module deployed"
    else
        fail "Test module deployed"
        return
    fi

    # Remove any old results file
    ssh_run "rm -f $RESULTS_FILE" || true

    # Restart Move so it picks up the new module
    printf "  Restarting Move service...\n"
    local has_root=false
    $SSH_ROOT "echo ok" >/dev/null 2>&1 && has_root=true

    if [ "$has_root" = true ]; then
        ssh_root_run "/etc/init.d/move stop >/dev/null 2>&1 || true"
        ssh_root_run "for name in MoveOriginal Move move-anything shadow_ui; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids 2>/dev/null || true; fi; done"
        ssh_root_run "rm -f /dev/shm/move-shadow-*"
        sleep 1
        ssh_root_run "/etc/init.d/move start >/dev/null 2>&1"
        pass "Move service restarted"
    else
        printf "  ${YELLOW}Cannot restart Move without root. Please restart Move manually.${NC}\n"
        skip "Move restart (no root access)"
    fi

    # Wait for Move to boot
    printf "  Waiting for Move to boot (10s)...\n"
    sleep 10

    # Prompt user to load the test module
    printf "\n${BOLD}${YELLOW}ACTION REQUIRED:${NC}\n"
    printf "  1. On your Move, open Move Anything menu\n"
    printf "  2. Navigate to Utility category\n"
    printf "  3. Load '${BOLD}Safety Tests${NC}' module\n"
    printf "  4. Tests will run automatically\n"
    printf "\n  Waiting for results (timeout: 120s)...\n"

    # Poll for results file
    local timeout=120
    local elapsed=0
    local found=false
    while [ $elapsed -lt $timeout ]; do
        if ssh_run "test -f $RESULTS_FILE" 2>/dev/null; then
            found=true
            break
        fi
        sleep 3
        elapsed=$((elapsed + 3))
        # Show progress every 15 seconds
        if [ $((elapsed % 15)) -eq 0 ]; then
            printf "  ... %ds elapsed\n" "$elapsed"
        fi
    done

    if [ "$found" = false ]; then
        fail "Results file appeared within ${timeout}s"
        printf "  ${YELLOW}Tip: Make sure you loaded 'Safety Tests' from the Move menu.${NC}\n"
        return
    fi

    pass "Results file received"

    # Read and parse results
    printf "\n  ${BOLD}On-Device Test Results:${NC}\n"
    local results
    results=$(ssh_run "cat $RESULTS_FILE")
    printf "%s\n" "$results" | while IFS= read -r line; do
        printf "    %s\n" "$line"
    done

    # Count passes and failures from device
    local js_pass js_fail
    js_pass=$(echo "$results" | grep -c "^PASS:" || true)
    js_fail=$(echo "$results" | grep -c "^FAIL:" || true)

    TOTAL_PASS=$((TOTAL_PASS + js_pass))
    TOTAL_FAIL=$((TOTAL_FAIL + js_fail))

    if [ "$js_fail" -eq 0 ]; then
        pass "All JS-level tests passed ($js_pass tests)"
    else
        fail "$js_fail JS-level tests failed (of $((js_pass + js_fail)))"
    fi
}

# ===== Phase 5: Cleanup =====

phase_cleanup() {
    section "Phase 5: Cleanup"

    # Remove test module from device
    ssh_run "rm -rf $MODULES_DIR/$TEST_MODULE_ID" || true
    ssh_run "rm -f $RESULTS_FILE" || true
    ssh_run "rm -rf $BASE_DIR/test-safety-tmp" || true

    if ssh_run "test ! -d $MODULES_DIR/$TEST_MODULE_ID"; then
        pass "Test module removed"
    else
        fail "Test module removed"
    fi

    # Restart Move to restore normal operation
    local has_root=false
    $SSH_ROOT "echo ok" >/dev/null 2>&1 && has_root=true

    if [ "$has_root" = true ]; then
        printf "  Restarting Move service...\n"
        ssh_root_run "/etc/init.d/move stop >/dev/null 2>&1 || true"
        ssh_root_run "for name in MoveOriginal Move move-anything shadow_ui; do pids=\$(pidof \$name 2>/dev/null || true); if [ -n \"\$pids\" ]; then kill -9 \$pids 2>/dev/null || true; fi; done"
        ssh_root_run "rm -f /dev/shm/move-shadow-*"
        sleep 1
        ssh_root_run "/etc/init.d/move start >/dev/null 2>&1"
        pass "Move restarted normally"
    else
        printf "  ${YELLOW}Please restart Move manually to restore normal operation.${NC}\n"
        skip "Move restart (no root)"
    fi
}

# ===== Phase 6: Summary =====

phase_summary() {
    section "Summary"

    local total=$((TOTAL_PASS + TOTAL_FAIL + TOTAL_SKIP))
    printf "\n"
    printf "  ${GREEN}Passed${NC}:  %d\n" "$TOTAL_PASS"
    printf "  ${RED}Failed${NC}:  %d\n" "$TOTAL_FAIL"
    printf "  ${YELLOW}Skipped${NC}: %d\n" "$TOTAL_SKIP"
    printf "  Total:   %d\n" "$total"
    printf "\n"

    if [ "$TOTAL_FAIL" -eq 0 ]; then
        printf "  ${GREEN}${BOLD}All tests passed!${NC}\n\n"
        return 0
    else
        printf "  ${RED}${BOLD}Some tests failed.${NC}\n\n"
        return 1
    fi
}

# ===== Main =====

main() {
    printf "\n${BOLD}Move Anything - Safety Fix Verification${NC}\n"
    printf "========================================\n"

    phase_ssh
    phase_binary
    phase_filesystem
    phase_js_tests
    phase_cleanup
    phase_summary
}

main
