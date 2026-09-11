package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func decode(t *testing.T, s string) ReleaseJSON {
	t.Helper()
	var r ReleaseJSON
	if err := json.NewDecoder(strings.NewReader(s)).Decode(&r); err != nil {
		t.Fatalf("decode: %v", err)
	}
	return r
}

// A release.json with no channels block must resolve identically for
// both channels — that is the backwards-compat guarantee.
func TestResolveNoChannelsBlock(t *testing.T) {
	rel := decode(t, `{"version":"1.2.3","download_url":"https://x/mod.tar.gz"}`)

	for _, ch := range []string{ChannelStable, ChannelBeta, "", "garbage"} {
		got, served := resolveReleaseForChannel(rel, ch)
		if got.Version != "1.2.3" || got.DownloadURL != "https://x/mod.tar.gz" {
			t.Errorf("channel %q: unexpected %+v", ch, got)
		}
		if served != ChannelStable {
			t.Errorf("channel %q: expected served=stable, got %q", ch, served)
		}
	}
}

// A stable user must never see beta URLs, even when a beta entry exists.
func TestResolveStableIgnoresBeta(t *testing.T) {
	rel := decode(t, `{
		"version":"1.2.3","download_url":"https://x/1.2.3.tar.gz",
		"channels":{
			"stable":{"version":"1.2.3","download_url":"https://x/1.2.3.tar.gz"},
			"beta":{"version":"1.3.0-beta.1","download_url":"https://x/1.3.0-beta.1.tar.gz"}
		}
	}`)
	got, served := resolveReleaseForChannel(rel, ChannelStable)
	if got.Version != "1.2.3" || served != ChannelStable {
		t.Errorf("got %+v (%s), want 1.2.3 stable", got, served)
	}
}

// A beta strictly newer than stable wins for a beta user.
func TestResolveBetaAheadOfStable(t *testing.T) {
	rel := decode(t, `{
		"channels":{
			"stable":{"version":"1.2.3","download_url":"https://x/1.2.3.tar.gz"},
			"beta":{"version":"1.3.0-beta.1","download_url":"https://x/1.3.0-beta.1.tar.gz"}
		}
	}`)
	got, served := resolveReleaseForChannel(rel, ChannelBeta)
	if got.Version != "1.3.0-beta.1" || served != ChannelBeta {
		t.Errorf("got %+v (%s), want beta 1.3.0-beta.1", got, served)
	}
}

// The "beta user quietly lands on stable once stable catches up" rule:
// stable == beta or stable > beta both make beta lose.
func TestResolveBetaFallsBackWhenStableCatchesUp(t *testing.T) {
	// Same version in both slots: beta must NOT win (would keep the
	// user on a beta URL for an identical release).
	relEqual := decode(t, `{
		"channels":{
			"stable":{"version":"1.3.0","download_url":"https://x/stable-1.3.0.tar.gz"},
			"beta":{"version":"1.3.0","download_url":"https://x/beta-1.3.0.tar.gz"}
		}
	}`)
	got, served := resolveReleaseForChannel(relEqual, ChannelBeta)
	if got.DownloadURL != "https://x/stable-1.3.0.tar.gz" || served != ChannelStable {
		t.Errorf("equal: got %+v (%s), want stable-1.3.0", got, served)
	}

	// Stable strictly newer than beta: beta must lose.
	relAhead := decode(t, `{
		"channels":{
			"stable":{"version":"1.4.0","download_url":"https://x/1.4.0.tar.gz"},
			"beta":{"version":"1.3.0-beta.2","download_url":"https://x/1.3.0-beta.2.tar.gz"}
		}
	}`)
	got, served = resolveReleaseForChannel(relAhead, ChannelBeta)
	if got.Version != "1.4.0" || served != ChannelStable {
		t.Errorf("ahead: got %+v (%s), want stable 1.4.0", got, served)
	}
}

