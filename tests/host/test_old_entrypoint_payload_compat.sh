#!/usr/bin/env bash
set -uo pipefail

cd "$(dirname "$0")/../.."

# A never-blessed device (heal never made root-owned + setuid -- see
# repair_status.go and the web-update-shim-bootstrap-gap memory) cannot be
# fixed over the web: it has no root foothold, so a web update can refresh
# its /data payload but /opt/move/Move stays whatever entrypoint script it
# already has, forever, until someone runs install.sh/the GUI installer by
# hand. Charles: "i dont want to brick never blessed devices." That means
# the OLD pre-split entrypoint (vendored below, from before the boot
# selector split it into schwung-entry.sh + host/boot_target_lib.sh +
# bin/boot-select) is still live on those devices and hard-references
# payload files by path. Every payload we ship from now on must keep
# producing those files, or a never-blessed device's next web update
# silently ships it a payload its own boot script cannot run.
#
# This test derives the old entrypoint's payload references from a vendored
# copy of the script itself (never hand-restated), classifies them as HARD
# (dereferenced with no existence guard -- a miss either crashes the boot
# path or silently degrades it to bare MoveOriginal) or GUARDED-OPTIONAL
# (wrapped in [ -f ]/[ -x ] -- a miss just skips that feature), and then
# statically greps scripts/build.sh + scripts/package.sh to confirm each one
# is still built and still packaged. No Docker run: this is a static pin,
# not a build.

fails=0
say_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
say_pass() { echo "PASS: $*"; }

# Overridable so a throwaway copy of the scripts can be pointed at for a
# mutation proof, without ever touching the checked-in repo.
BUILD_SH="${BUILD_SH:-scripts/build.sh}"
PACKAGE_SH="${PACKAGE_SH:-scripts/package.sh}"
BUILD_DIR="${BUILD_DIR:-build}"

for f in "$BUILD_SH" "$PACKAGE_SH"; do
    if [ ! -f "$f" ]; then
        echo "FAIL: $f not found -- cannot run this test"
        exit 1
    fi
done

# awk, not grep|head: a single-pass scan with no early exit on the producer
# side, so pipefail cannot turn a short match list into a SIGPIPE failure
# (see tests_sigpipe_from_early_exit memory).
contains() {
    # $1 = file, $2 = literal substring (matched with index(), not a regex,
    # so a "." or "/" in a path is never accidentally a wildcard)
    awk -v pat="$2" 'index($0, pat) { found = 1 } END { exit !found }' "$1" 2>/dev/null
}

