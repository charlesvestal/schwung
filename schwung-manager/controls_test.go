package main

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type fakeControlsParams map[string]string

func (f fakeControlsParams) GetParam(slot uint8, key string) (string, error) {
	v, ok := f[string(rune('0'+slot))+"|"+key]
	if !ok {
		return "", errors.New("timeout")
	}
	return v, nil
}

func controlsTestApp(t *testing.T) (*App, string) {
	t.Helper()
	app := newTestApp(t, "")
	uuid := "set-1"
	if err := os.WriteFile(filepath.Join(app.basePath, "active_set.txt"), []byte(uuid+"\nMy Set\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	dir := filepath.Join(app.basePath, "set_state", uuid)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	return app, dir
}

func putControls(app *App, body any) *httptest.ResponseRecorder {
	b, _ := json.Marshal(body)
	req := httptest.NewRequest(http.MethodPut, "/api/controls", strings.NewReader(string(b)))
	rec := httptest.NewRecorder()
	app.handleControlsPut(rec, req)
	return rec
}

// A write lands only if it was based on the text on disk: the device edits
// the same file (learn), and a stale page must never overwrite it.
func TestControlsPutRequiresCurrentETag(t *testing.T) {
	app, dir := controlsTestApp(t)
	doc, err := app.loadControlsDoc(nil)
	if err != nil {
		t.Fatal(err)
	}
	if doc.Set.Name != "My Set" || doc.Text != "" {
		t.Fatalf("no document yet should read as empty text, got %+v", doc)
	}
	rec := putControls(app, controlsPut{UUID: "set-1", ETag: doc.ETag, Text: `{"version":1,"cc":[]}`})
	if rec.Code != http.StatusOK {
		t.Fatalf("first write: %d %s", rec.Code, rec.Body)
	}
	got, _ := os.ReadFile(filepath.Join(dir, controlsFile))
	if string(got) != `{"version":1,"cc":[]}` {
		t.Fatalf("file = %q", got)
	}
	// The device writes in between: the old etag is now stale.
	os.WriteFile(filepath.Join(dir, controlsFile), []byte(`{"version":1,"cc":[{"cc":5}]}`), 0o644)
	rec = putControls(app, controlsPut{UUID: "set-1", ETag: doc.ETag, Text: `{"version":1}`})
	if rec.Code != http.StatusConflict {
		t.Fatalf("stale etag: want 409, got %d", rec.Code)
	}
	got, _ = os.ReadFile(filepath.Join(dir, controlsFile))
	if !strings.Contains(string(got), `"cc":5`) {
		t.Fatalf("a refused write must leave the device's edit alone: %q", got)
	}
	// Another set loaded meanwhile: refused too.
	cur, _ := app.loadControlsDoc(nil)
	rec = putControls(app, controlsPut{UUID: "set-2", ETag: cur.ETag, Text: `{}`})
	if rec.Code != http.StatusConflict {
		t.Fatalf("wrong set: want 409, got %d", rec.Code)
	}
	// Not a JSON object: refused, never written.
	rec = putControls(app, controlsPut{UUID: "set-1", ETag: cur.ETag, Text: `[1,2]`})
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("non-object: want 400, got %d", rec.Code)
	}
	entries, _ := os.ReadDir(dir)
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), ".controls-") {
			t.Fatalf("temp file left behind: %s", e.Name())
		}
	}
}

