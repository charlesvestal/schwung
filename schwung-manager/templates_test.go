// Smoke tests for template loading and per-module config discovery.
// These catch template syntax errors and JSON schema shape bugs
// without needing a device.

package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadTemplates(t *testing.T) {
	m, err := loadTemplates()
	if err != nil {
		t.Fatalf("loadTemplates: %v", err)
	}
	required := []string{
		"config.html",
		"module_detail.html",
	}
	for _, name := range required {
		if _, ok := m[name]; !ok {
			t.Errorf("missing template %q", name)
		}
	}
}

func TestDiscoverModuleSchemas(t *testing.T) {
	tmp := t.TempDir()
	modDir := filepath.Join(tmp, "modules", "tools", "testmod")
	if err := os.MkdirAll(modDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(modDir, "module.json"),
		[]byte(`{"id":"testmod","name":"Test Module","component_type":"tool"}`), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(modDir, "settings-schema.json"),
		[]byte(`{"id":"testmod","label":"Test","items":[{"key":"foo","label":"Foo","type":"string","default":"bar"}]}`),
		0644); err != nil {
		t.Fatal(err)
	}

	schemas := discoverModuleSchemas(tmp)
	if len(schemas) != 1 {
		t.Fatalf("got %d schemas, want 1", len(schemas))
	}
	s := schemas[0]
	if s.ID != "testmod" || s.Name != "Test Module" {
		t.Errorf("schema id=%q name=%q", s.ID, s.Name)
	}
	if len(s.Sections) != 1 || len(s.Sections[0].Items) != 1 {
		t.Fatalf("flat items not normalized: %+v", s.Sections)
	}
	if s.Sections[0].Items[0].Key != "foo" {
		t.Errorf("item key=%q", s.Sections[0].Items[0].Key)
	}
}

func TestDiscoverModuleSchemas_RejectsIDMismatch(t *testing.T) {
	tmp := t.TempDir()
	modDir := filepath.Join(tmp, "modules", "tools", "realname")
	os.MkdirAll(modDir, 0755)
	// Declared ID does not match parent dir name — must be dropped.
	os.WriteFile(filepath.Join(modDir, "module.json"),
		[]byte(`{"id":"realname","component_type":"tool"}`), 0644)
	os.WriteFile(filepath.Join(modDir, "settings-schema.json"),
		[]byte(`{"id":"evil","items":[]}`), 0644)

	if got := len(discoverModuleSchemas(tmp)); got != 0 {
		t.Errorf("mismatched schema accepted: got %d schemas", got)
	}
}

func TestWriteModuleConfigAtomic(t *testing.T) {
	tmp := t.TempDir()
	if err := writeModuleConfigKey(tmp, "k1", "v1"); err != nil {
		t.Fatal(err)
	}
	cfg := readModuleConfig(tmp)
	if cfg["k1"] != "v1" {
		t.Fatalf("got %v", cfg)
	}
	if err := writeModuleConfigKey(tmp, "k2", 42); err != nil {
		t.Fatal(err)
	}
	cfg = readModuleConfig(tmp)
	if cfg["k1"] != "v1" || cfg["k2"].(float64) != 42 {
		t.Fatalf("got %v", cfg)
	}
}

