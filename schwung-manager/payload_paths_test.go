package main

import (
	"os"
	"path/filepath"
	"testing"
)

// Every subdir getInstallSubdir can produce must be a directory we enumerate.
// Driven off getInstallSubdir so the two cannot drift; restating its output
// here would just be a second copy of the bug this test exists to prevent.
func TestPayloadPathsCoverEveryInstallSubdir(t *testing.T) {
	types := []string{
		"sound_generator", "audio_fx", "midi_fx", "utility",
		"overtake", "tool", "system", "featured", "", "nonsense",
	}
	base := "/data/UserData/schwung"
	dirs := map[string]bool{}
	for _, d := range moduleInstallDirs(base) {
		dirs[d] = true
	}
	for _, ct := range types {
		want := filepath.Join(base, "modules", getInstallSubdir(ct))
		if !dirs[want] {
			t.Errorf("component_type %q installs to %s, which is not enumerated", ct, want)
		}
	}
}

func TestPayloadPathsFindsUtilityModule(t *testing.T) {
	base := t.TempDir()
	dir := filepath.Join(base, "modules", "utilities", "widget")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"),
		[]byte(`{"id":"widget","name":"Widget","version":"1.0.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	app := &App{basePath: base}
	if got := app.findModuleDir("widget"); got != dir {
		t.Errorf("findModuleDir = %q, want %q", got, dir)
	}
	if _, ok := discoverInstalledModules(base)["widget"]; !ok {
		t.Error("discoverInstalledModules did not find the utility module")
	}
}

func TestPayloadPathsRoots(t *testing.T) {
	if got := platformsRoot("/data/UserData/schwung"); got != "/data/UserData/platforms" {
		t.Errorf("platformsRoot = %q", got)
	}
	t.Setenv("BOOT_TARGETS_DIR", "")
	if got := bootTargetsDir(); got != "/data/UserData/boot-targets" {
		t.Errorf("bootTargetsDir default = %q", got)
	}
	t.Setenv("BOOT_TARGETS_DIR", "/tmp/fixture")
	if got := bootTargetsDir(); got != "/tmp/fixture" {
		t.Errorf("bootTargetsDir env = %q", got)
	}
}

// The Remote UI was the third place this list was restated, and it had the
// same hole: a utility module's web_ui.html was never found, silently.
func TestPayloadPathsRemoteUIFindsUtilityModule(t *testing.T) {
	base := t.TempDir()
	dir := filepath.Join(base, "modules", "utilities", "widget")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "web_ui.html"), []byte("<html></html>"), 0o644); err != nil {
		t.Fatal(err)
	}
	ru := &RemoteUI{basePath: base}
	if got := ru.findModuleWebUI("widget"); got == "" {
		t.Error("findModuleWebUI did not find a utility module's web_ui.html")
	}
	if got := ru.findModuleWebUI("absent"); got != "" {
		t.Errorf("findModuleWebUI invented a URL for a module that is not there: %q", got)
	}
}
