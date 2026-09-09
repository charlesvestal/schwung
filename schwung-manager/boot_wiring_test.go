// schwung-manager/boot_wiring_test.go
package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The target id differs from the module id on purpose: looking the entry up by
// module id is the mistake this test exists to catch.
func TestBootWiringUninstallDeregistersByOwner(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	dir := filepath.Join(base, "modules", "tools", "vmod")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(
		`{"id":"vmod","name":"V","version":"1.0.0","boot_target":{"id":"vee","name":"V","exec":"entry.sh"}}`),
		0o644); err != nil {
		t.Fatal(err)
	}
	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := readRegistryEntry(reg, "vee"); err != nil {
		t.Fatalf("target not registered under its declared id: %v", err)
	}
	if err := writeBootDefault(reg, "vee"); err != nil {
		t.Fatal(err)
	}

	if err := app.uninstallModule("vmod"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "vee")); !os.IsNotExist(err) {
		t.Error("uninstall left the picker row behind")
	}
	if got, _ := readBootDefault(reg); got != "schwung" {
		t.Errorf("default = %q, want schwung", got)
	}
}
