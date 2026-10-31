package main

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// A module whose web page is called index.html -- most visibly one that ships
// "web_ui": "assets/index.html" -- got a 404 from the Remote UI.
//
// http.ServeFile has a special case: a request whose path ENDS in
// "/index.html" is answered with a redirect to the same path without it
// ("./"). The browser follows it, the handler is then asked for a directory,
// and a handler that (rightly) refuses directories says 404. So the page was
// served by every other file name in the fleet and by none called index.html.
//
// These tests drive the real route through a mux, the way the browser does,
// and follow nothing: the answer to the FIRST request must be the file.

const webUIAssetRoute = "GET /api/remote-ui/module-assets/{id}/{filepath...}"

func webUIAssetApp(t *testing.T, files map[string]string) (*App, http.Handler) {
	t.Helper()
	app := newTestApp(t, "")
	modDir := filepath.Join(app.basePath, "modules", "demo")
	for rel, body := range files {
		full := filepath.Join(modDir, rel)
		if err := os.MkdirAll(filepath.Dir(full), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(full, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	mux := http.NewServeMux()
	mux.HandleFunc(webUIAssetRoute, app.handleModuleWebUIAsset)
	return app, mux
}

func getWebUIAsset(h http.Handler, path string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(http.MethodGet, "/api/remote-ui/module-assets/demo/"+path, nil)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	return rec
}

func TestModuleWebUIAssetServesIndexHTML(t *testing.T) {
	_, h := webUIAssetApp(t, map[string]string{
		"assets/index.html": "<html>assets index</html>",
		"index.html":        "<html>root index</html>",
		"web_ui.html":       "<html>web ui</html>",
	})
	cases := map[string]string{
		"assets/index.html": "<html>assets index</html>",
		"index.html":        "<html>root index</html>",
		"web_ui.html":       "<html>web ui</html>",
	}
	for path, want := range cases {
		rec := getWebUIAsset(h, path, nil)
		if rec.Code != http.StatusOK {
			t.Errorf("%s: status %d (Location %q), want 200 with the file itself",
				path, rec.Code, rec.Header().Get("Location"))
			continue
		}
		if rec.Body.String() != want {
			t.Errorf("%s: body %q, want %q", path, rec.Body.String(), want)
		}
		if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "text/html") {
			t.Errorf("%s: Content-Type %q, want text/html", path, ct)
		}
	}
}

// What the handler promises about every module page must hold for index.html
// too: framed by the Remote UI's own origin, and never cached.
func TestModuleWebUIAssetKeepsHeaders(t *testing.T) {
	_, h := webUIAssetApp(t, map[string]string{"assets/index.html": "x"})
	rec := getWebUIAsset(h, "assets/index.html", nil)
	if got := rec.Header().Get("X-Frame-Options"); got != "SAMEORIGIN" {
		t.Errorf("X-Frame-Options %q, want SAMEORIGIN", got)
	}
	if got := rec.Header().Get("Cache-Control"); got != "no-store" {
		t.Errorf("Cache-Control %q, want no-store", got)
	}
}

// ServeFile answered Range requests and conditional requests; the replacement
// must too, or an audio/video asset a module page streams would stop seeking.
func TestModuleWebUIAssetStillSupportsRange(t *testing.T) {
	_, h := webUIAssetApp(t, map[string]string{"assets/clip.bin": "0123456789"})
	rec := getWebUIAsset(h, "assets/clip.bin", map[string]string{"Range": "bytes=2-4"})
	if rec.Code != http.StatusPartialContent {
		t.Fatalf("status %d, want 206", rec.Code)
	}
	if rec.Body.String() != "234" {
		t.Errorf("range body %q, want 234", rec.Body.String())
	}
}

// What was refused stays refused: directories, missing files, traversal,
// another module's tree.
func TestModuleWebUIAssetStillRefuses(t *testing.T) {
	app, h := webUIAssetApp(t, map[string]string{
		"assets/index.html": "x",
		"web_ui.html":       "x",
	})
	// A file outside the module, reachable by a symlink inside it.
	outside := filepath.Join(app.basePath, "secret.txt")
	if err := os.WriteFile(outside, []byte("secret"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(outside, filepath.Join(app.basePath, "modules", "demo", "leak.txt")); err != nil {
		t.Fatal(err)
	}
	for _, path := range []string{
		"assets",         // a directory, no trailing slash
		"assets/",        // a directory with one
		"missing.html",   // nothing there
		"assets/nope.js", // nothing there
		"leak.txt",       // a symlink out of the module directory
	} {
		if rec := getWebUIAsset(h, path, nil); rec.Code != http.StatusNotFound {
			t.Errorf("%q: status %d, want 404", path, rec.Code)
		}
	}
	// ".." in the file path never reaches the filesystem.
	req := httptest.NewRequest(http.MethodGet, "/api/remote-ui/module-assets/demo/assets/..%2Fweb_ui.html", nil)
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("a .. in the path: status %d, want 404", rec.Code)
	}
	// An unknown module.
	req = httptest.NewRequest(http.MethodGet, "/api/remote-ui/module-assets/nodemo/web_ui.html", nil)
	rec = httptest.NewRecorder()
	h.ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Errorf("an unknown module: status %d, want 404", rec.Code)
	}
}
