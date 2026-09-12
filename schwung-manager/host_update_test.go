// The host update check, and the stale-catalog condition that made it lie.
//
// Field report: a device running 1.4.0 showed "1.0.0 available" with an
// Upgrade button. Nothing was wrong with the published catalog — the
// device could not reach it, so the manager served its untimed on-disk
// cache, saved before 2026-08-31 when host latest_version WAS 1.0.0. The
// check was `offered != installed`, which is not a version comparison, so
// an offered downgrade rendered as an update.

package main

import (
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestHostOfferIsUpdate(t *testing.T) {
	cases := []struct {
		name      string
		offered   string
		installed string
		want      bool
	}{
		// The regression. Was true — an Upgrade button pointing at the
		// v1.0.0 tarball, one click from downgrading a 1.4.0 device.
		{"stale catalog offers an older host", "1.0.0", "1.4.0", false},
		{"stale catalog offers an older patch", "1.3.3", "1.4.0", false},
		{"up to date", "1.4.0", "1.4.0", false},
		{"newer minor", "1.5.0", "1.4.0", true},
		{"newer patch", "1.4.1", "1.4.0", true},
		{"newer major", "2.0.0", "1.4.0", true},
		{"v prefix on either side is not a difference", "v1.4.0", "1.4.0", false},
		// Beta users: the resolver hands us the beta build, and the
		// prerelease ordering is versionNewer's, not ours.
		{"beta ahead of installed stable", "1.5.0-beta.1", "1.4.0", true},
		{"beta behind installed stable", "1.4.0-beta.1", "1.4.0", false},
		{"later beta over earlier beta", "1.5.0-beta.2", "1.5.0-beta.1", true},
		{"earlier beta over later beta", "1.5.0-beta.1", "1.5.0-beta.2", false},
		// An unreadable version.txt still gets offered the update: there
		// is nothing to compare, and refusing would strand a device that
		// cannot say what it runs.
		{"unknown installed version still offers", "1.4.0", "unknown", true},
		// Nothing to offer, or nothing to compare against.
		{"no offer", "", "1.4.0", false},
		{"no installed version", "1.4.0", "", false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := hostOfferIsUpdate(tc.offered, tc.installed); got != tc.want {
				t.Errorf("hostOfferIsUpdate(%q, %q) = %v, want %v",
					tc.offered, tc.installed, got, tc.want)
			}
		})
	}
}

func TestHumanizeAge(t *testing.T) {
	cases := []struct {
		d    time.Duration
		want string
	}{
		{30 * time.Second, "just now"},
		{time.Minute, "1 minute ago"},
		{90 * time.Second, "1 minute ago"},
		{45 * time.Minute, "45 minutes ago"},
		{time.Hour, "1 hour ago"},
		{5 * time.Hour, "5 hours ago"},
		{24 * time.Hour, "1 day ago"},
		{15 * 24 * time.Hour, "15 days ago"},
	}
	for _, tc := range cases {
		if got := humanizeAge(tc.d); got != tc.want {
			t.Errorf("humanizeAge(%v) = %q, want %q", tc.d, got, tc.want)
		}
	}
}

// writeCatalogCache plants a catalog in the on-disk cache, backdated, the
// way a device that fetched successfully weeks ago would have it.
func writeCatalogCache(t *testing.T, base, body string, age time.Duration) string {
	t.Helper()
	dir := filepath.Join(base, "manager-cache")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "catalog.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	when := time.Now().Add(-age)
	if err := os.Chtimes(path, when, when); err != nil {
		t.Fatal(err)
	}
	return path
}

const catalogHost140 = `{"catalog_version":2,"host":{"name":"Schwung",` +
	`"github_repo":"charlesvestal/schwung","latest_version":"1.4.0",` +
	`"download_url":"https://example.invalid/v1.4.0/schwung.tar.gz"},"modules":[]}`

// The host latest_version this catalog carried before 2026-08-31.
const catalogHost100 = `{"catalog_version":2,"host":{"name":"Schwung",` +
	`"github_repo":"charlesvestal/schwung","latest_version":"1.0.0",` +
	`"download_url":"https://example.invalid/v1.0.0/schwung.tar.gz"},"modules":[]}`

