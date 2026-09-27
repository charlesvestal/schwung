// schwung-manager/boot_wiring_test.go
package main

import (
	"fmt"
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

// Uninstall reports success when the payload is gone, even if reconcile has
// something to complain about.
//
// Reconcile fails for reasons that belong to OTHER payloads — a full picker,
// somebody else's id collision — and the module being removed here is already
// off the disk by then. Returning that error would tell the user "uninstall
// failed" about work that succeeded, and leave them retrying it forever.
func TestBootWiringUninstallSucceedsWhenReconcileErrors(t *testing.T) {
	app, base, _ := newReconcileApp(t)
	// One more declared target than the picker can hold, so reconcile is
	// guaranteed to return its cap error.
	for i := 0; i < bootPickerTargetCap+2; i++ {
		plantModule(t, base, "tools", fmt.Sprintf("mod%02d", i), fmt.Sprintf("M%02d", i), true)
	}
	if err := app.reconcileBootTargets(); err == nil {
		t.Fatal("setup is wrong: reconcile was expected to report the full picker")
	}

	if err := app.uninstallModule("mod00"); err != nil {
		t.Errorf("uninstallModule = %v; a removed payload must report success", err)
	}
	if app.findModuleDir("mod00") != "" {
		t.Error("payload still on disk")
	}
}
