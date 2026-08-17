#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# run_command_background() fires fire-and-forget helpers (curl for catalog
# checks and manual refresh) from long-lived processes — notably shadow_ui,
# which never wait()s. A single fork therefore leaks one zombie per call:
# setsid() detaches the controlling terminal but does not change the parent, so
# the exec'd child stays a child. Observed on hardware as `curl <defunct>`
# accumulating at roughly one per 90s until the PID table filled with them.
#
# The fix is the standard double-fork: the intermediate child _exit()s right
# after forking, orphaning the grandchild onto init (which reaps it), and the
# parent waitpid()s the intermediate child. This pins that shape so a future
# edit can't quietly collapse it back to a single fork.
#
# Uses grep rather than rg so the test runs anywhere, including hosts without
# ripgrep installed.

common="src/host/js_host_common.c"

if [ ! -f "$common" ]; then
  echo "FAIL: $common does not exist" >&2
  exit 1
fi

# Extract just the body of run_command_background (up to the closing brace at
# column 0) so these assertions can't be satisfied by unrelated code elsewhere.
body="$(awk '/^static void run_command_background\(/{f=1} f{print} f&&/^}$/{exit}' "$common")"

if [ -z "$body" ]; then
  echo "FAIL: could not locate run_command_background() in $common" >&2
  exit 1
fi

fork_count="$(printf '%s\n' "$body" | grep -c 'fork()' || true)"
if [ "$fork_count" -lt 2 ]; then
  echo "FAIL: run_command_background() must double-fork so the exec'd command" >&2
  echo "      is reparented to init; found $fork_count fork() call(s)." >&2
  echo "      A single fork leaks a zombie per call (see curl <defunct>)." >&2
  exit 1
fi

if ! printf '%s\n' "$body" | grep -q 'waitpid('; then
  echo "FAIL: run_command_background() must waitpid() the intermediate child," >&2
  echo "      otherwise that child is itself leaked as a zombie." >&2
  exit 1
fi

if ! printf '%s\n' "$body" | grep -q '_exit(0)'; then
  echo "FAIL: run_command_background()'s intermediate child must _exit(0)" >&2
  echo "      immediately so the grandchild is orphaned onto init." >&2
  exit 1
fi

echo "PASS: run_command_background double-forks and reaps its intermediate child"
