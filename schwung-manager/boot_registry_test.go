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

	// The shell reader is a per-line awk matcher: every field must sit alone
	// on its own line or the selector cannot see it.
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

func TestBootRegistryListAndOwner(t *testing.T) {
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

	// The payload id is "v-platform" but the target id is "vee": looking up by
	// id would miss it, which is how an uninstall leaves a row behind.
	got, ok := findEntryByOwner(entries, "module:v-platform")
	if !ok || got.ID != "vee" {
		t.Fatalf("findEntryByOwner = %+v, %v; want id vee", got, ok)
	}
	if _, ok := findEntryByOwner(entries, "module:absent"); ok {
		t.Error("findEntryByOwner matched an owner that is not present")
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
