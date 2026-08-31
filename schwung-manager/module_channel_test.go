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

// The ChannelPref file is written to manager-cache and survives round-
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
	data, err := os.ReadFile(filepath.Join(dir, "manager-cache", "manager-config.json"))
	if err != nil {
		t.Errorf("expected manager-cache/manager-config.json to exist: %v", err)
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
			"Channel":      channel,
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