// When channels.stable is omitted but the top-level version/url exist,
// they act as the stable slot. This is the shape produced by the
// suggested workflow between publishing a beta and cutting the next
// stable — top-level stays pinned to the last stable while
// channels.beta rolls forward.
func TestResolveTopLevelActsAsStable(t *testing.T) {
	rel := decode(t, `{
		"version":"1.2.3","download_url":"https://x/1.2.3.tar.gz",
		"channels":{
			"beta":{"version":"1.3.0-beta.1","download_url":"https://x/1.3.0-beta.1.tar.gz"}
		}
	}`)
	got, served := resolveReleaseForChannel(rel, ChannelStable)
	if got.Version != "1.2.3" || served != ChannelStable {
		t.Errorf("stable: got %+v (%s)", got, served)
	}
	got, served = resolveReleaseForChannel(rel, ChannelBeta)
	if got.Version != "1.3.0-beta.1" || served != ChannelBeta {
		t.Errorf("beta: got %+v (%s)", got, served)
	}
}

// Multi-module release.json still narrows via forModule; each entry
// then resolves independently.
func TestResolveMultiModule(t *testing.T) {
	rel := decode(t, `{
		"modules":{
			"mono":{
				"version":"0.3.1","download_url":"https://x/mono-0.3.1.tar.gz",
				"channels":{
					"beta":{"version":"0.4.0-beta.1","download_url":"https://x/mono-0.4.0-beta.1.tar.gz"}
				}
			},
			"mono-voice":{
				"version":"0.3.1","download_url":"https://x/mono-voice-0.3.1.tar.gz"
			}
		}
	}`)
	mono, ok := rel.forModule("mono")
	if !ok {
		t.Fatal("missing mono")
	}
	got, served := resolveReleaseForChannel(mono, ChannelBeta)
	if got.Version != "0.4.0-beta.1" || served != ChannelBeta {
		t.Errorf("mono beta: got %+v (%s)", got, served)
	}

	voice, _ := rel.forModule("mono-voice")
	got, served = resolveReleaseForChannel(voice, ChannelBeta)
	if got.Version != "0.3.1" || served != ChannelStable {
		t.Errorf("voice beta (no beta entry): got %+v (%s)", got, served)
	}
}

// The ChannelPref file is written beside the manager's other state
// (NOT under manager-cache/, which is disposable) and survives round-
// trips. An invalid value is rejected and the previous value stays.
func TestChannelPrefRoundTrip(t *testing.T) {
	dir := t.TempDir()
	cp := NewChannelPref(dir)

	if cp.Channel() != ChannelStable {
		t.Fatalf("default: got %q, want stable", cp.Channel())
	}
	if !cp.SetChannel("beta") {
		t.Fatal("SetChannel(beta) refused")
	}
	if cp.Channel() != ChannelBeta {
		t.Errorf("after set: got %q", cp.Channel())
	}
	if cp.SetChannel("something-else") {
		t.Fatal("SetChannel accepted a bogus channel")
	}
	if cp.Channel() != ChannelBeta {
		t.Errorf("after bogus set: got %q, want beta", cp.Channel())
	}

	// A fresh pref pointed at the same dir must reload the value.
	cp2 := NewChannelPref(dir)
	if cp2.Channel() != ChannelBeta {
		t.Errorf("reloaded pref: got %q, want beta", cp2.Channel())
	}

	// Confirm the file lives where the docs say it does.
	data, err := os.ReadFile(filepath.Join(dir, "manager-config.json"))
	if err != nil {
		t.Errorf("expected manager-config.json to exist: %v", err)
	}
	if !strings.Contains(string(data), `"module_channel"`) {
		t.Errorf("config file missing module_channel key: %s", data)
	}
}

