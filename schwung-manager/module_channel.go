// Beta / stable channel plumbing for the module catalog.
//
// Goal: let a module author publish a build for adventurous users to try
// without pushing it to everyone. A stable release stays universally
// visible; a beta is only visible to users who opted in.
//
// The manifest contract for module repos (release.json) is strictly
// additive. Old shape:
//
//	{"version": "1.2.3", "download_url": "https://.../mod.tar.gz"}
//
// New shape (optional; old shape still resolves as stable):
//
//	{
//	  "version": "1.2.3",
//	  "download_url": "https://.../mod-1.2.3.tar.gz",
//	  "channels": {
//	    "stable": {"version": "1.2.3",         "download_url": "..."},
//	    "beta":   {"version": "1.3.0-beta.2",  "download_url": "..."}
//	  }
//	}
//
// Multi-module release.json (one repo publishes several catalog entries)
// gets the same optional `channels` per module entry.
//
// Resolution rules (resolveReleaseForChannel):
//   - stable channel: use channels.stable if present, else fall back to
//     the top-level {version, download_url}.
//   - beta channel:   use channels.beta ONLY if it is strictly newer than
//     stable. Otherwise fall back to stable. This is what keeps a beta
//     user from getting stranded on an old beta after stable catches up.
//
// The user-selected channel is a manager-global preference stored in
// <basePath>/manager-cache/manager-config.json. Default is stable.
// Nothing outside the manager depends on it — the on-device shadow UI
// doesn't fetch modules.

package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// Channel names. Keep as constants so template comparisons and JSON
// values agree on spelling.
const (
	ChannelStable = "stable"
	ChannelBeta   = "beta"
)

// ChannelEntry is one channel's slot in a release.json's `channels` map.
// Same shape as the top-level {version, download_url} — the resolver
// treats them uniformly.
type ChannelEntry struct {
	Version     string `json:"version"`
	DownloadURL string `json:"download_url"`
}

// ChannelSet is the optional `channels` map on a release.json entry
// (top-level for single-module release.json, per-entry for multi-module).
type ChannelSet struct {
	Stable *ChannelEntry `json:"stable,omitempty"`
	Beta   *ChannelEntry `json:"beta,omitempty"`
}

// resolveReleaseForChannel picks the version+URL to install for a given
// channel from a decoded release.json (already narrowed via forModule).
// It returns the picked entry plus which channel actually served the
// request — beta users fall back to stable when the beta slot is missing
// or older, and that fallback needs to be visible in the UI.
//
// Callers pass an empty channel string for "no preference" (== stable).
func resolveReleaseForChannel(rel ReleaseJSON, want string) (ChannelEntry, string) {
	stable := stableEntry(rel)

	if want == ChannelBeta && rel.Channels != nil && rel.Channels.Beta != nil {
		beta := *rel.Channels.Beta
		if beta.Version != "" && beta.DownloadURL != "" {
			// A beta only wins when it is strictly newer than stable.
			// Equal or older betas fall through to stable, so the
			// beta user quietly lands on stable once it catches up.
			if channelNewer(beta.Version, stable.Version) {
				return beta, ChannelBeta
			}
		}
	}
	return stable, ChannelStable
}

// channelNewer reports whether `beta` should win over `stable` for a
// beta user. Reuses isNewerSemver for the numeric compare but adds
// one SemVer-2.0.0 rule the tolerant helper misses: a version WITH a
// prerelease suffix (e.g. "0.13.0-beta.1") is LESS than the same
// base version WITHOUT one ("0.13.0"). Without this, isNewerSemver
// classifies "0.13.0-beta.1" as newer than "0.13.0" (it has more
// dotted parts) — which would strand a beta user on the prerelease
// after the matching stable cut, exactly the failure mode this
// channel design is supposed to prevent.
func channelNewer(beta, stable string) bool {
	betaBase, betaPre := splitPrerelease(beta)
	stableBase, stablePre := splitPrerelease(stable)
	// Same base + exactly one side has a prerelease: the base version
	// wins. This is the guard against isNewerSemver classifying
	// "0.13.0-beta.1" as newer than "0.13.0". When BOTH sides have
	// prereleases (comparing beta.2 vs beta.1) or NEITHER does, fall
	// through to the ordinary compare.
	if betaBase == stableBase && (betaPre == "") != (stablePre == "") {
		return betaPre == "" // beta wins iff stable is the prerelease
	}
	return isNewerSemver(beta, stable)
}

// splitPrerelease splits a version on the first "-" so we can compare
// "0.13.0" and "0.13.0-beta.1" as (base, prerelease).
func splitPrerelease(v string) (string, string) {
	v = strings.TrimPrefix(v, "v")
	if i := strings.IndexByte(v, '-'); i >= 0 {
		return v[:i], v[i+1:]
	}
	return v, ""
}

