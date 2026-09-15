// "Check for Update" must actually check, and the cache must survive being
// read by one request while another writes it.

package main

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

// countingCatalog serves the catalog and counts how many times it is asked.
//
// The catalog and the release metadata are separate paths, and only the
// catalog is counted: a successful fetch pulls BOTH, so pointing the two
// URLs at one counting handler doubles every number and the test reads as
// a TTL failure that is really an arithmetic one.
func countingCatalog(t *testing.T, body string) (catalogURL, metaURL string, hits *atomic.Int32) {
	t.Helper()
	var n atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/catalog.json", func(w http.ResponseWriter, r *http.Request) {
		n.Add(1)
		w.Write([]byte(body))
	})
	mux.HandleFunc("/meta.json", func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{}`))
	})
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv.URL + "/catalog.json", srv.URL + "/meta.json", &n
}

func TestFetchHonoursTTLAndRefreshBypassesIt(t *testing.T) {
	catURL, metaURL, hits := countingCatalog(t, catalogHost140)
	cs := NewCatalogService(catURL, metaURL, t.TempDir())

	if _, err := cs.Fetch(); err != nil {
		t.Fatalf("first Fetch: %v", err)
	}
	if _, err := cs.Fetch(); err != nil {
		t.Fatalf("second Fetch: %v", err)
	}
	// The TTL is the point of the cache — a page render must not hit the
	// network every time.
	if got := hits.Load(); got != 1 {
		t.Errorf("two Fetch calls made %d requests, want 1 (TTL not honoured)", got)
	}

	// Refresh is the user pressing a button that says it checks.
	if _, err := cs.Refresh(); err != nil {
		t.Fatalf("Refresh: %v", err)
	}
	if got := hits.Load(); got != 2 {
		t.Errorf("Refresh made no new request (%d total): "+
			"\"Check for Update\" reports a cached answer as though it looked", got)
	}
}

// A failed Refresh must leave the last-known-good catalog in place — the
// button reports the failure, it does not blank the pages that follow.
func TestRefreshFailureKeepsCacheAndMarksStale(t *testing.T) {
	base := t.TempDir()
	var fail atomic.Bool
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if fail.Load() {
			http.Error(w, "nope", http.StatusServiceUnavailable)
			return
		}
		w.Write([]byte(catalogHost140))
	}))
	defer srv.Close()

	cs := NewCatalogService(srv.URL, srv.URL, base)
	if _, err := cs.Fetch(); err != nil {
		t.Fatalf("Fetch: %v", err)
	}
	if !cs.Status().Live {
		t.Fatal("not live after a successful fetch")
	}

	fail.Store(true)
	cat, err := cs.Refresh()
	if err == nil {
		t.Error("Refresh past a failing server returned no error")
	}
	if cat == nil || cat.Host.LatestVersion != "1.4.0" {
		t.Error("a failed Refresh dropped the last-known-good catalog")
	}
	if cs.Status().Live {
		t.Error("Status().Live still true after a failed Refresh")
	}
}

// The button, not the service. Swapping the handler back to Fetch leaves
// every service-level test above green -- the TTL behaviour it depends on
// is correct, and the defect is which method the handler CALLS.
func TestCheckUpdateButtonActuallyChecks(t *testing.T) {
	base := t.TempDir()
	catURL, metaURL, hits := countingCatalog(t, catalogHost140)
	app := newModulesTestApp(t, catURL, base, "1.4.0")
	app.catalogSvc = NewCatalogService(catURL, metaURL, base)

	// Some other page rendered first, seeding the 5-minute TTL. This is
	// the ordinary case: the user browses, THEN presses the button.
	if _, err := app.catalogSvc.Fetch(); err != nil {
		t.Fatalf("priming Fetch: %v", err)
	}
	before := hits.Load()

	rec := httptest.NewRecorder()
	app.handleSystemCheckUpdate(rec, httptest.NewRequest(http.MethodPost, "/system/check-update", nil))

	if rec.Code != http.StatusSeeOther {
		t.Fatalf("status %d, want a redirect", rec.Code)
	}
	if got := hits.Load(); got == before {
		t.Errorf("Check for Update made no request (%d before, %d after): it "+
			"reported a cached answer as though it had looked", before, got)
	}
	if loc := rec.Header().Get("Location"); !strings.Contains(loc, "1.4.0") {
		t.Errorf("redirect %q does not name the version it found", loc)
	}
}

// Run with -race. Before the mutex, every mutable field of CatalogService
// was written by one request's fetch while another request read it —
// net/http gives each request its own goroutine, and any handler may reach
// for the catalog.
func TestCatalogServiceConcurrentAccess(t *testing.T) {
	catURL, metaURL, _ := countingCatalog(t, catalogHost140)
	cs := NewCatalogService(catURL, metaURL, t.TempDir())

	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			for n := 0; n < 20; n++ {
				switch (i + n) % 4 {
				case 0:
					cs.Fetch()
				case 1:
					cs.Refresh() // forces the write path to overlap the reads
				case 2:
					cs.Status()
				case 3:
					for id := range cs.GetReleaseMeta() {
						_ = id
					}
				}
			}
		}(i)
	}
	wg.Wait()
}
