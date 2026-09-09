package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBootRegistryWriteIsFlat(t *testing.T) {
	dir := t.TempDir()
	e := registryEntry{ID: "v", Name: "V", Exec: "/data/UserData/platforms/v/entry.sh",
		Version: "0.3.0", Owner: "platform:v"}
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(filepath.Join(dir, "v", "boot.json"))
	if err != nil {
		t.Fatal(err)
	}
	got := string(raw)

	// Flatness is asserted because a NESTED object carrying the same key
	// shadows the real one in the selector's reader (measured; see
	// tests/host/test_boot_target_manager_json.sh). One field per line is the
	// shape shim-entrypoint.sh already writes for the "schwung" entry.
	for _, key := range []string{"name", "exec", "version", "owner"} {
		var found int
		for _, line := range strings.Split(got, "\n") {
			if strings.Contains(line, `"`+key+`"`) {
				found++
				if strings.Count(line, `":`) != 1 {
					t.Errorf("line %q carries more than one field", line)
				}
			}
		}
		if found != 1 {
			t.Errorf("key %q appears on %d lines, want 1\n%s", key, found, got)
		}
	}
	if strings.Contains(got, "{\n  \"") == false {
		t.Errorf("not an indented object:\n%s", got)
	}
}

func TestBootRegistryGoldenMatchesWriter(t *testing.T) {
	dir := t.TempDir()
	e := registryEntry{ID: "v", Name: "V", Exec: "/data/UserData/platforms/v/entry.sh",
		Version: "0.3.0", Owner: "platform:v"}
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "v", "boot.json"))
	if err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadFile("testdata/boot.json.golden")
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(want) {
		t.Errorf("writer output drifted from testdata/boot.json.golden.\ngot:\n%s\nwant:\n%s\n"+
			"tests/host/test_boot_target_manager_json.sh reads the golden with the "+
			"selector's own awk parser — update both together or not at all.", got, want)
	}
}

func TestBootRegistryListIncludesUnowned(t *testing.T) {
	dir := t.TempDir()
	mustWrite := func(e registryEntry) {
		t.Helper()
		if err := writeRegistryEntry(dir, e); err != nil {
			t.Fatal(err)
		}
	}
	mustWrite(registryEntry{ID: "vee", Name: "V", Exec: "/x/entry.sh", Owner: "module:v-platform"})
	mustWrite(registryEntry{ID: "hand", Name: "Hand", Exec: "/y/entry.sh"}) // no owner

	entries, err := listRegistryEntries(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 {
		t.Fatalf("listRegistryEntries = %d entries, want 2", len(entries))
	}

	// An unowned entry is LISTED — the Boot page shows it — even though
	// reconcile may never modify it.
	byID := map[string]registryEntry{}
	for _, e := range entries {
		byID[e.ID] = e
	}
	if byID["vee"].Owner != "module:v-platform" {
		t.Errorf("vee owner = %q", byID["vee"].Owner)
	}
	if byID["hand"].Owner != "" {
		t.Errorf("hand owner = %q, want empty", byID["hand"].Owner)
	}
}

func TestBootRegistryDefault(t *testing.T) {
	dir := t.TempDir()
	if got, _ := readBootDefault(dir); got != "" {
		t.Errorf("readBootDefault on empty registry = %q, want empty", got)
	}
	if err := writeBootDefault(dir, "vee"); err != nil {
		t.Fatal(err)
	}
	raw, _ := os.ReadFile(filepath.Join(dir, "default"))
	if string(raw) != "vee\n" {
		t.Errorf("default file = %q, want \"vee\\n\"", raw)
	}
	if got, _ := readBootDefault(dir); got != "vee" {
		t.Errorf("readBootDefault = %q", got)
	}
}

// FINDING 1. A torn boot.json turns a manager-owned entry into an IMMORTAL
// one: Owner == "" is the sentinel for "hand-installed, never touch", so a
// half-written file (power loss during the write — this codebase already
// documents torn .boot-attempt stamps as a real field occurrence) permanently
// consumes a picker slot pointing at a possibly-dangling exec, and nothing in
// the manager can rewrite or remove it.
//
// The observable is the INODE: an in-place os.WriteFile truncates and refills
// the same file, so a reader can see the empty middle. A write to a temp file
// in the same directory followed by rename never can.
func TestBootRegistryWriteIsAtomicRename(t *testing.T) {
	dir := t.TempDir()
	e := registryEntry{ID: "v", Name: "V", Exec: "/x/entry.sh", Version: "1.0.0", Owner: "platform:v"}
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(dir, "v", "boot.json")
	before, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}

	e.Version = "2.0.0"
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	after, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if os.SameFile(before, after) {
		t.Error("boot.json was rewritten IN PLACE: a torn write leaves an entry with " +
			"no owner, which reconcile will never rewrite or delete")
	}
	got, err := readRegistryEntry(dir, "v")
	if err != nil || got != e {
		t.Fatalf("after rewrite: %+v, %v; want %+v", got, err, e)
	}
	// A staging file left behind is a directory that lists as a target with a
	// second, unreadable boot.json beside it.
	ents, err := os.ReadDir(filepath.Join(dir, "v"))
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 1 || ents[0].Name() != "boot.json" {
		var names []string
		for _, d := range ents {
			names = append(names, d.Name())
		}
		t.Errorf("registry dir holds %v, want just boot.json", names)
	}
}

// FINDING 4. setBootDefault took a form value (main.go handleBootSetDefault,
// r.FormValue("id")) straight into filepath.Join. Join CLEANS "..", so
// id="../evil" reads <registry>/../evil/boot.json — and a module tarball can
// ship a file called boot.json. The check then passes and the traversal string
// is written verbatim into boot-targets/default, where the selector's
// bt_resolve_default / bt_exec_path run whatever that file names.
func TestBootDefaultRefusesTraversalID(t *testing.T) {
	root := t.TempDir()
	reg := filepath.Join(root, "boot-targets")
	if err := os.MkdirAll(reg, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("BOOT_TARGETS_DIR", reg)

	// A payload directory outside the registry carrying a boot.json — exactly
	// what a module tarball shipping that filename stages.
	evil := filepath.Join(root, "evil")
	if err := os.MkdirAll(evil, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(evil, "boot.json"),
		[]byte("{\n  \"name\": \"E\",\n  \"exec\": \"/bin/sh\"\n}\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	app := &App{basePath: filepath.Join(root, "schwung"), logger: testLogger()}
	for _, id := range []string{"../evil", "..", "sub/dir", "Evil"} {
		if err := app.setBootDefault(id); err == nil {
			t.Errorf("setBootDefault(%q) was accepted", id)
		}
	}
	if _, err := os.Stat(filepath.Join(reg, "default")); !os.IsNotExist(err) {
		raw, _ := os.ReadFile(filepath.Join(reg, "default"))
		t.Errorf("a default was written anyway: %q", raw)
	}

	// "stock" has no directory and must still be accepted.
	if err := app.setBootDefault("stock"); err != nil {
		t.Errorf("setBootDefault(\"stock\") = %v, want accepted", err)
	}
}
