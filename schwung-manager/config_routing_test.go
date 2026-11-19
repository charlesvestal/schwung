package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The Settings page writes each setting where the DEVICE reads it. Param View
// and Open With were written into shadow_config.json, which the device never
// reads for either: the page showed the new value and the Move ignored it.

func newConfigTestApp(t *testing.T) (*App, string) {
	t.Helper()
	base := t.TempDir()
	if err := os.MkdirAll(filepath.Join(base, "shared"), 0755); err != nil {
		t.Fatal(err)
	}
	schema, err := os.ReadFile(filepath.Join("..", "src", "shared", "settings-schema.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "shared", "settings-schema.json"), schema, 0644); err != nil {
		t.Fatal(err)
	}
	return &App{basePath: base, logger: testLogger()}, base
}

func setConfig(t *testing.T, app *App, key, value string) int {
	t.Helper()
	form := url.Values{"key": {key}, "value": {value}}
	req := httptest.NewRequest(http.MethodPost, "/config/set", strings.NewReader(form.Encode()))
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	req.Header.Set("X-CSRF-Token", "t")
	rec := httptest.NewRecorder()
	app.handleConfigSetSetting(rec, req)
	return rec.Code
}

func readJSONT(t *testing.T, path string) map[string]any {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	var m map[string]any
	if err := json.Unmarshal(b, &m); err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return m
}

func TestParamViewIsWrittenWhereTheDeviceReadsIt(t *testing.T) {
	app, base := newConfigTestApp(t)
	if c := setConfig(t, app, "param_view", "0"); c != 200 {
		t.Fatalf("status %d", c)
	}
	if v := readJSONT(t, filepath.Join(base, "param_view.json"))["param_view"]; v != float64(0) {
		t.Fatalf("param_view.json param_view = %v, want 0", v)
	}
	if _, err := os.Stat(filepath.Join(base, "shadow_config.json")); err == nil {
		if _, ok := readJSONT(t, filepath.Join(base, "shadow_config.json"))["param_view"]; ok {
			t.Fatal("param_view written to shadow_config.json, which nothing on the device reads")
		}
	}
	if got := app.readParamView(); got != 0 {
		t.Fatalf("readParamView = %v, want 0", got)
	}
	if c := setConfig(t, app, "param_view", "7"); c != http.StatusBadRequest {
		t.Fatalf("out-of-range param_view accepted: %d", c)
	}
}

func TestShadowUITriggerIsWrittenAsTheNameTheShimParses(t *testing.T) {
	app, base := newConfigTestApp(t)
	// The shim's load_feature_config matches these three strings.
	for i, name := range []string{"long_press", "shift_vol", "both"} {
		if c := setConfig(t, app, "shadow_ui_trigger", string(rune('0'+i))); c != 200 {
			t.Fatalf("status %d", c)
		}
		ft := readJSONT(t, filepath.Join(base, "config", "features.json"))
		if ft["shadow_ui_trigger"] != name {
			t.Fatalf("features.json shadow_ui_trigger = %v, want %q", ft["shadow_ui_trigger"], name)
		}
		if got := readShadowUITrigger(ft); got != float64(i) {
			t.Fatalf("readShadowUITrigger(%q) = %v, want %d", name, got, i)
		}
	}
	if _, err := os.Stat(filepath.Join(base, "shadow_config.json")); err == nil {
		if _, ok := readJSONT(t, filepath.Join(base, "shadow_config.json"))["shadow_ui_trigger"]; ok {
			t.Fatal("shadow_ui_trigger written to shadow_config.json, which nothing on the device reads")
		}
	}
	if got := readShadowUITrigger(map[string]any{}); got != 2 {
		t.Fatalf("absent trigger reads %v, want Both (2) as the shim defaults", got)
	}
}
