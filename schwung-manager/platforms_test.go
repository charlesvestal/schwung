// schwung-manager/platforms_test.go
package main

import (
	"encoding/json"
	"os"
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

// A tarball whose top-level directory is not <id>/ otherwise strews itself
// across the install root and then fails to register for a reason that reads
// as unrelated to the real problem.
func TestPlatformExtractionVerified(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	if err := os.MkdirAll(filepath.Join(root, "platforms", "wrongname"), 0o755); err != nil {
		t.Fatal(err)
	}
	err := verifyPlatformExtraction(base, "v")
	if err == nil {
		t.Fatal("want an error naming the missing platform.json, got nil")
	}
	if !strings.Contains(err.Error(), "platform.json") {
		t.Errorf("error %q does not name platform.json", err)
	}
}

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
