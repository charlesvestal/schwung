// Repair status: detect when the live entrypoint at /opt/move/Move
// doesn't include the boot-time schwung-heal invocation (the self-heal
// gate). Users in this state need a one-time root operation to install
// the new entrypoint + chmod 4755 the schwung-heal helper — neither
// schwung-manager (running as ableton) nor any on-device update path
// can perform that bootstrap without root. We surface a banner on every
// page so the user knows what's wrong and how to fix it.
//
// Cached for 30s so we don't re-read the file on every request, but
// refresh quickly enough that a successful repair is visible without
// requiring the user to wait.

package main

import (
	"os"
	"strings"
	"sync"
	"time"
)

const (
	repairEntrypointPath = "/opt/move/Move"
	repairTokenString    = "schwung-heal"
	repairCacheTTL       = 30 * time.Second
)

var (
	repairMu        sync.RWMutex
	repairLastCheck time.Time
	repairBootstrap bool
)

// bootstrapNeeded returns true when /opt/move/Move is missing the
// `schwung-heal` invocation (i.e. self-heal is not installed and any
// future shim drift will go unfixed). Reads from a 30s cache.
func bootstrapNeeded() bool {
	repairMu.RLock()
	if time.Since(repairLastCheck) < repairCacheTTL {
		v := repairBootstrap
		repairMu.RUnlock()
		return v
	}
	repairMu.RUnlock()

	repairMu.Lock()
	defer repairMu.Unlock()
	// Double-check after acquiring write lock; another goroutine may have refreshed.
	if time.Since(repairLastCheck) < repairCacheTTL {
		return repairBootstrap
	}
	repairLastCheck = time.Now()
	data, err := os.ReadFile(repairEntrypointPath)
	if err != nil {
		// File missing or unreadable — don't nag (we can't help anyway).
		repairBootstrap = false
		return false
	}
	repairBootstrap = !strings.Contains(string(data), repairTokenString)
	return repairBootstrap
}

// invalidateRepairCache forces the next bootstrapNeeded() call to
// re-read the entrypoint. Called after any operation that may have
// changed bootstrap state (e.g. user finished the SSH bootstrap and
// hit "Retry detection" on the repair page).
func invalidateRepairCache() {
	repairMu.Lock()
	repairLastCheck = time.Time{}
	repairMu.Unlock()
}
