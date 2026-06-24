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
	"bytes"
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

	// Content-based, not mtime-based: re-heal whenever the live system copy
	// differs byte-for-byte from the data-partition source. mtime comparison
	// (the old isStale) could skip a genuinely-different shim of equal size with
	// a non-newer mtime — exactly the silent-stale case this is meant to catch.
	shimStale := !filesIdentical(dataShim, healLibShim)
	entryStale := !filesIdentical(dataEntry, healSysEntry)
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

// filesIdentical reports whether a and b both exist and have byte-identical
// contents. A size check short-circuits the common "different build" case
// before reading. Any stat/read error (missing file, no permission) → false,
// i.e. "not confirmed identical" — the safe answer for the upgrade gate, which
// treats false as "mirror not verified, keep the update retryable".
func filesIdentical(a, b string) bool {
	ai, err := os.Stat(a)
	if err != nil {
		return false
	}
	bi, err := os.Stat(b)
	if err != nil {
		return false
	}
	if ai.Size() != bi.Size() {
		return false
	}
	ab, err := os.ReadFile(a)
	if err != nil {
		return false
	}
	bb, err := os.ReadFile(b)
	if err != nil {
		return false
	}
	return bytes.Equal(ab, bb)
}