func TestWriteModuleSecret(t *testing.T) {
	tmp := t.TempDir()
	if err := writeModuleSecret(tmp, "openai_api_key", "sk-abcdef  "); err != nil {
		t.Fatal(err)
	}
	if !isModuleSecretSet(tmp, "openai_api_key") {
		t.Fatal("is_set false after write")
	}
	data, err := os.ReadFile(filepath.Join(tmp, "secrets", "openai_api_key.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "sk-abcdef" {
		t.Errorf("content %q not trimmed", string(data))
	}
	st, _ := os.Stat(filepath.Join(tmp, "secrets", "openai_api_key.txt"))
	if st.Mode().Perm() != 0600 {
		t.Errorf("mode=%o want 0600", st.Mode().Perm())
	}
	// Overwrite must succeed (pre-remove before O_EXCL open).
	if err := writeModuleSecret(tmp, "openai_api_key", "sk-new"); err != nil {
		t.Fatalf("overwrite: %v", err)
	}
	data, _ = os.ReadFile(filepath.Join(tmp, "secrets", "openai_api_key.txt"))
	if string(data) != "sk-new" {
		t.Errorf("overwrite content %q", string(data))
	}
}

func TestModuleIDFromPath(t *testing.T) {
	cases := []struct {
		path, want string
	}{
		{"/modules/guitar-tuner", "guitar-tuner"},
		{"/modules/guitar-tuner/settings/set", "guitar-tuner"},
		{"/modules/guitar-tuner/settings/values", "guitar-tuner"},
		{"/modules/", ""},
		{"/modules/BAD", ""},
		{"/modules/../etc", ""},
		{"/config/modules/guitar-tuner", ""},
		{"/config", ""},
	}
	for _, c := range cases {
		if got := moduleIDFromPath(c.path); got != c.want {
			t.Errorf("moduleIDFromPath(%q) = %q, want %q", c.path, got, c.want)
		}
	}
}

func TestSnapshotRestoreUserState(t *testing.T) {
	tmp := t.TempDir()
	// Seed the module dir with user state.
	os.WriteFile(filepath.Join(tmp, "config.json"),
		[]byte(`{"foo":"bar"}`), 0644)
	os.MkdirAll(filepath.Join(tmp, "secrets"), 0700)
	os.WriteFile(filepath.Join(tmp, "secrets", "openai_api_key.txt"),
		[]byte("sk-xxxx"), 0600)

	snap, err := snapshotModuleUserState(tmp)
	if err != nil {
		t.Fatal(err)
	}
	if snap == nil {
		t.Fatal("nil snapshot")
	}

	// Simulate tarball extraction clobbering the files.
	os.WriteFile(filepath.Join(tmp, "config.json"),
		[]byte(`{"malicious":"default"}`), 0644)
	os.WriteFile(filepath.Join(tmp, "secrets", "openai_api_key.txt"),
		[]byte("sk-bad-default"), 0644)

	if err := restoreModuleUserState(tmp, snap); err != nil {
		t.Fatal(err)
	}

	data, _ := os.ReadFile(filepath.Join(tmp, "config.json"))
	if string(data) != `{"foo":"bar"}` {
		t.Errorf("config.json not restored: %q", data)
	}
	data, _ = os.ReadFile(filepath.Join(tmp, "secrets", "openai_api_key.txt"))
	if string(data) != "sk-xxxx" {
		t.Errorf("secret not restored: %q", data)
	}
	st, _ := os.Stat(filepath.Join(tmp, "secrets", "openai_api_key.txt"))
	if st.Mode().Perm() != 0600 {
		t.Errorf("secret mode=%o want 0600", st.Mode().Perm())
	}
}

func TestSnapshotMissingDir(t *testing.T) {
	tmp := t.TempDir()
	// Dir that doesn't exist yet (first install) — should return nil.
	snap, err := snapshotModuleUserState(filepath.Join(tmp, "missing"))
	if err != nil {
		t.Errorf("err on missing dir: %v", err)
	}
	if snap != nil {
		t.Errorf("non-nil snapshot for missing dir: %+v", snap)
	}
	// Restore with nil should be a no-op.
	if err := restoreModuleUserState(tmp, nil); err != nil {
		t.Errorf("restore(nil) err: %v", err)
	}
}

func TestResolveModuleDefaultSource_Containment(t *testing.T) {
	tmp := t.TempDir()
	if err := os.WriteFile(filepath.Join(tmp, "default.txt"), []byte("hello"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := resolveModuleDefaultSource(tmp, "default.txt"); got != "hello" {
		t.Errorf("got %q", got)
	}
	if got := resolveModuleDefaultSource(tmp, "../outside.txt"); got != "" {
		t.Errorf("traversal allowed: %q", got)
	}
	if got := resolveModuleDefaultSource(tmp, "/etc/passwd"); got != "" {
		t.Errorf("absolute path allowed: %q", got)
	}
}