# ---- 1. the old entrypoint, vendored verbatim (git show e1bdf967:src/shim-entrypoint.sh) ----
old_entrypoint=$(cat <<'OLD_ENTRYPOINT_EOF'
#!/usr/bin/env bash

# === Migration from move-anything → schwung ===
# When upgrading from 0.7.x via the Module Store, files land in
# /data/UserData/move-anything/ but with new schwung binary names.
# Detect this and migrate before proceeding.
SCHWUNG_DIR="/data/UserData/schwung"
OLD_DIR="/data/UserData/move-anything"

if [ ! -d "$SCHWUNG_DIR" ] && [ -d "$OLD_DIR" ] && [ ! -L "$OLD_DIR" ]; then
    # Old directory exists, new one doesn't — need to migrate
    mv "$OLD_DIR" "$SCHWUNG_DIR"
    ln -s "$SCHWUNG_DIR" "$OLD_DIR"

    # Migrate sample/preset directories
    OLD_SAMPLES="/data/UserData/UserLibrary/Samples/Move Everything"
    NEW_SAMPLES="/data/UserData/UserLibrary/Samples/Schwung"
    if [ -d "$OLD_SAMPLES" ] && [ ! -d "$NEW_SAMPLES" ] && [ ! -L "$OLD_SAMPLES" ]; then
        mv "$OLD_SAMPLES" "$NEW_SAMPLES"
        ln -s "$NEW_SAMPLES" "$OLD_SAMPLES"
    fi

    OLD_PRESETS="/data/UserData/UserLibrary/Track Presets/Move Everything"
    NEW_PRESETS="/data/UserData/UserLibrary/Track Presets/Schwung"
    if [ -d "$OLD_PRESETS" ] && [ ! -d "$NEW_PRESETS" ] && [ ! -L "$OLD_PRESETS" ]; then
        mv "$OLD_PRESETS" "$NEW_PRESETS"
        ln -s "$NEW_PRESETS" "$OLD_PRESETS"
    fi
fi

# === Factory-reset / missing-payload safety net ===
# A factory reset (or any wipe of /data) removes the entire Schwung payload,
# but our root-partition hooks survive: this script IS /opt/move/Move, the
# stock binary is at /opt/move/MoveOriginal, and on glibc 2.35+ images
# /usr/lib/schwung-shim.so is a real copy (not a symlink). If we went ahead
# and exec'd MoveOriginal with LD_PRELOAD=schwung-shim.so, the shim would load
# and immediately fail — it needs /data for SHM, config, modules, and the
# link-subscriber — crashing MoveOriginal on every boot. That is exactly the
# "Move doesn't boot after a factory reset" failure. So if the payload on
# /data is gone, launch stock Move with no shim. The device always boots; a
# later reinstall restores Schwung. Needs no root, so it works under the
# `start-stop-daemon -c ableton` launch context.
if [ ! -f "$SCHWUNG_DIR/schwung-shim.so" ] && [ -x /opt/move/MoveOriginal ]; then
    exec /opt/move/MoveOriginal
fi

# === Fix /usr/lib/ shim symlink if stale ===
# After migration, ensure the shim symlink points to the right file
if [ -f "$SCHWUNG_DIR/schwung-shim.so" ]; then
    SHIM_TARGET=$(readlink /usr/lib/schwung-shim.so 2>/dev/null || true)
    if [ "$SHIM_TARGET" != "$SCHWUNG_DIR/schwung-shim.so" ]; then
        rm -f /usr/lib/schwung-shim.so
        ln -s "$SCHWUNG_DIR/schwung-shim.so" /usr/lib/schwung-shim.so
    fi
    # Remove old-name symlink if present
    rm -f /usr/lib/move-anything-shim.so
fi

# === Update /opt/move/Move entrypoint if stale ===
# If /opt/move/Move still references the old name, replace it with this script
if grep -q 'move-anything-shim.so' /opt/move/Move 2>/dev/null; then
    cp "$SCHWUNG_DIR/shim-entrypoint.sh" /opt/move/Move
    chmod +x /opt/move/Move
fi

# === Self-heal /usr/lib shim and /opt/move entrypoint at every boot ===
# /etc/init.d/move launches us via `start-stop-daemon -c ableton`, so this
# script (and everything it spawns) runs as ableton — which can't write
# /usr/lib or /opt/move directly. schwung-heal is a tiny setuid-root
# helper (no CLI input, hardcoded paths) that mirrors the data-partition
# shim and entrypoint to their system locations when stale. Idempotent.
# Runs BEFORE LD_PRELOAD exec so the corrected shim is what MoveOriginal
# loads — no reboot needed.
SCHWUNG_HEAL="$SCHWUNG_DIR/bin/schwung-heal"
if [ -x "$SCHWUNG_HEAL" ]; then
    "$SCHWUNG_HEAL" >>"$SCHWUNG_DIR/heal-boot.log" 2>&1
fi

# Set library path for bundled TTS libraries
export LD_LIBRARY_PATH=$SCHWUNG_DIR/lib:$LD_LIBRARY_PATH

# Note: link-subscriber is launched by the shim (auto-recovery lifecycle)

# Start live display server if present
DISPLAY_SRV="$SCHWUNG_DIR/display-server"
if [ -x "$DISPLAY_SRV" ]; then
    "$DISPLAY_SRV" >/dev/null 2>&1 &
fi

# Start schwung-manager web UI if present (skip if already running)
SCHWUNG_MGR="$SCHWUNG_DIR/schwung-manager"
SCHWUNG_MGR_LOG="$SCHWUNG_DIR/schwung-manager.log"
SCHWUNG_MGR_PID="$SCHWUNG_DIR/schwung-manager.pid"
if [ -x "$SCHWUNG_MGR" ]; then
    # Skip if already running.
    #
    # `kill -0` alone is NOT enough: it asks whether SOMETHING holds that pid,
    # not whether the manager does. The pid file survives a reboot, and Linux
    # hands the number out again — observed 2026-08-20, where the stale pid 928
    # came back as `display-server`, so this test passed, the manager was never
    # started, and port 7700 was simply dead until someone noticed. It fails
    # silently and only after a reboot, which is the worst combination.
    #
    # So confirm the pid is actually the manager by reading its cmdline.
    SCHWUNG_MGR_RUNNING=0
    if [ -f "$SCHWUNG_MGR_PID" ]; then
        mgr_pid="$(cat "$SCHWUNG_MGR_PID" 2>/dev/null)"
        # A non-numeric or empty pid file must not turn into a bare `/proc//cmdline`.
        case "$mgr_pid" in
            ''|*[!0-9]*) mgr_pid="" ;;
        esac
        # SCHWUNG_PROC_DIR is /proc on the device; the host test overrides it,
        # since the dev machines have no /proc to build a fixture in.
        if [ -n "$mgr_pid" ] && kill -0 "$mgr_pid" 2>/dev/null &&
           tr '\0' ' ' < "${SCHWUNG_PROC_DIR:-/proc}/$mgr_pid/cmdline" 2>/dev/null |
               grep -q "schwung-manager"; then
            SCHWUNG_MGR_RUNNING=1
        fi
    fi
    if [ "$SCHWUNG_MGR_RUNNING" = "1" ]; then
        : # already running
    else
        # Rotate log if over 100KB
        if [ -f "$SCHWUNG_MGR_LOG" ]; then
            log_size=$(wc -c < "$SCHWUNG_MGR_LOG" 2>/dev/null || echo 0)
            if [ "$log_size" -gt 102400 ]; then
                tail -c 102400 "$SCHWUNG_MGR_LOG" > "$SCHWUNG_MGR_LOG.tmp" 2>/dev/null
                mv "$SCHWUNG_MGR_LOG.tmp" "$SCHWUNG_MGR_LOG"
            fi
        fi
        "$SCHWUNG_MGR" -port 7700 -roots /data/UserData/ >>"$SCHWUNG_MGR_LOG" 2>&1 &
        echo $! > "$SCHWUNG_MGR_PID"
    fi