// The chain comes from the running device when it answers in full, and from
// the set's saved files otherwise -- never half of each.
func TestControlsChainLiveOrSaved(t *testing.T) {
	app, dir := controlsTestApp(t)
	live := fakeControlsParams{
		"0|synth_module": "obxd", "0|fx_count": "2", "0|fx1_module": "freeverb", "0|fx2_module": "",
		"0|midi_fx_count": "0",
		"0|master_fx:modules": `[{"id":"cloudseed","path":"/x/cloudseed/cloudseed.so"},{"id":"","path":""}]`,
	}
	for _, s := range []string{"1", "2", "3"} {
		live[s+"|synth_module"] = ""
		live[s+"|fx_count"] = "0"
		live[s+"|midi_fx_count"] = ""
	}
	doc, err := app.loadControlsDoc(live)
	if err != nil {
		t.Fatal(err)
	}
	c := doc.Chain
	if !c.Live || c.Slots[0].Synth != "obxd" || len(c.Slots[0].FX) != 2 || c.Slots[0].FX[0] != "freeverb" ||
		c.MasterFX[0] != "cloudseed" || len(c.MasterFX) != controlsMasterFx {
		t.Fatalf("live chain wrong: %+v", c)
	}
	// One read times out: the whole live answer is dropped for the files.
	delete(live, "2|synth_module")
	os.WriteFile(filepath.Join(dir, "slot_0.json"), []byte(`{"synth":{"module":"dx7"}}`), 0o644)
	doc, _ = app.loadControlsDoc(live)
	if doc.Chain.Live {
		t.Fatalf("a partial live read must not be used")
	}
	if len(doc.Chain.Slots) != controlsSlots || len(doc.Chain.MasterFX) != controlsMasterFx {
		t.Fatalf("saved chain shape wrong: %+v", doc.Chain)
	}
}

func TestControlsParamsKey(t *testing.T) {
	cases := []struct {
		q    string
		slot uint8
		key  string
		ok   bool
	}{
		{"slot=1&comp=synth", 1, "synth:chain_params", true},
		{"slot=0&comp=fx3", 0, "fx3:chain_params", true},
		{"slot=3&comp=midi_fx1", 3, "midi_fx1:chain_params", true},
		{"fx=8", 0, "master_fx:fx8:chain_params", true},
		{"fx=9", 0, "", false},
		{"slot=4&comp=synth", 0, "", false},
		{"slot=0&comp=synth:module", 0, "", false},
		{"slot=0&comp=fx0", 0, "", false},
		{"slot=0&comp=overtake_dsp", 0, "", false},
	}
	for _, c := range cases {
		q, _ := url.ParseQuery(c.q)
		slot, key, ok := controlsParamsKey(q)
		if ok != c.ok || (ok && (slot != c.slot || key != c.key)) {
			t.Errorf("%s: got (%d, %q, %v)", c.q, slot, key, ok)
		}
	}
}

// Only the named shared modules are served, from the installed shared/ dir.
func TestControlsJSWhitelist(t *testing.T) {
	app, _ := controlsTestApp(t)
	os.MkdirAll(filepath.Join(app.basePath, "shared"), 0o755)
	os.WriteFile(filepath.Join(app.basePath, "shared", "control_map.mjs"), []byte("export const X = 1;"), 0o644)
	os.WriteFile(filepath.Join(app.basePath, "shared", "secret.mjs"), []byte("no"), 0o644)
	mux := http.NewServeMux()
	mux.HandleFunc("GET /controls/js/{name}", app.handleControlsJS)
	for name, want := range map[string]int{"control_map.mjs": 200, "secret.mjs": 404, "..%2Factive_set.txt": 404} {
		rec := httptest.NewRecorder()
		mux.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/controls/js/"+name, nil))
		if rec.Code != want {
			t.Errorf("%s: got %d want %d", name, rec.Code, want)
		}
		if want == 200 && !strings.HasPrefix(rec.Header().Get("Content-Type"), "text/javascript") {
			t.Errorf("%s: an ES module needs a JS content type, got %q", name, rec.Header().Get("Content-Type"))
		}
	}
}

// The page is registered with the template set (it once 500'd for want of it).
func TestControlsPageRenders(t *testing.T) {
	app, _ := controlsTestApp(t)
	tm, err := loadTemplates()
	if err != nil {
		t.Fatal(err)
	}
	app.tmpl = tm
	rec := httptest.NewRecorder()
	app.handleControls(rec, httptest.NewRequest(http.MethodGet, "/controls", nil))
	if rec.Code != http.StatusOK || !strings.Contains(rec.Body.String(), "/static/controls.js") {
		t.Fatalf("controls page: %d", rec.Code)
	}
}