// Render modules.html end to end with a beta available on one module
// and a stable-only version on another. Both channel modes are
// exercised so template action arguments (helper arity, missing map
// keys) get walked, not just parsed.
func TestModulesTemplateRendersWithChannel(t *testing.T) {
	tmpls, err := loadTemplates()
	if err != nil {
		t.Fatal(err)
	}
	tmpl := tmpls["modules.html"]
	if tmpl == nil {
		t.Fatal("modules.html not loaded")
	}
	stable := ChannelEntry{Version: "1.2.3", DownloadURL: "https://x/1.2.3.tar.gz"}
	beta := ChannelEntry{Version: "1.3.0-beta.1", DownloadURL: "https://x/beta.tar.gz"}
	meta := map[string]ReleaseMeta{
		"has-beta": {Version: "1.2.3", Channels: &ChannelSet{Stable: &stable, Beta: &beta}},
		"no-beta":  {Version: "0.9.0"},
	}
	for _, channel := range []string{ChannelStable, ChannelBeta} {
		data := map[string]any{
			"Title":     "Modules",
			"CSRFToken": "csrf",
			"Modules": []CatalogModule{
				{ID: "has-beta", Name: "Has Beta", ComponentType: "sound_generator", GithubRepo: "u/r"},
				{ID: "no-beta", Name: "No Beta", ComponentType: "audio_fx", GithubRepo: "u/r"},
			},
			// Leave "has-beta" uninstalled so both channels render it
			// in the Available section — that's where the beta badge
			// on the offered version and the stable-user teaser both
			// live.
			"Installed":    map[string]InstalledModule{"no-beta": {ID: "no-beta", Version: "0.9.0"}},
			"HasInstalled": true,
			"ReleaseMeta":  meta,
			"Taxonomy":     CatalogTaxonomy{}, "Channel": channel,
		}
		var buf bytes.Buffer
		if err := tmpl.ExecuteTemplate(&buf, "modules.html", data); err != nil {
			t.Fatalf("execute modules.html (%s): %v", channel, err)
		}
		out := buf.String()
		if channel == ChannelStable {
			// Stable users see the teaser but no "beta" badge.
			if !strings.Contains(out, "beta v1.3.0-beta.1 available") {
				t.Errorf("stable: expected teaser in output")
			}
			if strings.Contains(out, `badge-beta`) {
				t.Errorf("stable: unexpected beta badge (would tag stable version)")
			}
		} else {
			// Beta users see the badge and no teaser (they already have it).
			if !strings.Contains(out, `badge-beta`) {
				t.Errorf("beta: expected beta badge")
			}
			if strings.Contains(out, "beta v1.3.0-beta.1 available") {
				t.Errorf("beta: teaser should not appear on beta channel")
			}
		}
	}
}

// versionNewer must treat "0.13.0-beta.1" as older than "0.13.0" —
// the tolerant isNewerSemver classifies the prerelease as newer
// because it has more dotted parts, which would strand a beta user
// on the prerelease after the matching stable cut. This test pins
// the guard.
func TestChannelNewerPrereleaseLosesToBase(t *testing.T) {
	cases := []struct {
		beta, stable string
		want         bool
	}{
		{"0.13.0-beta.1", "0.13.0", false}, // matching stable released
		{"0.13.0-beta.1", "0.12.9", true},  // beta ahead of last stable
		{"0.13.0-beta.2", "0.13.0-beta.1", true},
		{"1.0.0", "1.0.0-rc.5", true},
		{"1.0.0-rc.5", "1.0.0-rc.5", false},
		{"1.0.0-rc.5", "1.0.0", false},
	}
	for _, c := range cases {
		got := versionNewer(c.beta, c.stable)
		if got != c.want {
			t.Errorf("versionNewer(%q, %q) = %v, want %v", c.beta, c.stable, got, c.want)
		}
	}
}