fi

# Start filebrowser for file management (port 404, no auth) if enabled
FB="$SCHWUNG_DIR/bin/filebrowser"
FB_FLAG="$SCHWUNG_DIR/filebrowser_enabled"
if [ -x "$FB" ] && [ -f "$FB_FLAG" ]; then
    "$FB" \
        --noauth \
        --address 0.0.0.0 \
        --port 404 \
        --root /data/UserData \
        --database "$SCHWUNG_DIR/filebrowser.db" \
        --disableThumbnails \
        --disablePreviewResize \
        --disableExec \
        --disableTypeDetectionByHeader \
        >/dev/null 2>&1 &
fi

exec env LD_PRELOAD=schwung-shim.so /opt/move/MoveOriginal
OLD_ENTRYPOINT_EOF
)

# ---- 2. derive every $SCHWUNG_DIR/ reference, rather than hand-restating it ----
referenced=$(printf '%s\n' "$old_entrypoint" | awk '
{
    line = $0
    while (match(line, /\$SCHWUNG_DIR\/[A-Za-z0-9_.\/-]+/)) {
        ref = substr(line, RSTART, RLENGTH)
        sub(/^\$SCHWUNG_DIR\//, "", ref)
        print ref
        line = substr(line, RSTART + RLENGTH)
    }
}' | sort -u)

ref_count=$(printf '%s\n' "$referenced" | awk 'NF { c++ } END { print c + 0 }')
if [ "$ref_count" -lt 5 ]; then
    say_fail "derived fewer than 5 SCHWUNG_DIR references from the vendored fixture (got $ref_count) -- the heredoc is probably broken"
fi

# ---- 3. skip-list: paths the old entrypoint only WRITES or that are runtime ----
# state never produced by build.sh/package.sh in the first place. Each entry
# is commented so a future reviewer sees WHY it is excluded, rather than
# guessing that it was forgotten.
is_skipped() {
    case "$1" in
        heal-boot.log) return 0 ;;          # write-only: schwung-heal's stdout/stderr, appended
        schwung-manager.log) return 0 ;;    # write-only: rotated + appended by the entrypoint
        schwung-manager.pid) return 0 ;;    # write-only: echo $! > ... on every launch
        filebrowser.db) return 0 ;;         # runtime db filebrowser itself creates, never shipped
        filebrowser_enabled) return 0 ;;    # runtime toggle flag written by settings, never shipped
        *) return 1 ;;
    esac
}

# ---- 4. classify the survivors: HARD (no existence guard) vs GUARDED-OPTIONAL ----
is_hard() {
    case "$1" in
        schwung-shim.so) return 0 ;;     # the payload sentinel itself -- its absence is what
                                          # silently degrades boot to bare MoveOriginal
        shim-entrypoint.sh) return 0 ;;  # `cp "$SCHWUNG_DIR/shim-entrypoint.sh" /opt/move/Move`
                                          # has no existence guard on the source path
        *) return 1 ;;
    esac
}
is_guarded_optional() {
    case "$1" in
        bin/schwung-heal|lib|display-server|schwung-manager|bin/filebrowser) return 0 ;;
        *) return 1 ;;
    esac
}