// stableEntry returns the stable-channel slot for a release.json,
// preferring channels.stable and falling back to the top-level fields.
// Never returns nil — an entirely blank release.json still resolves to
// a zero-value ChannelEntry so the resolver can hand a caller SOMETHING
// to check for emptiness.
func stableEntry(rel ReleaseJSON) ChannelEntry {
	if rel.Channels != nil && rel.Channels.Stable != nil {
		s := *rel.Channels.Stable
		if s.Version != "" || s.DownloadURL != "" {
			return s
		}
	}
	return ChannelEntry{
		Version:     rel.Version,
		DownloadURL: rel.DownloadURL,
	}
}

// betaEntry returns the beta-channel slot for a release.json, or nil
// if none is declared. Used by UI hints ("newer beta available") that
// need to know a beta exists regardless of whether the current user is
// on the beta channel.
func betaEntry(rel ReleaseJSON) *ChannelEntry {
	if rel.Channels == nil || rel.Channels.Beta == nil {
		return nil
	}
	b := *rel.Channels.Beta
	if b.Version == "" && b.DownloadURL == "" {
		return nil
	}
	return &b
}

// channelVersion returns the version string a given channel would
// install for one module, using the static-site release-metadata
// snapshot (not a live release.json fetch — this feeds page rendering).
// Returns "" when metadata has nothing to say. Uses the same
// prerelease-aware compare as the resolver, so the UI and the
// download can't disagree.
func channelVersion(rm ReleaseMeta, channel string) string {
	stable := channelStableVersion(rm)
	if channel == ChannelBeta && rm.Channels != nil && rm.Channels.Beta != nil {
		beta := rm.Channels.Beta.Version
		if beta != "" && channelNewer(beta, stable) {
			return beta
		}
	}
	return stable
}

// channelStableVersion resolves the stable version from a ReleaseMeta,
// preferring channels.stable and falling back to the top-level Version
// so older metadata files (no channels block) still work.
func channelStableVersion(rm ReleaseMeta) string {
	if rm.Channels != nil && rm.Channels.Stable != nil && rm.Channels.Stable.Version != "" {
		return rm.Channels.Stable.Version
	}
	return rm.Version
}

// ---------------------------------------------------------------------------
// Manager-side channel preference
// ---------------------------------------------------------------------------

// ChannelPref is the manager-global channel preference. Persisted so it
// survives restarts. Missing/invalid file resolves to stable — a user
// who has never touched the toggle sees the pre-existing behavior.
type ChannelPref struct {
	basePath string
	mu       sync.RWMutex
	channel  string
}

// NewChannelPref loads the on-disk preference. Nil-safe callers welcome:
// a nil ChannelPref responds Stable to every read, which is the right
// default for tests and non-device dev environments.
func NewChannelPref(basePath string) *ChannelPref {
	cp := &ChannelPref{basePath: basePath, channel: ChannelStable}
	cp.loadFromDisk()
	return cp
}

func (cp *ChannelPref) configPath() string {
	if cp == nil || cp.basePath == "" {
		return ""
	}
	return filepath.Join(cp.basePath, "manager-cache", "manager-config.json")
}

func (cp *ChannelPref) loadFromDisk() {
	p := cp.configPath()
	if p == "" {
		return
	}
	data, err := os.ReadFile(p)
	if err != nil {
		return
	}
	var raw map[string]any
	if err := json.Unmarshal(data, &raw); err != nil {
		return
	}
	if v, ok := raw["module_channel"].(string); ok {
		if c := normalizeChannel(v); c != "" {
			cp.channel = c
		}
	}
}

// Channel returns the current channel — always a valid non-empty value.
func (cp *ChannelPref) Channel() string {
	if cp == nil {
		return ChannelStable
	}
	cp.mu.RLock()
	defer cp.mu.RUnlock()
	if cp.channel == "" {
		return ChannelStable
	}
	return cp.channel
}

// SetChannel persists a new channel choice. An unknown value is
// rejected with a bool false — callers surface that as an HTTP 400.
func (cp *ChannelPref) SetChannel(v string) bool {
	if cp == nil {
		return false
	}
	c := normalizeChannel(v)
	if c == "" {
		return false
	}
	cp.mu.Lock()
	cp.channel = c
	cp.mu.Unlock()

	p := cp.configPath()
	if p == "" {
		return true
	}
	// Merge into any existing manager-config so a future setting on
	// the same file doesn't get clobbered by a channel write.
	existing := map[string]any{}
	if data, err := os.ReadFile(p); err == nil {
		_ = json.Unmarshal(data, &existing)
	}
	existing["module_channel"] = c
	_ = os.MkdirAll(filepath.Dir(p), 0o755)
	if data, err := json.MarshalIndent(existing, "", "  "); err == nil {
		_ = os.WriteFile(p, append(data, '\n'), 0o644)
	}
	return true
}

// normalizeChannel accepts the two known values (case-insensitively)
// and rejects anything else with an empty string. Keeping this in one
// spot means the disk file and the HTTP handler can't drift apart on
// what "beta" means.
func normalizeChannel(v string) string {
	switch strings.ToLower(strings.TrimSpace(v)) {
	case ChannelStable:
		return ChannelStable
	case ChannelBeta:
		return ChannelBeta
	}
	return ""
}