// The host resolver runs on the same code path as modules. These
// tests pin that: a stable-only host is unchanged, and a host with a
// beta ahead of stable serves beta only to beta users.
func TestHostResolveForChannel(t *testing.T) {
	// No channels block — behaves like a pre-channels catalog.
	plain := CatalogHost{LatestVersion: "0.12.1", DownloadURL: "https://x/0.12.1.tar.gz"}
	for _, ch := range []string{ChannelStable, ChannelBeta} {
		v, u, served := hostResolveForChannel(plain, ch)
		if v != "0.12.1" || u != "https://x/0.12.1.tar.gz" || served != ChannelStable {
			t.Errorf("plain %q: got v=%q u=%q served=%q", ch, v, u, served)
		}
	}

	// Beta ahead of stable — only beta users see it.
	stable := ChannelEntry{Version: "0.12.1", DownloadURL: "https://x/0.12.1.tar.gz"}
	beta := ChannelEntry{Version: "0.13.0-beta.1", DownloadURL: "https://x/0.13.0-beta.1.tar.gz"}
	with := CatalogHost{
		LatestVersion: "0.12.1",
		DownloadURL:   "https://x/0.12.1.tar.gz",
		Channels:      &ChannelSet{Stable: &stable, Beta: &beta},
	}
	v, _, served := hostResolveForChannel(with, ChannelStable)
	if v != "0.12.1" || served != ChannelStable {
		t.Errorf("with-beta stable: v=%q served=%q", v, served)
	}
	v, _, served = hostResolveForChannel(with, ChannelBeta)
	if v != "0.13.0-beta.1" || served != ChannelBeta {
		t.Errorf("with-beta beta: v=%q served=%q", v, served)
	}

	// Stable caught up — beta user quietly lands on stable.
	stable2 := ChannelEntry{Version: "0.13.0", DownloadURL: "https://x/0.13.0.tar.gz"}
	caughtUp := CatalogHost{
		LatestVersion: "0.13.0",
		DownloadURL:   "https://x/0.13.0.tar.gz",
		Channels:      &ChannelSet{Stable: &stable2, Beta: &beta},
	}
	v, _, served = hostResolveForChannel(caughtUp, ChannelBeta)
	if v != "0.13.0" || served != ChannelStable {
		t.Errorf("caught-up beta: v=%q served=%q", v, served)
	}
}

// A nil ChannelPref (e.g. tests, non-device builds) must still respond
// Stable to every read and reject writes cleanly.
func TestChannelPrefNilSafe(t *testing.T) {
	var cp *ChannelPref
	if cp.Channel() != ChannelStable {
		t.Errorf("nil.Channel: got %q", cp.Channel())
	}
	if cp.SetChannel("beta") {
		t.Errorf("nil.SetChannel: accepted a write")
	}
}

// The template helper's version calc must agree with the install-time
// resolver — a mismatch would mean the button says one thing and the
// install does another. This test guards against divergence.
func TestChannelVersionMatchesResolver(t *testing.T) {
	stable := ChannelEntry{Version: "1.2.3", DownloadURL: "https://x/1.2.3.tar.gz"}
	beta := ChannelEntry{Version: "1.3.0-beta.1", DownloadURL: "https://x/beta.tar.gz"}
	rm := ReleaseMeta{
		Version:  "1.2.3",
		Channels: &ChannelSet{Stable: &stable, Beta: &beta},
	}
	rel := ReleaseJSON{
		Version:     "1.2.3",
		DownloadURL: "https://x/1.2.3.tar.gz",
		Channels:    &ChannelSet{Stable: &stable, Beta: &beta},
	}
	for _, ch := range []string{ChannelStable, ChannelBeta} {
		resolverGot, _ := resolveReleaseForChannel(rel, ch)
		helperGot := channelVersion(rm, ch)
		if resolverGot.Version != helperGot {
			t.Errorf("channel %q: resolver=%q helper=%q", ch, resolverGot.Version, helperGot)
		}
	}
}

// A beta user must be carried BACK onto stable once stable catches up.
// The resolver half of this was covered from the start
// (TestResolveBetaFallsBackWhenStableCatchesUp); the compare that
// decides whether the Update button is even drawn was not, and it went
// through the raw isNewerSemver — which answers false for
// ("0.13.0", "0.13.0-beta.1"), because it reads "0-beta" as 0 and then
// hands the win to whichever side has more dotted parts. The resolver
// offered the right version to a button nothing rendered.
func TestVersionNewerCarriesBetaUserBackToStable(t *testing.T) {
	cases := []struct {
		name      string
		offered   string
		installed string
		want      bool
	}{
		{"stable supersedes the matching prerelease", "0.13.0", "0.13.0-beta.1", true},
		{"prerelease does not supersede its own base", "0.13.0-beta.1", "0.13.0", false},
		{"later prerelease supersedes an earlier one", "0.13.0-beta.2", "0.13.0-beta.1", true},
		{"beta ahead of installed stable", "1.3.0-beta.1", "1.2.3", true},
		{"same version is not an update", "0.13.0", "0.13.0", false},
		{"older stable is not an update", "0.12.9", "0.13.0", false},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := versionNewer(c.offered, c.installed); got != c.want {
				t.Errorf("versionNewer(%q, %q) = %v, want %v",
					c.offered, c.installed, got, c.want)
			}
		})
	}
}

