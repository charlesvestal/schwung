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
	"syscall"
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

// invalidateRepairCache forces the next bootstrapNeeded() / shimStale()
// call to re-read. Called after any operation that may have changed repair
// state (e.g. user finished the SSH bootstrap and hit "Retry detection").
func invalidateRepairCache() {
	repairMu.Lock()
	repairLastCheck = time.Time{}
	repairMu.Unlock()
	shimMu.Lock()
	shimLastCheck = time.Time{}
	shimMu.Unlock()
	healMu.Lock()
	healLastCheck = time.Time{}
	healMu.Unlock()
}

const (
	repairShimData = "/data/UserData/schwung/schwung-shim.so"
	repairShimLib  = "/usr/lib/schwung-shim.so"
)

var (
	shimMu          sync.RWMutex
	shimLastCheck   time.Time
	shimStaleCached bool
)

// shimStale reports whether the LIVE shim at /usr/lib is out of date — i.e.
// not byte-identical to the installed payload shim on /data. That happens
// when an update extracted a new shim but the privileged mirror to /usr/lib
// never ran (unblessed/missing heal). This is the *actual* stuck-shim
// condition the user feels: blank module slots, "can't load", menu stuck on
// "none" — because shadow_ui (new) talks to an old shim over a mismatched
// SHM layout. Distinct from bootstrapNeeded (which only inspects the boot
// entrypoint and misses this). Reads as `ableton` — two file reads, no
// privilege needed. 30s cache.
func shimStale() bool {
	shimMu.RLock()
	if time.Since(shimLastCheck) < repairCacheTTL {
		v := shimStaleCached
		shimMu.RUnlock()
		return v
	}
	shimMu.RUnlock()

	shimMu.Lock()
	defer shimMu.Unlock()
	if time.Since(shimLastCheck) < repairCacheTTL {
		return shimStaleCached
	}
	shimLastCheck = time.Now()
	// No installed payload shim to compare against → nothing to advise.
	if _, err := os.Stat(repairShimData); err != nil {
		shimStaleCached = false
		return false
	}
	// Stale when the live /usr/lib copy isn't byte-identical to the payload
	// (filesIdentical also returns false if /usr/lib is missing → repair).
	shimStaleCached = !filesIdentical(repairShimData, repairShimLib)
	return shimStaleCached
}

const repairHealPath = "/data/UserData/schwung/bin/schwung-heal"

var (
	healMu          sync.RWMutex
	healLastCheck   time.Time
	healUnblessedC  bool
)

// healUnblessed reports whether the schwung-heal helper is NOT properly
// privileged — missing, or not setuid, or setuid but owned by a non-root user
// (e.g. `ableton`, which a failed web update leaves behind via os.Rename of the
// staged copy). In any of those states, heal cannot escalate when invoked by
// the ableton-context entrypoint/manager, so the NEXT web update will silently
// fail to mirror the shim — even if the live shim happens to match right now.
// This catches the latent case the band-aid SSH fix leaves behind (shim mirrored
// once via root, but heal still owned by ableton), so the banner persists until
// the durable repair (chown root + setuid) is done. 30s cache.
func healUnblessed() bool {
	healMu.RLock()
	if time.Since(healLastCheck) < repairCacheTTL {
		v := healUnblessedC
		healMu.RUnlock()
		return v
	}
	healMu.RUnlock()

	healMu.Lock()
	defer healMu.Unlock()
	if time.Since(healLastCheck) < repairCacheTTL {
		return healUnblessedC
	}
	healLastCheck = time.Now()
	info, err := os.Stat(repairHealPath)
	if err != nil {
		healUnblessedC = true // missing → can't self-heal
		return true
	}
	if info.Mode()&os.ModeSetuid == 0 {
		healUnblessedC = true // no setuid bit → runs as caller (ableton)
		return true
	}
	// setuid set, but owner must be root for it to grant euid=0.
	if st, ok := info.Sys().(*syscall.Stat_t); ok && st.Uid != 0 {
		healUnblessedC = true // setuid-but-nonroot (e.g. ableton) → euid=ableton
		return true
	}
	healUnblessedC = false
	return false
}

// repairNeeded is the combined "manual repair required" signal: the boot
// entrypoint can't self-heal (bootstrapNeeded), OR the live shim is already
// out of date (shimStale, active symptom), OR heal isn't properly blessed
// (healUnblessed, latent — next update will fail). All need a one-time root
// action the ableton-context web manager cannot perform.
func repairNeeded() bool {
	return bootstrapNeeded() || shimStale() || healUnblessed()
}
