// Custom install from a GitHub URL, against a multi-module release.json.
//
// Catalog installs always narrowed a release.json through forModule; the
// custom-install path read only the top-level download_url, so a repo
// publishing several modules could only ever install its first one by URL.
// These pin the chooser, the selection, and that the two older shapes
// (single-module, and a multi-module file whose top-level url is the
// courtesy copy for old managers) are untouched.

package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// moduleTarball builds a <id>/module.json tarball, the shape every release ships.
func moduleTarball(t *testing.T, id, componentType string) []byte {
	t.Helper()
	var buf bytes.Buffer
	gz := gzip.NewWriter(&buf)
	tw := tar.NewWriter(gz)
	mj := []byte(`{"id":"` + id + `","name":"` + id + `","component_type":"` + componentType + `"}`)
	for _, h := range []*tar.Header{
		{Name: id + "/", Typeflag: tar.TypeDir, Mode: 0755},
		{Name: id + "/module.json", Typeflag: tar.TypeReg, Mode: 0644, Size: int64(len(mj))},
	} {
		if err := tw.WriteHeader(h); err != nil {
			t.Fatal(err)
		}
		if h.Typeflag == tar.TypeReg {
			if _, err := tw.Write(mj); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := gz.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// fakeGitHub serves release.json for user/repo on main, and a tarball per id
// under /dl/<id>.tar.gz. It records which tarballs were fetched.
type fakeGitHub struct {
	srv        *httptest.Server
	mu         sync.Mutex
	downloaded []string
}

func newFakeGitHub(t *testing.T, releaseJSON func(base string) string, tarballs map[string][]byte) *fakeGitHub {
	t.Helper()
	f := &fakeGitHub{}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.URL.Path == "/user/repo/main/release.json":
			io.WriteString(w, releaseJSON(f.srv.URL))
		case strings.HasPrefix(r.URL.Path, "/dl/"):
			id := strings.TrimSuffix(strings.TrimPrefix(r.URL.Path, "/dl/"), ".tar.gz")
			data, ok := tarballs[id]
			if !ok {
				http.NotFound(w, r)
				return
			}
			f.mu.Lock()
			f.downloaded = append(f.downloaded, id)
			f.mu.Unlock()
			w.Write(data)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(f.srv.Close)

	old := rawGitHubBase
	rawGitHubBase = f.srv.URL
	t.Cleanup(func() { rawGitHubBase = old })
	return f
}

func (f *fakeGitHub) fetched() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.downloaded...)
}

func newCustomInstallApp(t *testing.T) *App {
	t.Helper()
	tmpl, err := loadTemplates()
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("BOOT_TARGETS_DIR", filepath.Join(t.TempDir(), "boot-targets"))
	return &App{
		tmpl:     tmpl,
		basePath: t.TempDir(),
		logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

func postCustomInstall(app *App, form url.Values) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodPost, "/modules/install-custom", strings.NewReader(form.Encode()))
	r.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	w := httptest.NewRecorder()
	app.handleCustomInstall(w, r)
	return w
}

func githubForm(module string) url.Values {
	f := url.Values{"source": {"github"}, "url": {"https://github.com/user/repo"}}
	if module != "" {
		f.Set("module", module)
	}
	return f
}

func assertInstalled(t *testing.T, app *App, subdir, id string) {
	t.Helper()
	p := filepath.Join(app.basePath, "modules", subdir, id, "module.json")
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("%s not installed at %s: %v", id, p, err)
	}
}

const multiRelease = `{
	"version": "0.2.0",
	"download_url": "BASE/dl/module-a.tar.gz",
	"install_path": "modules/tools/module-a",
	"modules": {
		"module-a": {"version": "0.2.0", "download_url": "BASE/dl/module-a.tar.gz"},
		"module-b": {"version": "0.2.1", "download_url": "BASE/dl/module-b.tar.gz"}
	}
}`

func withBase(doc string) func(string) string {
	return func(base string) string { return strings.ReplaceAll(doc, "BASE", base) }
}

func multiTarballs(t *testing.T) map[string][]byte {
	return map[string][]byte{
		"module-a": moduleTarball(t, "module-a", "tool"),
		"module-b": moduleTarball(t, "module-b", "sound_generator"),
	}
}

func TestCustomInstallSingleModuleUnchanged(t *testing.T) {
	gh := newFakeGitHub(t, withBase(`{"version":"1.0.0","download_url":"BASE/dl/solo.tar.gz"}`),
		map[string][]byte{"solo": moduleTarball(t, "solo", "audio_fx")})
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm(""))
	if w.Code != http.StatusSeeOther {
		t.Fatalf("status %d, want 303", w.Code)
	}
	if loc := w.Header().Get("Location"); !strings.Contains(loc, "Installed+solo") {
		t.Fatalf("Location = %q, want the install success flash", loc)
	}
	if got := gh.fetched(); len(got) != 1 || got[0] != "solo" {
		t.Fatalf("downloaded %v, want [solo]", got)
	}
	assertInstalled(t, app, "audio_fx", "solo")
}