// The same rollback, exercised through the template helper the Modules
// page actually calls, so a future refactor that reintroduces
// isNewerSemver at this call site fails here rather than on a device.
func TestHasUpdateOffersStableToABetaUser(t *testing.T) {
	hasUpdate, ok := funcMap["hasUpdate"].(func(string, map[string]InstalledModule, map[string]ReleaseMeta, string) bool)
	if !ok {
		t.Fatalf("hasUpdate has an unexpected signature: %T", funcMap["hasUpdate"])
	}
	installed := map[string]InstalledModule{
		"wayward": {Version: "0.13.0-beta.1"},
	}
	meta := map[string]ReleaseMeta{
		"wayward": {
			Version: "0.13.0",
			Channels: &ChannelSet{
				Stable: &ChannelEntry{Version: "0.13.0"},
				Beta:   &ChannelEntry{Version: "0.13.0-beta.1"},
			},
		},
	}
	for _, channel := range []string{ChannelStable, ChannelBeta} {
		if !hasUpdate("wayward", installed, meta, channel) {
			t.Errorf("channel %q: a user on 0.13.0-beta.1 must be offered 0.13.0", channel)
		}
	}
}

// daveboxMeta is davebox's real release history, taken from the GitHub
// API on 2026-09-11. It is the fixture that matters because its tag
// scheme is what breaks a version-string compare: "1.0b2" and "1.0b"
// both parse to [1, 0] and tie, so three consecutive releases never
// drew an Update button for anyone installed at v1.0b.
//
// Note the two SAME-DAY pairs (v1.0b/v1.0b2, beta.7/beta.8). They are
// why the generator emits full timestamps rather than the day strings
// the other metadata fields use — at day resolution these tie again,
// on exactly the comparisons being fixed.
func daveboxMeta() ReleaseMeta {
	return ReleaseMeta{
		Version: "v1.0-beta.8",
		Releases: []ReleaseRef{
			{Tag: "v0.4.0", PublishedAt: "2026-05-15T18:01:54Z"},
			{Tag: "v1.0b", PublishedAt: "2026-05-30T01:47:25Z"},
			{Tag: "v1.0b2", PublishedAt: "2026-05-30T17:16:08Z"},
			{Tag: "v1.0b3", PublishedAt: "2026-05-31T01:37:42Z"},
			{Tag: "v1.0b4", PublishedAt: "2026-06-07T23:00:11Z"},
			{Tag: "v1.0-beta.5", PublishedAt: "2026-06-24T17:46:32Z"},
			{Tag: "v1.0-beta.6", PublishedAt: "2026-06-25T16:19:18Z"},
			{Tag: "v1.0-beta.7", PublishedAt: "2026-07-21T00:14:05Z"},
			{Tag: "v1.0-beta.8", PublishedAt: "2026-07-21T14:48:40Z"},
		},
	}
}

func TestUpdateAvailableOrdersByPublishDate(t *testing.T) {
	rm := daveboxMeta()
	cases := []struct {
		offered, installed string
		want               bool
		why                string
	}{
		{"1.0b2", "1.0b", true, "same day, nine hours apart — the version compare ties here"},
		{"1.0b3", "1.0b2", true, "version compare ties"},
		{"1.0b4", "1.0b3", true, "version compare ties"},
		{"1.0-beta.5", "1.0b4", true, "tag scheme changes mid-history"},
		{"1.0-beta.8", "1.0-beta.7", true, "same day, fourteen hours apart"},
		{"1.0-beta.8", "1.0b", true, "three releases behind"},
		{"1.0-beta.8", "1.0-beta.8", false, "already current"},
		{"1.0b4", "1.0-beta.8", false, "never offer an older release"},
		{"0.4.0", "1.0-beta.8", false, "never offer an older release"},
	}
	for _, c := range cases {
		t.Run(c.offered+"_over_"+c.installed, func(t *testing.T) {
			if got := updateAvailable(rm, c.offered, c.installed); got != c.want {
				t.Errorf("updateAvailable(%q, %q) = %v, want %v (%s)",
					c.offered, c.installed, got, c.want, c.why)
			}
		})
	}
}

