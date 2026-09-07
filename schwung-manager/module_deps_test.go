package main

import (
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

// A catalog served from memory: CatalogService.Fetch returns its cached copy
// when `fetched` is recent, so a test never reaches the network.
func depsApp(t *testing.T, mods []CatalogModule, installed ...string) *App {
	t.Helper()
	base := t.TempDir()
	for _, id := range installed {
		if err := os.MkdirAll(filepath.Join(base, "modules", "audio_fx", id), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	return &App{
		basePath: base,
		logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
		catalogSvc: &CatalogService{
			catalog: &Catalog{Modules: mods},
			fetched: time.Now(),
		},
	}
}

// requires_modules is a LIST THE MANAGER ACTS ON, where `requires` is prose for
// the user about assets it cannot fetch. Keeping them apart matters: a module
// that names a ROM in `requires` must not have the manager try to install one.
func TestRequiresModulesIsSeparateFromRequiresProse(t *testing.T) {
	var m CatalogModule
	raw := []byte(`{"id":"dr32","requires":"WAV samples","requires_modules":["clap"]}`)
	if err := jsonUnmarshal(raw, &m); err != nil {
		t.Fatal(err)
	}
	if m.Requires != "WAV samples" {
		t.Errorf("Requires = %q, want the prose", m.Requires)
	}
	if !reflect.DeepEqual(m.RequiresModules, []string{"clap"}) {
		t.Errorf("RequiresModules = %v, want [clap]", m.RequiresModules)
	}
}

func TestInstalledDependentsOf(t *testing.T) {
	cat := []CatalogModule{
		{ID: "clap", Name: "Airwindows"},
		{ID: "dr32", RequiresModules: []string{"clap"}},
		{ID: "other", RequiresModules: []string{"clap"}},
		{ID: "unrelated"},
	}

	// Nothing installed: nobody pins it. A module the user never installed
	// cannot make another one un-removable by proxy.
	if got := depsApp(t, cat).installedDependentsOf("clap"); len(got) != 0 {
		t.Errorf("dependents with nothing installed = %v, want none", got)
	}

	// Only what is ON DISK counts, and the answer is sorted so the message
	// a user sees does not reorder between calls.
	app := depsApp(t, cat, "dr32", "other")
	if got := app.installedDependentsOf("clap"); !reflect.DeepEqual(got, []string{"dr32", "other"}) {
		t.Errorf("dependents = %v, want [dr32 other]", got)
	}

	// A module never declares itself.
	if got := app.installedDependentsOf("dr32"); len(got) != 0 {
		t.Errorf("dr32 dependents = %v, want none", got)
	}

	// AN UNREACHABLE CATALOG ANSWERS NOBODY, so uninstall keeps working
	// offline. That is the deliberate direction to fail in: the alternative is
	// a device that cannot remove a module because it cannot reach the network.
	offline := &App{
		basePath:   app.basePath,
		logger:     slog.New(slog.NewTextHandler(io.Discard, nil)),
		catalogSvc: nil,
	}
	if got := offline.installedDependentsOf("clap"); len(got) != 0 {
		t.Errorf("offline dependents = %v, want none", got)
	}
}

func TestUninstallRefusesWhileDependedOn(t *testing.T) {
	cat := []CatalogModule{
		{ID: "clap"},
		{ID: "dr32", RequiresModules: []string{"clap"}},
	}
	app := depsApp(t, cat, "clap", "dr32")

	err := app.uninstallModule("clap")
	if err == nil {
		t.Fatal("uninstalling a depended-on module succeeded; it must be refused")
	}
	// The message has to NAME the dependent. "cannot uninstall" alone leaves
	// the user with no next step on a device with no other way to look it up.
	if !contains(err.Error(), "dr32") {
		t.Errorf("refusal %q does not name the dependent", err)
	}
	if app.findModuleDir("clap") == "" {
		t.Error("the refused uninstall deleted the module anyway")
	}

	// Remove the dependent and it becomes removable.
	if err := app.uninstallModule("dr32"); err != nil {
		t.Fatalf("uninstalling the dependent failed: %v", err)
	}
	if err := app.uninstallModule("clap"); err != nil {
		t.Fatalf("uninstalling clap after its dependent was removed failed: %v", err)
	}
	if app.findModuleDir("clap") != "" {
		t.Error("clap survived its own uninstall")
	}
}

// The dependency walk must terminate on a catalog that names a cycle, and must
// not reinstall something already present -- a dependency is a floor, not a
// version pin, so reinstalling one would quietly downgrade a module the user
// updated on purpose.
func TestInstallDepsBreaksCycles(t *testing.T) {
	// NEITHER IS INSTALLED. An earlier version of this test pre-installed both,
	// so the already-present check short-circuited before the recursion and the
	// cycle guard was never reached -- removing the guard left the test green.
	//
	// min_host_version is unsatisfiable so the walk terminates at the compat
	// check rather than attempting a download: the recursion happens in the
	// dependency loop, which runs BEFORE that check, so the cycle is still what
	// this exercises.
	cat := []CatalogModule{
		{ID: "a", RequiresModules: []string{"b"}, MinHostVer: "999.0.0"},
		{ID: "b", RequiresModules: []string{"a"}, MinHostVer: "999.0.0"},
	}
	app := depsApp(t, cat)
	if err := os.MkdirAll(filepath.Join(app.basePath, "host"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(app.basePath, "host", "version.txt"),
		[]byte("0.1.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	done := make(chan error, 1)
	go func() { done <- app.installModuleWithDeps(&cat[0], map[string]bool{}) }()
	select {
	case err := <-done:
		if err == nil {
			t.Fatal("expected the unsatisfiable min_host_version to stop the walk")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("installModuleWithDeps did not terminate on a dependency cycle")
	}
}

// A dependency ALREADY PRESENT is skipped rather than reinstalled: a dependency
// is a floor, not a version pin, so reinstalling would quietly downgrade a
// module the user updated on purpose. Reaching the compat check at all proves
// the dep loop returned without attempting a download for the present one.
func TestInstallDepsSkipsPresent(t *testing.T) {
	cat := []CatalogModule{
		{ID: "dr32", RequiresModules: []string{"clap"}, MinHostVer: "999.0.0"},
		{ID: "clap"},
	}
	app := depsApp(t, cat, "clap")
	if err := os.MkdirAll(filepath.Join(app.basePath, "host"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(app.basePath, "host", "version.txt"),
		[]byte("0.1.0\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	err := app.installModuleWithDeps(&cat[0], map[string]bool{})
	if err == nil || contains(err.Error(), "clap") {
		t.Fatalf("expected to stop at dr32 own compat check, got %v", err)
	}
}

// A dependency the catalog does not know about is an ERROR naming the
// dependency, not a silent partial install: a module whose effects do nothing
// because a dependency is absent is undiagnosable from the device.
func TestMissingDependencyIsNamed(t *testing.T) {
	cat := []CatalogModule{{ID: "dr32", RequiresModules: []string{"ghost"}}}
	app := depsApp(t, cat)
	err := app.installModuleWithDeps(&cat[0], map[string]bool{})
	if err == nil {
		t.Fatal("a dependency missing from the catalog was not reported")
	}
	if !contains(err.Error(), "ghost") {
		t.Errorf("error %q does not name the missing dependency", err)
	}
}