func TestCustomInstallMultiModuleShowsChooser(t *testing.T) {
	gh := newFakeGitHub(t, withBase(multiRelease), multiTarballs(t))
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm(""))
	if w.Code != http.StatusOK {
		t.Fatalf("status %d, want 200 (chooser); Location=%q", w.Code, w.Header().Get("Location"))
	}
	body := w.Body.String()
	for _, want := range []string{
		`name="module" value="module-a"`,
		`name="module" value="module-b"`,
		`name="url" value="user/repo"`,
		`name="source" value="github"`,
		`action="/modules/install-custom"`,
		"0.2.1",
	} {
		if !strings.Contains(body, want) {
			t.Errorf("chooser lacks %q", want)
		}
	}
	// Sorted by id, so the page is stable across map iteration order.
	if strings.Index(body, `value="module-a"`) > strings.Index(body, `value="module-b"`) {
		t.Error("chooser rows are not sorted by id")
	}
	if got := gh.fetched(); len(got) != 0 {
		t.Fatalf("chooser downloaded %v; it must install nothing", got)
	}
}

func TestCustomInstallMultiModuleInstallsChosen(t *testing.T) {
	gh := newFakeGitHub(t, withBase(multiRelease), multiTarballs(t))
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm("module-b"))
	if w.Code != http.StatusSeeOther {
		t.Fatalf("status %d, want 303", w.Code)
	}
	if loc := w.Header().Get("Location"); !strings.Contains(loc, "Installed+module-b") {
		t.Fatalf("Location = %q, want module-b installed", loc)
	}
	// The top-level download_url points at module-a; downloading that
	// would be the old behaviour.
	if got := gh.fetched(); len(got) != 1 || got[0] != "module-b" {
		t.Fatalf("downloaded %v, want [module-b]", got)
	}
	assertInstalled(t, app, "sound_generators", "module-b")
}

func TestCustomInstallMultiModuleUnknownID(t *testing.T) {
	gh := newFakeGitHub(t, withBase(multiRelease), multiTarballs(t))
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm("module-nope"))
	if w.Code != http.StatusSeeOther {
		t.Fatalf("status %d, want 303", w.Code)
	}
	loc := w.Header().Get("Location")
	if !strings.HasPrefix(loc, "/modules?") || !strings.Contains(loc, "flash=") ||
		!strings.Contains(loc, "module-nope") {
		t.Fatalf("Location = %q, want a /modules flash naming the id", loc)
	}
	if got := gh.fetched(); len(got) != 0 {
		t.Fatalf("unknown id downloaded %v", got)
	}
}

func TestCustomInstallModulesOnlyAccepted(t *testing.T) {
	doc := `{"modules": {
		"module-a": {"version": "0.2.0", "download_url": "BASE/dl/module-a.tar.gz"},
		"module-b": {"version": "0.2.0", "download_url": "BASE/dl/module-b.tar.gz"}
	}}`
	newFakeGitHub(t, withBase(doc), multiTarballs(t))
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm(""))
	if w.Code != http.StatusOK {
		t.Fatalf("status %d, Location=%q: a modules-only release.json must count as found",
			w.Code, w.Header().Get("Location"))
	}
	if !strings.Contains(w.Body.String(), `value="module-b"`) {
		t.Fatal("chooser does not list the modules-only file's ids")
	}

	w = postCustomInstall(app, githubForm("module-a"))
	if loc := w.Header().Get("Location"); !strings.Contains(loc, "Installed+module-a") {
		t.Fatalf("Location = %q, want module-a installed", loc)
	}
	assertInstalled(t, app, "tools", "module-a")
}

// One module in the map: nothing to choose, install it.
func TestCustomInstallSingleEntryMapInstallsWithoutAsking(t *testing.T) {
	doc := `{"modules": {"module-b": {"version": "0.2.0", "download_url": "BASE/dl/module-b.tar.gz"}}}`
	gh := newFakeGitHub(t, withBase(doc), multiTarballs(t))
	app := newCustomInstallApp(t)

	w := postCustomInstall(app, githubForm(""))
	if loc := w.Header().Get("Location"); !strings.Contains(loc, "Installed+module-b") {
		t.Fatalf("status %d Location = %q, want module-b installed directly", w.Code, loc)
	}
	if got := gh.fetched(); len(got) != 1 || got[0] != "module-b" {
		t.Fatalf("downloaded %v, want [module-b]", got)
	}
}
