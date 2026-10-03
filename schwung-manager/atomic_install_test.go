package main

import (
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
}

// A replaced file must arrive at a NEW inode, and a handle open on the old
// one -- what a mapped dsp.so is, to the kernel -- must keep reading the old
// bytes. Writing through it in place is #474.
func TestInstallTreeAtomicallyNewInodeOldHandleIntact(t *testing.T) {
	dest := t.TempDir()
	staged := t.TempDir()
	writeFile(t, filepath.Join(dest, "mod/dsp.so"), "OLD-CODE")
	writeFile(t, filepath.Join(dest, "mod/roms/user.rom"), "USER")
	writeFile(t, filepath.Join(dest, "mod/config.json"), "{\"user\":1}")
	writeFile(t, filepath.Join(staged, "mod/dsp.so"), "NEW-CODE")
	writeFile(t, filepath.Join(staged, "mod/module.json"), "{}")
	writeFile(t, filepath.Join(staged, "mod/sub/dir/helper.so.1"), "NEW-HELPER")

	before, err := os.Stat(filepath.Join(dest, "mod/dsp.so"))
	if err != nil {
		t.Fatal(err)
	}
	held, err := os.Open(filepath.Join(dest, "mod/dsp.so")) // the "mapping"
	if err != nil {
		t.Fatal(err)
	}
	defer held.Close()

	replaced, err := installTreeAtomically(staged, dest)
	if err != nil {
		t.Fatal(err)
	}

	after, err := os.Stat(filepath.Join(dest, "mod/dsp.so"))
	if err != nil {
		t.Fatal(err)
	}
	if os.SameFile(before, after) {
		t.Error("dsp.so kept its inode: the update was written through the live file")
	}
	old, _ := io.ReadAll(held)
	if string(old) != "OLD-CODE" {
		t.Errorf("a handle on the old dsp.so read %q, want the old bytes intact", old)
	}
	if b, _ := os.ReadFile(filepath.Join(dest, "mod/dsp.so")); string(b) != "NEW-CODE" {
		t.Errorf("dsp.so = %q after install", b)
	}
	for _, keep := range []string{"mod/roms/user.rom", "mod/config.json"} {
		if _, err := os.Stat(filepath.Join(dest, keep)); err != nil {
			t.Errorf("%s was not in the tarball and must survive: %v", keep, err)
		}
	}
	if b, _ := os.ReadFile(filepath.Join(dest, "mod/sub/dir/helper.so.1")); string(b) != "NEW-HELPER" {
		t.Error("nested directories were not created")
	}
	if len(replaced) != 1 || replaced[0] != filepath.Join("mod", "dsp.so") {
		t.Errorf("replaced native = %v, want only mod/dsp.so (a NEW .so replaced nothing)", replaced)
	}
}

func TestInstallTreeAtomicallyConflicts(t *testing.T) {
	dest := t.TempDir()
	staged := t.TempDir()
	writeFile(t, filepath.Join(dest, "mod/thing/inner"), "x") // a directory
	writeFile(t, filepath.Join(staged, "mod/thing"), "file")  // the update has a file
	if _, err := installTreeAtomically(staged, dest); err == nil {
		t.Error("a file over a directory must be an error")
	}

	dest2 := t.TempDir()
	staged2 := t.TempDir()
	writeFile(t, filepath.Join(dest2, "mod/thing"), "file")
	writeFile(t, filepath.Join(staged2, "mod/thing/inner"), "x")
	if _, err := installTreeAtomically(staged2, dest2); err == nil {
		t.Error("a directory over a file must be an error")
	}
}

// End to end through the real tar, including the host update's
// --strip-components; the staging directory must not be left behind.
func TestExtractTarballAtomically(t *testing.T) {
	if _, err := exec.LookPath("tar"); err != nil {
		t.Skip("no tar")
	}
	src := t.TempDir()
	writeFile(t, filepath.Join(src, "schwung/modules/chain/dsp.so"), "NEW-CHAIN")
	writeFile(t, filepath.Join(src, "schwung/host/version.txt"), "9.9.9")
	tarPath := filepath.Join(t.TempDir(), "u.tar.gz")
	if out, err := exec.Command("tar", "-czf", tarPath, "-C", src, "schwung").CombinedOutput(); err != nil {
		t.Fatalf("tar: %v %s", err, out)
	}

	dest := t.TempDir()
	writeFile(t, filepath.Join(dest, "modules/chain/dsp.so"), "OLD-CHAIN")
	writeFile(t, filepath.Join(dest, "host/version.txt"), "1.0.0")
	before, _ := os.Stat(filepath.Join(dest, "modules/chain/dsp.so"))

	replaced, err := extractTarballAtomically(tarPath, dest, true, "--strip-components=1", "--exclude=*/host/version.txt")
	if err != nil {
		t.Fatal(err)
	}
	after, _ := os.Stat(filepath.Join(dest, "modules/chain/dsp.so"))
	if os.SameFile(before, after) {
		t.Error("chain dsp.so rewritten in place")
	}
	if b, _ := os.ReadFile(filepath.Join(dest, "host/version.txt")); string(b) != "1.0.0" {
		t.Errorf("excluded version.txt was touched: %q", b)
	}
	if len(replaced) != 1 {
		t.Errorf("replaced = %v", replaced)
	}
	entries, _ := os.ReadDir(dest)
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), ".staging-") {
			t.Errorf("staging dir %s left behind", e.Name())
		}
	}
}

// Source pin: no install path may go back to extracting over, or deleting,
// a live module / host directory. (The host update's later single-file
// extract of host/version.txt is fine: a text file nothing maps, placed last
// on purpose.)
func TestNoInPlaceInstallLeft(t *testing.T) {
	src, err := os.ReadFile("main.go")
	if err != nil {
		t.Fatal(err)
	}
	s := string(src)
	for _, bad := range []string{
		`exec.Command("tar", "-xzf", tmpPath, "-C", categoryDir)`,
		`extractCmd := exec.Command("tar", "-xzof", tarPath, "-C", app.basePath,`,
		`os.RemoveAll(destDir)`,
	} {
		if strings.Contains(s, bad) {
			t.Errorf("in-place install is back: %s", bad)
		}
	}
	if n := strings.Count(s, "installTreeAtomically(") + strings.Count(s, "extractTarballAtomically("); n < 4 {
		t.Errorf("expected all four install paths to go through the atomic helpers, found %d calls", n)
	}
}
