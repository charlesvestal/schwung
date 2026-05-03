// Self-heal stale shim and entrypoint.
//
// /etc/init.d/move launches MoveLauncher via `start-stop-daemon -c
// ableton`, so MoveLauncher → MoveOriginal → shim-entrypoint.sh →
// schwung-manager all run as ableton. This Go binary therefore can't
// write /usr/lib/schwung-shim.so or /opt/move/Move directly. The
// privilege escalation goes through schwung-heal, a tiny setuid-root
// helper installed at /data/UserData/schwung/bin/schwung-heal (see
// src/schwung-heal.c).
//
// shim-entrypoint.sh already invokes schwung-heal at every boot before
// LD_PRELOAD exec, so by the time schwung-manager comes up /usr/lib
// should already be current. This function is belt-and-suspenders: it
// re-runs the heal when schwung-manager was restarted on-the-fly
// (e.g. after a web manager extract) without a full Move reboot. The
// in-process MoveOriginal still has the old shim mmap'd until the next
// Move restart — schwung-heal closes the file-system gap so the next
// boot loads the right binary.

package main

import (
	"os"
	"os/exec"
	"path/filepath"
)

const (
	healLibShim  = "/usr/lib/schwung-shim.so"
	healSysEntry = "/opt/move/Move"
)

// healShimIfStale invokes schwung-heal (setuid-root) when the OS-level
// shim or entrypoint is older than the data-partition copy. Silent
// no-op on a clean system.
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

	heal := filepath.Join(app.basePath, "bin", "schwung-heal")
	info, err := os.Stat(heal)
	if err != nil {
		app.logger.Warn("self-heal: schwung-heal missing, cannot heal",
			"path", heal, "err", err)
		return
	}
	if info.Mode()&os.ModeSetuid == 0 {
		app.logger.Warn("self-heal: schwung-heal lacks setuid bit — install incomplete?",
			"path", heal, "mode", info.Mode().String())
		// fall through and try anyway; the helper will print a clear error
	}

	output, err := exec.Command(heal).CombinedOutput()
	if err != nil {
		app.logger.Error("self-heal: schwung-heal failed",
			"err", err, "output", string(output))
		return
	}
	app.logger.Info("self-heal: schwung-heal completed", "output", string(output))
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
