# Fix On-Device Upgrade Entrypoint & schwung-manager Startup

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Ensure schwung-manager starts correctly after on-device upgrades, so `schwung.local` and the web UI work for all users.

**Architecture:** Add a boot-time breadcrumb check to `shim-entrypoint.sh` that runs `post-update.sh` as root when needed. Fix the on-device update paths to write the breadcrumb. This is a belt-and-suspenders approach since the entrypoint runs as root (init system) while on-device updates run as ableton (unprivileged).

**Tech Stack:** Shell scripts, JavaScript (QuickJS), Go (schwung-manager)

---

## Root Cause Analysis

`post-update.sh` needs root to:
- Copy `shim-entrypoint.sh` to `/opt/move/Move`
- Wrap MoveWebService to use port 8080 (freeing port 80 for schwung-manager)
- Update symlinks in `/usr/lib/`

Three upgrade paths exist:

| Path | Runs as | post-update.sh works? |
|------|---------|----------------------|
| GUI installer (`install.sh`) | root (SSH) | Yes — does its own root setup, doesn't rely on post-update.sh |
| schwung-manager web UI | root (init system) | Yes — schwung-manager inherits root |
| On-device Module Store (shadow_ui.js) | ableton | **NO** — silently fails on root operations |
| On-device Module Store (store/ui.js) | ableton | **Doesn't even call it** |

schwung-manager itself only runs if:
1. `/opt/move/Move` is the shim-entrypoint.sh script (not stock binary)
2. The entrypoint has the schwung-manager launch block
3. MoveWebService is wrapped to use port 8080 (so schwung-manager can bind port 80)

All three conditions require root to set up. So on-device upgrades can never bootstrap schwung-manager from scratch.

## User Scenarios

### Scenario A: Fresh install via GUI installer
- **Status:** Works correctly
- **Why:** install.sh SSHes as root, sets up entrypoint, MoveWebService wrapper, everything
- **No fix needed**

### Scenario B: User on current version, upgrades on-device to new version
- **Status:** BROKEN — schwung-manager stops working if entrypoint gets stale
- **Current behavior:** post-update.sh runs as ableton, silently fails to update `/opt/move/Move`
- **Impact:** `/opt/move/Move` keeps old entrypoint, schwung-manager block may be missing or outdated
- **Fix:** Breadcrumb approach (see below)

### Scenario C: User installed old version (pre-schwung-manager), upgrades on-device
- **Status:** BROKEN — schwung-manager never starts
- **Current behavior:** `/opt/move/Move` has old entrypoint without schwung-manager block, post-update.sh can't fix it
- **Impact:** No schwung.local, no web UI
- **Fix:** Breadcrumb won't help (old entrypoint doesn't check for it). Requires ONE GUI repair/reinstall to get the new entrypoint. After that, breadcrumb approach handles future updates.

### Scenario D: User upgrades via schwung-manager web UI
- **Status:** Works correctly
- **Why:** schwung-manager runs as root, post-update.sh succeeds
- **No fix needed** (but schwung-manager must already be running — chicken-and-egg)

### Scenario E: User does GUI repair after on-device upgrade
- **Status:** SHOULD work but reported as broken by Pirgo
- **Why unclear:** The GUI repair SSHes as root and does all setup. Need more diagnostics from Pirgo.
- **Possible causes:** GUI installer error not shown to user, SSH key issue, network timeout

### Scenario F: Module Store (non-shadow) host update
- **Status:** BROKEN — doesn't call post-update.sh at all
- **Current behavior:** Extracts tarball, says "restart to apply", no post-update
- **Fix:** Add post-update.sh call (will still fail as ableton, but breadcrumb write will work)

## Fix Strategy

### Part 1: Breadcrumb in shim-entrypoint.sh (boot-time root execution)

Add to `shim-entrypoint.sh` near the top (after migration block):

```bash
# === Run post-update if flagged by on-device upgrade ===
POST_UPDATE_FLAG="$SCHWUNG_DIR/post-update-needed"
POST_UPDATE_SCRIPT="$SCHWUNG_DIR/scripts/post-update.sh"
if [ -f "$POST_UPDATE_FLAG" ] && [ -f "$POST_UPDATE_SCRIPT" ]; then
    sh "$POST_UPDATE_SCRIPT"
    rm -f "$POST_UPDATE_FLAG"
fi
```

This runs as root (init system launches the entrypoint) and handles future on-device upgrades.

### Part 2: Always run post-update.sh at boot (belt and suspenders)

Actually, since `post-update.sh` is idempotent and fast, we should just **always run it at boot** instead of using a breadcrumb. This eliminates the breadcrumb coordination entirely and handles ALL scenarios — including ones where the breadcrumb wasn't written (old update code, interrupted update, etc.).

```bash
# === Run post-update setup (idempotent, ensures entrypoint/wrappers are current) ===
POST_UPDATE_SCRIPT="$SCHWUNG_DIR/scripts/post-update.sh"
if [ -f "$POST_UPDATE_SCRIPT" ]; then
    sh "$POST_UPDATE_SCRIPT"
fi
```

This is simpler, more robust, and self-healing. If anything gets out of sync (manual file edits, partial updates, firmware reset), the next boot fixes it.

### Part 3: Fix Module Store (non-shadow) to call post-update.sh

`src/modules/store/ui.js` `updateHost()` doesn't call post-update.sh. Add it (it'll fail as ableton for root ops, but that's OK — boot-time handles it).

