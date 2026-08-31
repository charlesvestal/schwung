// Where a module install/update sends you afterwards.
//
// moduleRedirect used to read only the Referer, and every response sets
// `Referrer-Policy: no-referrer` — so the header is always absent and the
// `/modules/<id>` fallback fired on every install. Installing from the module
// list therefore dumped you on that module's detail page, which is the whole
// complaint: with a filter set and several modules to install, you lost your
// place every time.
//
// The form now states where it came from. These pin the parts that fail
// quietly: the precedence, and the refusal of a return_to that points off this
// server.

package main

import (
	"html/template"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

func TestSafeReturnToRejectsOffSiteDestinations(t *testing.T) {
	for _, bad := range []string{
		"",                       // absent
		"https://evil.example/x", // absolute
		"//evil.example/x",       // protocol-relative — a browser leaves the host
		"/\\evil.example",        // backslash smuggling
		"modules",                // not rooted
		" /modules",              // leading space, not rooted
	} {
		if got := safeReturnTo(bad); got != "" {
			t.Errorf("safeReturnTo(%q) = %q, want \"\" (refused)", bad, got)
		}
	}
	for _, good := range []string{"/modules", "/modules?sort=az", "/"} {
		if got := safeReturnTo(good); got != good {
			t.Errorf("safeReturnTo(%q) = %q, want it accepted", good, got)
		}
	}
}

// The regression itself: no Referer (which is ALWAYS the case here), but the
// form says where it came from, so we must not fall back to the detail page.
func TestModuleRedirectPrefersReturnTo(t *testing.T) {
	app := &App{}

	form := url.Values{"return_to": {"/modules"}}
	r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install",
		strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	// Deliberately NO Referer — that is the live configuration.
	w := httptest.NewRecorder()

	app.moduleRedirect(w, r, "minijv", "ok", flashSuccess)

	loc := w.Header().Get("Location")
	if !strings.HasPrefix(loc, "/modules?flash=") {
		t.Fatalf("Location = %q, want a redirect back to /modules", loc)
	}
	if strings.Contains(loc, "/modules/minijv") {
		t.Fatalf("Location = %q — fell back to the module detail page, which is the bug", loc)
	}
}

// Without a return_to and without a Referer there is nothing better to do than
// the detail page, so that behaviour must survive.
func TestModuleRedirectFallsBackWhenNothingSaysOtherwise(t *testing.T) {
	app := &App{}
	r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install", nil)
	w := httptest.NewRecorder()

	app.moduleRedirect(w, r, "minijv", "ok", flashSuccess)

	if loc := w.Header().Get("Location"); !strings.HasPrefix(loc, "/modules/minijv?flash=") {
		t.Fatalf("Location = %q, want the detail-page fallback", loc)
	}
}

// An off-site return_to must be ignored rather than followed.
func TestModuleRedirectIgnoresOffSiteReturnTo(t *testing.T) {
	app := &App{}
	form := url.Values{"return_to": {"https://evil.example/"}}
	r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install",
		strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	w := httptest.NewRecorder()

	app.moduleRedirect(w, r, "minijv", "ok", flashSuccess)

	loc := w.Header().Get("Location")
	if strings.Contains(loc, "evil.example") {
		t.Fatalf("Location = %q — followed an off-site return_to", loc)
	}
}

// An absolute same-origin Referer still works, for a deployment that relaxed
// the policy: reducing it to a path must not turn into the detail-page fallback.
func TestModuleRedirectAcceptsAbsoluteReferer(t *testing.T) {
	app := &App{}
	r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install", nil)
	r.Header.Set("Referer", "http://move.local:7700/modules?sort=az")
	w := httptest.NewRecorder()

	app.moduleRedirect(w, r, "minijv", "ok", flashSuccess)

	loc := w.Header().Get("Location")
	if !strings.HasPrefix(loc, "/modules?sort=az") {
		t.Fatalf("Location = %q, want the Referer's path and query preserved", loc)
	}
}

// Installing several in a row must not stack flashes on the URL.
func TestModuleRedirectDoesNotStackFlashes(t *testing.T) {
	app := &App{}
	form := url.Values{"return_to": {"/modules?flash=Something+installed"}}
	r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install",
		strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	w := httptest.NewRecorder()

	app.moduleRedirect(w, r, "minijv", "ok", flashSuccess)

	if loc := w.Header().Get("Location"); strings.Count(loc, "flash=") != 1 {
		t.Fatalf("Location = %q, want exactly one flash", loc)
	}
}

// The toast's colour must survive the redirect, so a failed install cannot
// arrive looking like a successful one — which is what happened while every
// flash was rendered as "info".
func TestModuleRedirectCarriesFlashKind(t *testing.T) {
	for _, tc := range []struct{ kind, want string }{
		{flashSuccess, "flash_type=success"},
		{flashError, "flash_type=error"},
	} {
		app := &App{}
		form := url.Values{"return_to": {"/modules"}}
		r := httptest.NewRequest(http.MethodPost, "/modules/minijv/install",
			strings.NewReader(form.Encode()))
		r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
		w := httptest.NewRecorder()

		app.moduleRedirect(w, r, "minijv", "msg", tc.kind)

		if loc := w.Header().Get("Location"); !strings.Contains(loc, tc.want) {
			t.Errorf("kind %q: Location = %q, want it to carry %q", tc.kind, loc, tc.want)
		}
	}
}

// Only the known kinds become a CSS class, so a hand-typed flash_type cannot
// inject one.
func TestRenderFlashTypeIsAllowlisted(t *testing.T) {
	app := &App{
		tmpl:   map[string]*template.Template{"t.html": template.Must(template.New("t.html").Parse("ok"))},
		logger: slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
	for _, tc := range []struct{ q, want string }{
		{"success", "success"},
		{"error", "error"},
		{"", "info"},
		{"evil\" onload=x", "info"},
		{"warning", "info"}, // not wired up yet — must not pass through
	} {
		r := httptest.NewRequest(http.MethodGet, "/modules?flash=hi&flash_type="+url.QueryEscape(tc.q), nil)
		data := map[string]any{}
		w := httptest.NewRecorder()
		app.render(w, r, "t.html", data)
		if got := data["FlashType"]; got != tc.want {
			t.Errorf("flash_type=%q -> FlashType %v, want %q", tc.q, got, tc.want)
		}
	}
}

// Uninstall keeps you where you were, but its fallback is the LIST, never the
// module's detail page: a sideloaded module has no detail page once removed,
// so moduleRedirect's fallback would 404.
func TestUninstallRedirectDest(t *testing.T) {
	// No return_to: the fallback.
	r := httptest.NewRequest(http.MethodPost, "/modules/gone/uninstall", nil)
	got := uninstallRedirectDest(r)
	if strings.Contains(got, "/modules/gone") {
		t.Fatalf("dest = %q -- a removed module's detail page 404s", got)
	}
	if !strings.HasPrefix(got, "/modules?") {
		t.Fatalf("dest = %q, want the modules list", got)
	}
	if !strings.Contains(got, "flash_type="+flashSuccess) {
		t.Errorf("dest = %q, want a success-coloured toast", got)
	}

	// With a return_to, the filtered list is preserved.
	form := url.Values{"return_to": {"/modules?sort=az"}}
	r2 := httptest.NewRequest(http.MethodPost, "/modules/gone/uninstall",
		strings.NewReader(form.Encode()))
	r2.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	if got := uninstallRedirectDest(r2); !strings.HasPrefix(got, "/modules?sort=az&flash=") {
		t.Errorf("dest = %q, want the return_to preserved with the flash appended", got)
	}

	// An off-site return_to is refused here too.
	form3 := url.Values{"return_to": {"//evil.example/"}}
	r3 := httptest.NewRequest(http.MethodPost, "/modules/gone/uninstall",
		strings.NewReader(form3.Encode()))
	r3.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	if got := uninstallRedirectDest(r3); strings.Contains(got, "evil.example") {
		t.Errorf("dest = %q -- followed an off-site return_to", got)
	}
}
