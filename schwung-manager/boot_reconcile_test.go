// schwung-manager/boot_reconcile_test.go
package main

import (
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
)

// plantModule writes a module payload with an optional boot_target block.
func plantModule(t *testing.T, base, subdir, id, name string, withTarget bool) {
	t.Helper()
	dir := filepath.Join(base, "modules", subdir, id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	block := ""
	if withTarget {
		block = fmt.Sprintf(`,"boot_target":{"name":%q,"exec":"entry.sh"}`, name)
		if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	manifest := fmt.Sprintf(`{"id":%q,"name":%q,"version":"1.0.0"%s}`, id, name, block)
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
}

func newReconcileApp(t *testing.T) (*App, string, string) {
	t.Helper()
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	reg := filepath.Join(root, "boot-targets")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(reg, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("BOOT_TARGETS_DIR", reg)
	return &App{basePath: base, logger: testLogger()}, base, reg
}

func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}

func TestReconcileCreatesDeclaredTarget(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", true)

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	e, err := readRegistryEntry(reg, "vplat")
	if err != nil {
		t.Fatalf("entry not created: %v", err)
	}
	if e.Name != "V" || e.Owner != "module:vplat" {
		t.Errorf("entry = %+v", e)
	}
	wantExec := filepath.Join(base, "modules", "tools", "vplat", "entry.sh")
	if e.Exec != wantExec {
		t.Errorf("exec = %q, want %q", e.Exec, wantExec)
	}
}

func TestReconcileDeletesOrphanAndHealsDefault(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := writeRegistryEntry(reg, registryEntry{ID: "gone", Name: "Gone",
		Exec: "/nowhere/entry.sh", Owner: "module:gone"}); err != nil {
		t.Fatal(err)
	}
	if err := writeBootDefault(reg, "gone"); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "gone")); !os.IsNotExist(err) {
		t.Error("orphaned entry was not deleted")
	}
	got, _ := readBootDefault(reg)
	if got != "schwung" {
		t.Errorf("default = %q, want schwung", got)
	}
}

func TestReconcileDeletesWhenBlockRetired(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", false) // installed, no boot_target
	if err := writeRegistryEntry(reg, registryEntry{ID: "vplat", Name: "V",
		Exec: filepath.Join(base, "modules", "tools", "vplat", "entry.sh"),
		Owner: "module:vplat"}); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "vplat")); !os.IsNotExist(err) {
		t.Error("entry survived its payload retiring the boot_target block")
	}
}

func TestReconcileRewritesChangedEntry(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V2", true)
	if err := writeRegistryEntry(reg, registryEntry{ID: "vplat", Name: "V1",
		Exec: "/stale/path/entry.sh", Version: "0.0.1", Owner: "module:vplat"}); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	e, _ := readRegistryEntry(reg, "vplat")
	if e.Name != "V2" {
		t.Errorf("name = %q, want V2", e.Name)
	}
	if e.Exec != filepath.Join(base, "modules", "tools", "vplat", "entry.sh") {
		t.Errorf("stale exec not rewritten: %q", e.Exec)
	}
}

// The two entries reconcile must never touch, in the situation where every
// other rule says delete: no payload on disk at all.
func TestReconcileNeverTouchesUnownedOrSchwung(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := writeRegistryEntry(reg, registryEntry{ID: "hand", Name: "Hand",
		Exec: "/opt/hand/entry.sh"}); err != nil { // no owner
		t.Fatal(err)
	}
	if err := writeRegistryEntry(reg, registryEntry{ID: "schwung", Name: "Schwung",
		Exec: "/data/UserData/schwung/schwung-entry.sh", Owner: "module:schwung"}); err != nil {
		t.Fatal(err)
	}
	if err := writeBootDefault(reg, "hand"); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "hand", "boot.json")); err != nil {
		t.Error("hand-installed entry was deleted")
	}
	if _, err := os.Stat(filepath.Join(reg, "schwung", "boot.json")); err != nil {
		t.Error("schwung entry was deleted")
	}
	if got, _ := readBootDefault(reg); got != "hand" {
		t.Errorf("default = %q, want hand (reconcile must not repoint a live default)", got)
	}
}

func TestReconcileRefusesPastRowCap(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	for i := 0; i < bootPickerTargetCap+2; i++ {
		plantModule(t, base, "tools", fmt.Sprintf("mod%02d", i), fmt.Sprintf("M%02d", i), true)
	}
	err := app.reconcileBootTargets()
	if err == nil {
		t.Fatal("want an error naming the row cap, got nil")
	}
	entries, _ := listRegistryEntries(reg)
	if len(entries) > bootPickerTargetCap {
		t.Errorf("registered %d targets, cap is %d", len(entries), bootPickerTargetCap)
	}
}