### Part 4: Improve post-update.sh error visibility

Currently all root operations fail silently. Add breadcrumb writing so we can at least track whether it ran and whether root ops succeeded:

```bash
echo "$(date '+%Y-%m-%d %H:%M:%S') uid=$(id -u)" > "$BASE/post-update-ran"
```

### Transition Plan for Existing Users

| User state | What they need to do | After that |
|-----------|---------------------|------------|
| Scenario C (old entrypoint, no schwung-manager block) | ONE GUI repair/reinstall | New entrypoint auto-runs post-update.sh on every boot |
| Scenario B (recent entrypoint but stale) | Reboot after next release | New entrypoint handles it |
| Scenario E (GUI repair "didn't work") | Need diagnostics — try manual `ssh root@move.local "sh /data/UserData/schwung/scripts/post-update.sh"` then reboot | New entrypoint handles it |

**KEY INSIGHT:** The "always run post-update at boot" approach means that once a user has the new entrypoint (via GUI install), they NEVER need the GUI installer again for entrypoint/wrapper maintenance. On-device upgrades update `post-update.sh` in the data partition, and the next boot runs it as root.

---

## Tasks

### Task 1: Update shim-entrypoint.sh to run post-update.sh at boot

**Files:**
- Modify: `src/shim-entrypoint.sh`

**Step 1: Add post-update.sh execution block**

Add after the `/usr/lib/` shim symlink fix block (after line 41), before the display server start:

```bash
# === Run post-update setup (idempotent — ensures entrypoint, wrappers, symlinks are current) ===
POST_UPDATE_SCRIPT="$SCHWUNG_DIR/scripts/post-update.sh"
if [ -f "$POST_UPDATE_SCRIPT" ]; then
    sh "$POST_UPDATE_SCRIPT" >> "$SCHWUNG_DIR/post-update-boot.log" 2>&1
fi
```

Note: This must come BEFORE the schwung-manager launch block, because post-update.sh sets up the MoveWebService wrapper (port 8080), which must happen before schwung-manager tries to bind port 80.

**Step 2: Verify the entrypoint order is correct**

The order should be:
1. Migration (move-anything → schwung)
2. Fix shim symlink
3. **Run post-update.sh** ← NEW
4. Start display-server
5. Start schwung-manager
6. Start filebrowser
7. exec MoveOriginal

**Step 3: Test mentally / verify idempotency**

`post-update.sh` is already idempotent:
- Uses `if [ ! -f ... ]` guards for backups
- Overwrites wrappers unconditionally (safe — content is deterministic)
- `rm -f` and `ln -sf` are idempotent

Running it every boot adds <100ms and ensures the system is always consistent.

### Task 2: Add post-update.sh call to Module Store (non-shadow) update path

**Files:**
- Modify: `src/modules/store/ui.js`

**Step 1: Add post-update.sh call after extraction**

In `updateHost()` function, after the extract step (around line 224-229), add:

```javascript
    /* Run post-update setup (runs as ableton — root ops will be handled at next boot
     * by shim-entrypoint.sh which runs post-update.sh as root) */
    if (host_file_exists(BASE_DIR + '/scripts/post-update.sh')) {
        host_system_cmd('sh "' + BASE_DIR + '/scripts/post-update.sh"');
    }
```

This won't fix root operations immediately, but it will handle ableton-level ops and write the breadcrumb. The real fix happens at next boot via the entrypoint.

### Task 3: Improve post-update.sh logging

**Files:**
- Modify: `scripts/post-update.sh`

**Step 1: Add UID logging and operation status**

Change the breadcrumb line (line 104) to include more diagnostic info:

```bash
echo "$(date '+%Y-%m-%d %H:%M:%S') uid=$(id -u) entrypoint=$(test -f /opt/move/Move && head -c 2 /opt/move/Move)" > "$BASE/post-update-ran"
```

**Step 2: Add individual operation logging**

After the entrypoint copy (line 47), add:

```bash
    echo "post-update: entrypoint installed"
else
    echo "post-update: skipped entrypoint (MoveOriginal missing or entrypoint missing)"
```

After the MoveWebService wrapper creation (line 67), add:

```bash
        echo "post-update: MoveWebService wrapper installed (port 8080)"
```

This way `post-update-boot.log` will show exactly what happened on each boot.

### Task 4: Verify no regressions

**Manual verification on device:**

1. Deploy with `./scripts/install.sh local --skip-modules --skip-confirmation`
2. Verify schwung-manager starts: `pidof schwung-manager`
3. Verify schwung.local resolves
4. Simulate on-device upgrade: remove schwung-manager block from `/opt/move/Move`, reboot
5. Verify post-update.sh restores it on boot
6. Verify schwung-manager starts after reboot

### Task 5: Commit

```bash
git add src/shim-entrypoint.sh scripts/post-update.sh src/modules/store/ui.js
git commit -m "fix: run post-update.sh as root at boot to fix on-device upgrades

On-device upgrades (Module Store, shadow UI) run post-update.sh as
ableton, which silently fails on root operations (copying entrypoint
to /opt/move/Move, wrapping MoveWebService). This leaves schwung-manager
unable to start because MoveWebService holds port 80.

Fix: shim-entrypoint.sh now runs post-update.sh at boot (as root,
from init system). Since post-update.sh is idempotent, this is safe
to run every boot and self-heals any drift.

Also adds post-update.sh call to the non-shadow Module Store update
path which was missing it entirely."
```
