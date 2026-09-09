// schwung-manager/boot_reconcile_test.go
package main

import (
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
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
		Exec:  filepath.Join(base, "modules", "tools", "vplat", "entry.sh"),
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

// plantPlatform writes a platform payload declaring a boot target under the
// given target id.
func plantPlatform(t *testing.T, base, id, targetID, name string) {
	t.Helper()
	dir := filepath.Join(platformsRoot(base), id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	manifest := fmt.Sprintf(
		`{"id":%q,"name":%q,"version":"1.0.0","boot_target":{"id":%q,"name":%q,"exec":"entry.sh"}}`,
		id, name, targetID, name)
	if err := os.WriteFile(filepath.Join(dir, "platform.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
}

// plantModuleTarget writes a module payload whose boot_target id differs from
// the module id.
func plantModuleTarget(t *testing.T, base, subdir, id, targetID, name string) {
	t.Helper()
	dir := filepath.Join(base, "modules", subdir, id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	manifest := fmt.Sprintf(
		`{"id":%q,"name":%q,"version":"1.0.0","boot_target":{"id":%q,"name":%q,"exec":"entry.sh"}}`,
		id, name, targetID, name)
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
}

// FINDING 1. An ownership hand-off must not strand an immortal entry.
//
// Deleting only entries whose ID is absent from `desired` misses the case
// where the id IS desired, by somebody else: the old owner is gone, the claim
// check refuses to rewrite the row for the new owner, and nothing deletes it.
// The exec then points into a deleted directory forever — and the selector
// stamps a boot-attempt strike BEFORE it discovers the exec is missing, so
// three boots later the picker is forced on every boot with only SSH to fix
// it.
func TestReconcileDeletesEntryWhoseOwnerLeftEvenWhenIDIsReclaimed(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModuleTarget(t, base, "tools", "foo", "vee", "Vee")
	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if e, _ := readRegistryEntry(reg, "vee"); e.Owner != "module:foo" {
		t.Fatalf("setup: owner = %q, want module:foo", e.Owner)
	}
	fooExec := filepath.Join(base, "modules", "tools", "foo", "entry.sh")

	// foo is uninstalled; platform bar wants the same target id.
	if err := os.RemoveAll(filepath.Join(base, "modules", "tools", "foo")); err != nil {
		t.Fatal(err)
	}
	plantPlatform(t, base, "bar", "vee", "Vee")

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatalf("reconcile: %v", err)
	}
	e, err := readRegistryEntry(reg, "vee")
	if err != nil {
		t.Fatalf("entry vanished entirely: %v", err)
	}
	if e.Owner != "platform:bar" {
		t.Errorf("owner = %q, want platform:bar — the departed owner's row was never released", e.Owner)
	}
	if e.Exec == fooExec {
		t.Errorf("exec still points into the deleted payload: %q", e.Exec)
	}
}

// FINDING 2. One id collision must not disable reconcile device-wide.
//
// Returning (nil, err) on the first clash made every later reconcile a no-op:
// no deregistration on uninstall, no stale-exec rewrite, no default healing —
// and every call site is warn-only, so nothing surfaced.
func TestReconcileCollisionSkipsOnlyTheClashingEntry(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	// Two payloads claiming "vee", plus unrelated work that must still happen.
	plantModuleTarget(t, base, "tools", "aaa", "vee", "A")
	plantModuleTarget(t, base, "tools", "bbb", "vee", "B")
	plantModule(t, base, "tools", "solo", "Solo", true)
	if err := writeRegistryEntry(reg, registryEntry{ID: "gone", Name: "Gone",
		Exec: "/nowhere/entry.sh", Owner: "module:gone"}); err != nil {
		t.Fatal(err)
	}

	err := app.reconcileBootTargets()
	if err == nil {
		t.Fatal("want an error naming the colliding owners, got nil")
	}
	if !strings.Contains(err.Error(), "aaa") || !strings.Contains(err.Error(), "bbb") {
		t.Errorf("collision error must name both owners, got: %v", err)
	}
	if strings.Contains(err.Error(), "full") {
		t.Errorf("a collision is not a full picker; message: %v", err)
	}
	if _, err := readRegistryEntry(reg, "solo"); err != nil {
		t.Errorf("an unrelated target was never registered: %v", err)
	}
	if _, err := os.Stat(filepath.Join(reg, "gone")); !os.IsNotExist(err) {
		t.Error("an orphaned entry survived: the collision aborted the whole pass")
	}

	// Deterministic winner: a flip-flop rewrites the registry on alternating
	// reconciles, which is a boot.json churned on every install.
	first, _ := readRegistryEntry(reg, "vee")
	for i := 0; i < 3; i++ {
		_ = app.reconcileBootTargets()
		again, _ := readRegistryEntry(reg, "vee")
		if again != first {
			t.Fatalf("collision winner flipped: %+v then %+v", first, again)
		}
	}
	if first.Owner != "module:aaa" {
		t.Errorf("winner = %q, want the sort-first claimant module:aaa", first.Owner)
	}
}

// FINDING 3. A validation REFUSAL is not the payload retiring its block.
//
// A refused target dropped out of `desired`, so step 1 deleted the live row
// and healed `default` to schwung. The publisher fixes the payload next
// release and the row comes back; the user's boot default does not.
func TestReconcileRefusalKeepsEntryAndDefault(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", true)
	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if err := writeBootDefault(reg, "vplat"); err != nil {
		t.Fatal(err)
	}

	// A bad update: the manifest still declares boot_target, the entry script
	// it names is gone, so parseBootTarget refuses.
	if err := os.Remove(filepath.Join(base, "modules", "tools", "vplat", "entry.sh")); err != nil {
		t.Fatal(err)
	}

	_ = app.reconcileBootTargets()
	if _, err := os.Stat(filepath.Join(reg, "vplat", "boot.json")); err != nil {
		t.Error("a refused target deleted the live entry; only an ABSENT payload may do that")
	}
	if got, _ := readBootDefault(reg); got != "vplat" {
		t.Errorf("default = %q, want vplat — a refusal silently discarded the user's choice", got)
	}
}

// FINDING 4. Reconcile is serialized.
//
// It runs at startup and from four HTTP handlers with no lock. Two overlapping
// passes can have one read a payload directory while the other's tar is
// mid-flight — manifest landed, entry script not — so the target is refused
// and (before finding 3's fix) the live row was deleted and the default healed.
func TestReconcileIsSerialized(t *testing.T) {
	app, base, _ := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", true)

	var inside, maxInside atomic.Int32
	bootReconcileInside = func() {
		n := inside.Add(1)
		for {
			m := maxInside.Load()
			if n <= m || maxInside.CompareAndSwap(m, n) {
				break
			}
		}
		time.Sleep(20 * time.Millisecond)
		inside.Add(-1)
	}
	defer func() { bootReconcileInside = nil }()

	var wg sync.WaitGroup
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_ = app.reconcileBootTargets()
		}()
	}
	wg.Wait()
	if got := maxInside.Load(); got != 1 {
		t.Errorf("%d reconcile passes ran at once; the registry has no lock", got)
	}
}