// Metadata with no releases[] — a stale manager-cache/ copy, or any
// version the list cannot date — must fall back to the version compare
// rather than reporting "no update" for everything.
func TestUpdateAvailableFallsBackWhenUndatable(t *testing.T) {
	t.Run("no releases list at all", func(t *testing.T) {
		rm := ReleaseMeta{Version: "0.5.0"}
		if !updateAvailable(rm, "0.5.0", "0.4.0") {
			t.Error("with no dates, must fall back to the version compare")
		}
		if updateAvailable(rm, "0.4.0", "0.5.0") {
			t.Error("fallback must not offer an older version")
		}
	})

	t.Run("installed version matches no tag", func(t *testing.T) {
		rm := daveboxMeta()
		// A sideloaded build the catalog has never seen.
		if !updateAvailable(rm, "1.0-beta.8", "0.9.0") {
			t.Error("an undatable installed version must fall back, not block")
		}
	})

	t.Run("beta rollback still works through the fallback", func(t *testing.T) {
		rm := ReleaseMeta{Version: "0.13.0"}
		if !updateAvailable(rm, "0.13.0", "0.13.0-beta.1") {
			t.Error("stable must supersede its own prerelease")
		}
	})
}

// The v prefix lives on the tag and not in module.json, so matching has
// to normalise both sides. 129 of 133 catalogued modules rely on this.
func TestPublishedAtNormalisesTheVPrefix(t *testing.T) {
	rm := daveboxMeta()
	for _, v := range []string{"1.0b4", "v1.0b4"} {
		if got := publishedAt(rm, v); got != "2026-06-07T23:00:11Z" {
			t.Errorf("publishedAt(%q) = %q, want the v1.0b4 timestamp", v, got)
		}
	}
	if got := publishedAt(rm, "9.9.9"); got != "" {
		t.Errorf("an unknown version must be undatable, got %q", got)
	}
}

// A user running a beta build should be able to see that they are. The
// badge is driven by the author's prerelease FLAG, never by the tag
// looking beta-ish — davebox's whole 1.x line is named "beta" and
// marked prerelease on none of it.
func TestInstalledIsPrereleaseUsesTheFlagNotTheName(t *testing.T) {
	rm := ReleaseMeta{
		Releases: []ReleaseRef{
			{Tag: "v0.13.0-beta.1", PublishedAt: "2026-09-02T10:00:00Z", Prerelease: true},
			{Tag: "v0.13.0", PublishedAt: "2026-09-05T10:00:00Z", Prerelease: false},
		},
	}
	cases := []struct {
		version string
		want    bool
		why     string
	}{
		{"0.13.0-beta.1", true, "flagged prerelease"},
		{"v0.13.0-beta.1", true, "v prefix normalised"},
		{"0.13.0", false, "flagged stable"},
		{"0.9.9", false, "unknown version — never guess"},
		{"", false, "empty version"},
	}
	for _, c := range cases {
		if got := installedIsPrerelease(rm, c.version); got != c.want {
			t.Errorf("installedIsPrerelease(%q) = %v, want %v (%s)",
				c.version, got, c.want, c.why)
		}
	}

	// davebox: beta-shaped tags, prerelease flag set on none of them.
	// Badging these would tell every davebox user they are on a beta.
	if installedIsPrerelease(daveboxMeta(), "1.0-beta.8") {
		t.Error("an unflagged release must not be badged from its name")
	}

	// Metadata with no releases[] at all cannot answer.
	if installedIsPrerelease(ReleaseMeta{Version: "0.13.0-beta.1"}, "0.13.0-beta.1") {
		t.Error("without releases[] there is no flag to read; must stay silent")
	}
}