# Every referenced path must be accounted for by exactly one of: skip-list,
# HARD, GUARDED-OPTIONAL. An unaccounted reference means the fixture grew a
# new dependency nobody triaged -- fail loudly rather than silently ignoring it.
unaccounted=""
hard_deps=""
guarded_deps=""
while IFS= read -r ref; do
    [ -n "$ref" ] || continue
    if is_skipped "$ref"; then
        continue
    elif is_hard "$ref"; then
        hard_deps="$hard_deps $ref"
    elif is_guarded_optional "$ref"; then
        guarded_deps="$guarded_deps $ref"
    else
        unaccounted="$unaccounted $ref"
    fi
done <<EOF
$referenced
EOF

if [ -n "$unaccounted" ]; then
    say_fail "the old entrypoint fixture references paths not triaged into the skip-list, HARD, or GUARDED-OPTIONAL sets:$unaccounted"
fi

# Sanity: schwung-shim.so and shim-entrypoint.sh must actually have been
# picked up as HARD (a typo in is_hard() would silently degrade them to
# unaccounted, which the check above would already have caught -- but if
# someone "fixed" that by widening is_skipped() instead, this catches it).
case "$hard_deps" in
    *schwung-shim.so*) ;;
    *) say_fail "schwung-shim.so did not land in the HARD set -- the sentinel dependency is not being checked" ;;
esac
case "$hard_deps" in
    *shim-entrypoint.sh*) ;;
    *) say_fail "shim-entrypoint.sh did not land in the HARD set" ;;
esac

# ---- 5. every HARD and GUARDED-OPTIONAL dep must still be built AND packaged ----
#
# path | build.sh substring | package.sh substring | build/ artifact (for the
# local-only extra check; "-d" prefix means directory, else a plain file)
check_spec='
schwung-shim.so|build/schwung-shim.so|./schwung-shim.so|build/schwung-shim.so
shim-entrypoint.sh|shim-entrypoint.sh ./build/|./shim-entrypoint.sh|build/shim-entrypoint.sh
bin/schwung-heal|build/bin/schwung-heal|./bin|build/bin/schwung-heal
lib|./build/lib/|./lib|-dbuild/lib
display-server|build/display-server|./display-server|build/display-server
schwung-manager|build/schwung-manager|./schwung-manager|build/schwung-manager
bin/filebrowser|./build/bin/|./bin|build/bin/filebrowser
'

# Fed via a heredoc (not a pipe) so the loop runs in THIS shell, not a
# subshell -- a piped `while read` would run in a subshell and every
# say_fail/say_pass inside it would update a copy of $fails that vanishes
# when the pipeline exits.
while IFS='|' read -r dep build_pat pkg_pat artifact; do
    [ -n "$dep" ] || continue

    if ! contains "$BUILD_SH" "$build_pat"; then
        say_fail "$dep -- $BUILD_SH no longer builds it (expected to find: $build_pat)"
        continue
    fi
    if ! contains "$PACKAGE_SH" "$pkg_pat"; then
        say_fail "$dep -- $PACKAGE_SH no longer packages it (expected to find: $pkg_pat)"
        continue
    fi

    # Local-only extra: if a build/ tree exists on this machine, confirm the
    # artifact is actually there. Skipped entirely when build/ is absent (a
    # fresh checkout / CI without a prior build.sh run) -- this is a bonus
    # check, not the pin.
    if [ -d "$BUILD_DIR" ]; then
        case "$artifact" in
            -d*)
                a="${artifact#-d}"
                [ -d "$BUILD_DIR/${a#build/}" ] || true
                ;;
            *)
                [ -f "$BUILD_DIR/${artifact#build/}" ] || true
                ;;
        esac
    fi

    say_pass "$dep -- still built ($build_pat) and packaged ($pkg_pat)"
done <<EOF
$check_spec
EOF

if [ "$fails" -eq 0 ]; then
    say_pass "old pre-split entrypoint's payload dependencies (${hard_deps# } | guarded:${guarded_deps# }) are all still built and packaged -- a never-blessed device's next web update will not brick it"
    exit 0
else
    echo "FAILED: $fails check(s)"
    exit 1
fi
