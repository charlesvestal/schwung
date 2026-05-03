// Self-heal stale shim and entrypoint.
//
// On-device update paths (Module Store, shadow_ui) extract the host tarball
// while running as the unprivileged `ableton` user, so post-update.sh's
// /usr/lib/ and /opt/move/ writes silently fail. Users who upgrade host
// on-device then reboot and load a stale /usr/lib/schwung-shim.so even
// though /data/UserData/schwung/schwung-shim.so is current — symptom: any
// shim feature added in the new release (e.g. button_passthrough or cable-0
// transport forwarding) is missing.
//
// schwung-manager is launched by shim-entrypoint.sh from init, which runs
// as root, so this binary inherits root and can finish post-update.sh's
// privileged work. Compare mtimes on every startup; if anything is stale,
// run post-update.sh, verify the gap closed, then trigger one reboot
// (with a 60 s loop guard) so the freshly-installed shim is what mmaps
// into MoveOriginal next time.

package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const (
	healLibShim     = "/usr/lib/schwung-shim.so"
	healSysEntry    = "/opt/move/Move"
	healGuardFile   = ".self-heal-reboot-ts"
	healGuardWindow = 60 * time.Second
)

// healShimIfStale runs post-update.sh when the OS-level shim or entrypoint
// is older than the data-partition copy, and triggers one bounded reboot
// if the heal succeeded. Idempotent and silent on a clean system.
func (app *App) healShimIfStale() {
	dataShim := filepath.Join(app.basePath, "schwung-shim.so")
	dataEntry := filepath.Join(app.basePath, "shim-entrypoint.sh")

	shimStale := isStale(dataShim, healLibShim)
	entryStale := isStale(dataEntry, healSysEntry)
	if !shimStale && !entryStale {
		return
	}
	app.logger.Info("self-heal: stale files detected",
		"shim_stale", shimStale, "entrypoint_stale", entryStale)

	postUpdate := filepath.Join(app.basePath, "scripts", "post-update.sh")
	if _, err := os.Stat(postUpdate); err != nil {
		app.logger.Warn("self-heal: post-update.sh missing, cannot heal",
			"path", postUpdate, "err", err)
		return
	}

	cmd := exec.Command("sh", postUpdate)
	cmd.Dir = app.basePath
	output, err := cmd.CombinedOutput()
	if err != nil {
		app.logger.Error("self-heal: post-update.sh failed",
			"err", err, "output", string(output))
		return
	}
	app.logger.Info("self-heal: post-update.sh completed", "output", string(output))

	// Re-check — only reboot if the heal actually closed the gap. Without
	// this guard a permanent stat mismatch (e.g. mtime preserved by tar)
	// would loop reboots up to the loop-guard window forever.
	if isStale(dataShim, healLibShim) || isStale(dataEntry, healSysEntry) {
		app.logger.Warn("self-heal: post-update.sh ran but files still stale, not rebooting")
		return
	}

	guardPath := filepath.Join(app.basePath, healGuardFile)
	if data, err := os.ReadFile(guardPath); err == nil {
		if ts, perr := strconv.ParseInt(strings.TrimSpace(string(data)), 10, 64); perr == nil {
			elapsed := time.Since(time.Unix(ts, 0))
			if elapsed >= 0 && elapsed < healGuardWindow {
				app.logger.Warn("self-heal: skipping reboot — already rebooted recently",
					"since", elapsed)
				return
			}
		}
	}

	if err := os.WriteFile(guardPath, []byte(strconv.FormatInt(time.Now().Unix(), 10)), 0644); err != nil {
		app.logger.Warn("self-heal: failed to write guard file (continuing anyway)", "err", err)
	}

	app.logger.Info("self-heal: rebooting to load new shim")
	exec.Command("sync").Run()
	if err := exec.Command("reboot").Run(); err != nil {
		app.logger.Error("self-heal: reboot command failed", "err", err)
	}
}

// isStale returns true when src exists and is newer than dst, or when dst
// is missing entirely. Returns false when src can't be stat'd (don't trigger
// heals on missing source).
func isStale(src, dst string) bool {
	si, err := os.Stat(src)
	if err != nil {
		return false
	}
	di, err := os.Stat(dst)
	if err != nil {
		return true
	}
	return si.ModTime().After(di.ModTime())
}