func TestCatalogStatusProvenance(t *testing.T) {
	t.Run("served from disk when the fetch fails", func(t *testing.T) {
		base := t.TempDir()
		writeCatalogCache(t, base, catalogHost100, 14*24*time.Hour)
		// A server that refuses, standing in for no network at all.
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			http.Error(w, "nope", http.StatusServiceUnavailable)
		}))
		defer srv.Close()

		cs := NewCatalogService(srv.URL, srv.URL, base)
		cat, err := cs.Fetch()
		if err == nil {
			t.Fatal("want a fetch error")
		}
		if cat == nil {
			t.Fatal("want the disk cache served despite the error")
		}
		st := cs.Status()
		if st.Live {
			t.Error("Status().Live is true after a failed fetch: a caller cannot " +
				"tell it is rendering month-old data")
		}
		if got := time.Since(st.At); got < 13*24*time.Hour {
			t.Errorf("Status().At is %v old, want ~14 days (the cache file's mtime)", got)
		}
	})

	t.Run("live after a successful fetch", func(t *testing.T) {
		base := t.TempDir()
		writeCatalogCache(t, base, catalogHost100, 14*24*time.Hour)
		srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			w.Write([]byte(catalogHost140))
		}))
		defer srv.Close()

		cs := NewCatalogService(srv.URL, srv.URL, base)
		cat, err := cs.Fetch()
		if err != nil {
			t.Fatalf("Fetch: %v", err)
		}
		if cat.Host.LatestVersion != "1.4.0" {
			t.Errorf("served %q, want the fetched 1.4.0 over the cached 1.0.0",
				cat.Host.LatestVersion)
		}
		st := cs.Status()
		if !st.Live {
			t.Error("Status().Live is false after a successful fetch")
		}
		if time.Since(st.At) > time.Minute {
			t.Errorf("Status().At is %v old after a live fetch, want ~now", time.Since(st.At))
		}
	})
}

// newModulesTestApp builds the App that handleModules actually needs.
func newModulesTestApp(t *testing.T, catalogURL, base, hostVersion string) *App {
	t.Helper()
	if err := os.MkdirAll(filepath.Join(base, "host"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "host", "version.txt"),
		[]byte(hostVersion+"\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	tmpl, err := loadTemplates()
	if err != nil {
		t.Fatal(err)
	}
	return &App{
		tmpl:        tmpl,
		catalogSvc:  NewCatalogService(catalogURL, catalogURL, base),
		channelPref: NewChannelPref(base),
		basePath:    base,
		logger:      slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

// The whole defect, rendered. Unit-testing the comparison alone would not
// have caught it: the bug was in what the handler COMPOSED, and the page is
// the only place the two halves meet.
func TestModulesPageOnStaleCatalog(t *testing.T) {
	base := t.TempDir()
	writeCatalogCache(t, base, catalogHost100, 14*24*time.Hour)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "nope", http.StatusServiceUnavailable)
	}))
	defer srv.Close()

	app := newModulesTestApp(t, srv.URL, base, "1.4.0")
	rec := httptest.NewRecorder()
	app.handleModules(rec, httptest.NewRequest(http.MethodGet, "/modules", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("status %d, want 200", rec.Code)
	}
	body := rec.Body.String()

	if strings.Contains(body, "1.0.0 available") {
		t.Error("page offers v1.0.0 as an update over an installed 1.4.0 — " +
			"the stale-catalog downgrade this fix exists to prevent")
	}
	if strings.Contains(body, "Upgrade to v1.0.0") {
		t.Error("page renders an Upgrade button for an older host version")
	}
	if !strings.Contains(body, "Offline") {
		t.Error("page renders disk-cached data with no indication it is offline; " +
			"that silence is what made a month-old version read as a real release")
	}
	if !strings.Contains(body, "14 days ago") {
		t.Error("offline notice does not say how old the cached catalog is")
	}
	// The page must still WORK offline — the disk cache exists so users can
	// repair and remove modules with no network.
	if !strings.Contains(body, "Schwung Host") {
		t.Error("page did not render its host summary at all")
	}
	if !strings.Contains(body, "1.4.0") {
		t.Error("page does not report the installed version")
	}
}

// The live case, same handler: a genuinely newer host still gets offered,
// and the offline notice stays off.
func TestModulesPageOffersNewerHost(t *testing.T) {
	base := t.TempDir()
	catalog := strings.Replace(catalogHost140, `"latest_version":"1.4.0"`,
		`"latest_version":"1.5.0"`, 1)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(catalog))
	}))
	defer srv.Close()

	app := newModulesTestApp(t, srv.URL, base, "1.4.0")
	rec := httptest.NewRecorder()
	app.handleModules(rec, httptest.NewRequest(http.MethodGet, "/modules", nil))

	body := rec.Body.String()
	if !strings.Contains(body, "v1.5.0 available") {
		t.Error("a newer host version is not offered as an update")
	}
	if strings.Contains(body, "Offline") {
		t.Error("offline notice shown for a live catalog fetch")
	}
}
