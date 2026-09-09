// schwung-manager/platforms_test.go
package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestPlatformCatalogParsesWithAndWithout(t *testing.T) {
	var withOut Catalog
	if err := json.Unmarshal([]byte(`{"catalog_version":2,"modules":[]}`), &withOut); err != nil {
		t.Fatal(err)
	}
	if len(withOut.Platforms) != 0 {
		t.Errorf("absent platforms key parsed to %d entries", len(withOut.Platforms))
	}

	var with Catalog
	raw := `{"catalog_version":2,"modules":[],"platforms":[
	  {"id":"v","name":"V","github_repo":"who/v","default_branch":"main",
	   "asset_name":"v-platform.tar.gz","min_host_version":"1.3.2"}]}`
	if err := json.Unmarshal([]byte(raw), &with); err != nil {
		t.Fatal(err)
	}
	if len(with.Platforms) != 1 || with.Platforms[0].ID != "v" {
		t.Fatalf("platforms = %+v", with.Platforms)
	}
}

func TestPlatformDiscovery(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	dir := filepath.Join(root, "platforms", "v")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "platform.json"),
		[]byte(`{"id":"v","name":"V","version":"0.3.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	// A directory with no manifest is not a platform.
	if err := os.MkdirAll(filepath.Join(root, "platforms", "junk"), 0o755); err != nil {
		t.Fatal(err)
	}

	got := discoverInstalledPlatforms(base)
	if len(got) != 1 || got["v"] != dir {
		t.Fatalf("discoverInstalledPlatforms = %+v", got)
	}
}

// verifyPlatformExtraction was replaced by extractPlatformPayload, which
// stages and only then renames into place. Its "did the tarball carry a
// top-level <id>/" assertion lives in
// TestPlatformExtractLeavesNothingBehindOnMismatch below, alongside the
// cleanup that check used to skip.

func TestPlatformRowsMergeCatalogAndDisk(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	dir := filepath.Join(root, "platforms", "orphan")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "platform.json"),
		[]byte(`{"id":"orphan","name":"Orphan","version":"0.1.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	cat := []CatalogPlatform{{ID: "v", Name: "V"}}

	rows := platformRows(base, cat, map[string]string{"v": "0.4.0"})
	var haveOrphan, haveV bool
	for _, r := range rows {
		if r.ID == "orphan" {
			haveOrphan = true
			if !r.Installed || !r.Removable {
				t.Errorf("orphan row = %+v; an installed platform absent from the catalog must still be removable", r)
			}
		}
		if r.ID == "v" {
			haveV = true
			if r.Installed {
				t.Errorf("v is not installed but row says it is: %+v", r)
			}
			if r.Available != "0.4.0" {
				t.Errorf("available = %q, want 0.4.0", r.Available)
			}
		}
	}
	if !haveOrphan || !haveV {
		t.Fatalf("rows = %+v", rows)
	}
}

// makePlatformTarball writes a .tar.gz whose single top-level directory is
// topDir, holding a platform.json.
func makePlatformTarball(t *testing.T, dest, topDir string) {
	t.Helper()
	stage := t.TempDir()
	inner := filepath.Join(stage, topDir)
	if err := os.MkdirAll(filepath.Join(inner, "sub"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(inner, "platform.json"),
		[]byte(`{"id":"`+topDir+`","name":"V","version":"0.3.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(inner, "sub", "entry.sh"),
		[]byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command("tar", "-czf", dest, "-C", stage, topDir)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("tar: %v\n%s", err, out)
	}
}

// FINDING 6. A tarball whose top-level directory is not <id>/ failed the
// verification and reported an install failure — but the payload it DID
// extract was left in the platforms root, where the next reconcile discovers
// it and registers BAR's boot_target under platform:bar: an id the user never
// installed and which is not in the catalog.
func TestPlatformExtractLeavesNothingBehindOnMismatch(t *testing.T) {
	root := t.TempDir()
	tarball := filepath.Join(t.TempDir(), "p.tar.gz")
	makePlatformTarball(t, tarball, "bar")

	if err := extractPlatformPayload(root, "v", tarball); err == nil {
		t.Fatal("a tarball with the wrong top-level directory was accepted")
	}
	ents, err := os.ReadDir(root)
	if err != nil {
		t.Fatal(err)
	}
	if len(ents) != 0 {
		var names []string
		for _, d := range ents {
			names = append(names, d.Name())
		}
		t.Errorf("failed extraction left %v in the platforms root; reconcile ADOPTS it", names)
	}
}

func TestPlatformExtractInstallsMatchingPayload(t *testing.T) {
	root := t.TempDir()
	tarball := filepath.Join(t.TempDir(), "p.tar.gz")
	makePlatformTarball(t, tarball, "v")

	if err := extractPlatformPayload(root, "v", tarball); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "v", "platform.json")); err != nil {
		t.Errorf("payload not in place: %v", err)
	}
	if _, err := os.Stat(filepath.Join(root, "v", "sub", "entry.sh")); err != nil {
		t.Errorf("nested file not in place: %v", err)
	}
	ents, _ := os.ReadDir(root)
	if len(ents) != 1 {
		t.Errorf("platforms root holds %d entries, want just the payload", len(ents))
	}
}

// The catalog-supplied id becomes a PATH. Unvalidated, id ".." resolves the
// manifest check to /data/UserData/platform.json and the extraction to the
// parent of the platforms root.
func TestPlatformExtractRefusesBadID(t *testing.T) {
	root := t.TempDir()
	tarball := filepath.Join(t.TempDir(), "p.tar.gz")
	makePlatformTarball(t, tarball, "v")

	for _, id := range []string{"..", "../evil", "sub/dir", "", "V"} {
		if err := extractPlatformPayload(root, id, tarball); err == nil {
			t.Errorf("extractPlatformPayload accepted id %q", id)
		}
	}
	ents, _ := os.ReadDir(root)
	if len(ents) != 0 {
		t.Errorf("a refused id still wrote %d entries", len(ents))
	}
}

// FINDING 5. docs/BOOT_TARGETS.md is explicit that a target runs as ableton.
// The module path chowns -R; the platform path called the single,
// NON-recursive chownToAbleton, so every shipped file and subdirectory stayed
// root:root and any state the platform writes fails EACCES at boot — which, if
// the platform treats it as fatal, is three strikes and a forced picker with
// the cause invisible.
func TestPlatformChownIsRecursive(t *testing.T) {
	root := t.TempDir()
	if err := os.MkdirAll(filepath.Join(root, "sub", "deeper"), 0o755); err != nil {
		t.Fatal(err)
	}
	for _, p := range []string{"top.txt", "sub/mid.txt", "sub/deeper/leaf.txt"} {
		if err := os.WriteFile(filepath.Join(root, p), []byte("x"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	var seen []string
	restore := chownEach
	chownEach = func(p string) { seen = append(seen, p) }
	defer func() { chownEach = restore }()

	chownTreeToAbleton(root)

	want := []string{root,
		filepath.Join(root, "sub"),
		filepath.Join(root, "sub", "deeper"),
		filepath.Join(root, "sub", "deeper", "leaf.txt"),
		filepath.Join(root, "sub", "mid.txt"),
		filepath.Join(root, "top.txt")}
	got := map[string]bool{}
	for _, p := range seen {
		got[p] = true
	}
	for _, w := range want {
		if !got[w] {
			t.Errorf("chownTreeToAbleton skipped %s", w)
		}
	}
}

// And the install path must USE it. A recursive helper nobody calls is the
// same bug with a longer diff.
func TestPlatformInstallChownsRecursively(t *testing.T) {
	raw, err := os.ReadFile("platforms.go")
	if err != nil {
		t.Fatal(err)
	}
	src := string(raw)
	if strings.Contains(src, "chownToAbleton(payloadDir)") {
		t.Error("installPlatform still uses the non-recursive chownToAbleton on the payload; " +
			"every shipped file stays root:root and the platform's own state writes fail EACCES")
	}
	if !strings.Contains(src, "chownTreeToAbleton(payloadDir)") {
		t.Error("installPlatform does not chown the payload tree at all")
	}
}
