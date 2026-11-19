package main

import (
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
)

func TestSetPagesMovePreservesMetadataAndRejectsStale(t *testing.T) {
	root := t.TempDir()
	pages := filepath.Join(root, "set_pages")
	if err := os.MkdirAll(filepath.Join(pages, "page_1", "abc", "First"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(pages, "page_2"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pages, "current_page.txt"), []byte("0\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pages, "page_1", "xattrs.txt"), []byte("abc user.song-index 2\nabc user.song-color 4\n"), 0644); err != nil {
		t.Fatal(err)
	}
	s, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	if s.Pages[1][2] == nil || s.Pages[1][2].Name != "First" || s.Pages[1][2].Color == nil || *s.Pages[1][2].Color != 4 {
		t.Fatalf("wrong layout: %+v", s.Pages[1][2])
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[2][5] = "abc"
	if err := saveSetPages(root, s.Revision, layout); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(pages, "page_2", "abc", "First")); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(pages, "page_2", "xattrs.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "abc user.song-color 4\nabc user.song-index 5\n" {
		t.Fatalf("metadata: %s", data)
	}
	if err := saveSetPages(root, s.Revision, layout); err == nil {
		t.Fatal("accepted stale revision")
	}
}

func TestSetPagesRejectsChangedPageAndInvalidLayouts(t *testing.T) {
	root := t.TempDir()
	pages := filepath.Join(root, "set_pages")
	if err := os.MkdirAll(filepath.Join(pages, "page_1", "one", "Name"), 0755); err != nil {
		t.Fatal(err)
	}
	for path, data := range map[string]string{"current_page.txt": "0\n", "page_1/xattrs.txt": "one user.song-index 0\n", "page_1/one/Name/Song.abl": "original"} {
		if err := os.WriteFile(filepath.Join(pages, path), []byte(data), 0644); err != nil {
			t.Fatal(err)
		}
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[1][0] = "one"
	layout[0][1] = "one"
	layout[1][0] = ""
	if err = saveSetPages(root, snap.Revision, layout); err == nil {
		t.Fatal("accepted move into active page")
	}
	layout[0][1] = ""
	layout[1][0] = "one"
	layout[2][0] = "one"
	if err = saveSetPages(root, snap.Revision, layout); err == nil {
		t.Fatal("accepted duplicate set")
	}
	layout[2][0] = ""
	if err = os.WriteFile(filepath.Join(pages, "page_1/one/Name/Song.abl"), []byte("changed"), 0644); err != nil {
		t.Fatal(err)
	}
	if err = saveSetPages(root, snap.Revision, layout); err == nil {
		t.Fatal("accepted changed set")
	}
	if _, err = os.Stat(filepath.Join(pages, "page_1/one/Name/Song.abl")); err != nil {
		t.Fatal(err)
	}
}

func TestSetPagesRejectsPageChange(t *testing.T) {
	root := t.TempDir()
	pages := filepath.Join(root, "set_pages")
	if err := os.MkdirAll(pages, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pages, "current_page.txt"), []byte("0\n"), 0644); err != nil {
		t.Fatal(err)
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(pages, "current_page.txt"), []byte("1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	if err = saveSetPages(root, snap.Revision, layout); err == nil {
		t.Fatal("accepted changed active page")
	}
}

func TestSetPagesBlocksAfterInterruptedEdit(t *testing.T) {
	root := t.TempDir()
	pending := filepath.Join(root, "set_pages", ".manager-edit-interrupted")
	if err := os.MkdirAll(pending, 0755); err != nil {
		t.Fatal(err)
	}
	if _, err := readSetPages(root); err == nil {
		t.Fatal("accepted unfinished edit")
	}
}

func TestSetPagesSwapKeepsEachSetsAttributes(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "set_pages")
	for _, x := range []struct{ page, id, name, attrs string }{{"page_1", "a", "A", "a user.song-index 1\na user.song-color 7\n"}, {"page_2", "b", "B", "b user.song-index 3\nb user.song-color 9\n"}} {
		d := filepath.Join(base, x.page)
		if err := os.MkdirAll(filepath.Join(d, x.id, x.name), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(d, "xattrs.txt"), []byte(x.attrs), 0644); err != nil {
			t.Fatal(err)
		}
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[1][1] = "b"
	layout[2][3] = "a"
	if err = saveSetPages(root, snap.Revision, layout); err != nil {
		t.Fatal(err)
	}
	after, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	if after.Pages[1][1].ID != "b" || after.Pages[2][3].ID != "a" {
		t.Fatal("swap not saved")
	}
	if after.attrs["a"]["user.song-color"] != "7" || after.attrs["b"]["user.song-color"] != "9" {
		t.Fatal("set attributes changed")
	}
}

func TestSetPagesIgnoresOrphanMetadata(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "set_pages")
	a := filepath.Join(base, "page_1")
	b := filepath.Join(base, "page_2")
	if err := os.MkdirAll(filepath.Join(a, "one", "Song"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(b, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(a, "xattrs.txt"), []byte("one user.song-index 0\none user.song-color 2\n"), 0644); err != nil {
		t.Fatal(err)
	}
	// A collision in the device's move leaves lines behind for Sets that are
	// not in that page's directory; its restore ignores them.
	if err := os.WriteFile(filepath.Join(b, "xattrs.txt"), []byte("one user.song-color 19\nstray line\n"), 0644); err != nil {
		t.Fatal(err)
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	if snap.Pages[1][0] == nil || snap.Pages[1][0].ID != "one" {
		t.Fatal("set missing after orphan metadata")
	}
	if got := snap.attrs["one"]["user.song-color"]; got != "2" {
		t.Fatalf("orphan metadata overwrote the real Set: color %q", got)
	}
	if len(snap.location) != 1 {
		t.Fatalf("orphan metadata created a Set: %v", snap.location)
	}
}

func TestSetPagesRecoversMissingSlotMetadata(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "set_pages")
	if err := os.MkdirAll(filepath.Join(base, "page_1", "one", "Song"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "page_1", "xattrs.txt"), []byte("one user.song-color 3\n"), 0644); err != nil {
		t.Fatal(err)
	}
	original := setPageReadXattr
	defer func() { setPageReadXattr = original }()
	setPageReadXattr = func(path, key string) (string, error) {
		if key == "user.song-index" {
			return "5", nil
		}
		return "3", nil
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	if snap.Pages[1][5] == nil || snap.Pages[1][5].ID != "one" {
		t.Fatal("Set missing after recovering its slot")
	}
	if got := snap.attrs["one"]["user.song-index"]; got != "5" {
		t.Fatalf("recovered slot not healed in metadata: %q", got)
	}
}

func TestSetPagesRemovesStageAfterRolledBackSave(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "set_pages")
	for _, id := range []string{"a", "b"} {
		if err := os.MkdirAll(filepath.Join(base, "page_1", id, "Song"), 0755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(filepath.Join(base, "current_page.txt"), []byte("0\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(base, "page_1", "xattrs.txt"), []byte("a user.song-index 0\nb user.song-index 1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(base, "page_2"), 0755); err != nil {
		t.Fatal(err)
	}
	// A non-directory at the destination is a collision that readSetPages
	// ignores, so the save gets as far as the second move before failing.
	if err := os.WriteFile(filepath.Join(base, "page_2", "a"), []byte("blocker"), 0644); err != nil {
		t.Fatal(err)
	}
	snap, err := readSetPages(root)
	if err != nil {
		t.Fatal(err)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[2][0] = "a"
	layout[2][1] = "b"
	if err = saveSetPages(root, snap.Revision, layout); err == nil {
		t.Fatal("accepted destination collision")
	}
	entries, err := os.ReadDir(base)
	if err != nil {
		t.Fatal(err)
	}
	for _, entry := range entries {
		if strings.HasPrefix(entry.Name(), ".manager-edit-") {
			t.Fatalf("rolled-back save left %s behind", entry.Name())
		}
	}
	for _, id := range []string{"a", "b"} {
		if _, err := os.Stat(filepath.Join(base, "page_1", id, "Song")); err != nil {
			t.Fatalf("%s was not restored: %v", id, err)
		}
	}
	if _, err := readSetPages(root); err != nil {
		t.Fatalf("later load blocked: %v", err)
	}
}

func TestSetPagesRefusesToReadDuringPageChange(t *testing.T) {
	root := t.TempDir()
	lock, err := os.OpenFile(filepath.Join(root, setPagesLockName), os.O_CREATE|os.O_RDWR, 0666)
	if err != nil {
		t.Fatal(err)
	}
	defer lock.Close()
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		t.Fatal(err)
	}
	defer syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
	original := setPagesLockWait
	defer func() { setPagesLockWait = original }()
	setPagesLockWait = 0
	if _, err := readSetPages(root); err == nil || !strings.Contains(err.Error(), "in progress") {
		t.Fatalf("read during page change returned %v", err)
	}
}

func TestSetPagesShowsActiveSetsWithoutAllowingEdits(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	setDir := filepath.Join(root, "UserLibrary", "Sets", "active", "My Set")
	if err := os.MkdirAll(setDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(base, "set_pages"), 0755); err != nil {
		t.Fatal(err)
	}
	original := setPageReadXattr
	defer func() { setPageReadXattr = original }()
	setPageReadXattr = func(path, key string) (string, error) {
		if key == "user.song-index" {
			return "2", nil
		}
		return "4", nil
	}
	snap, err := readSetPages(base)
	if err != nil {
		t.Fatal(err)
	}
	if got := snap.Pages[0][2]; got == nil || got.Name != "My Set" || got.Color == nil || *got.Color != 4 {
		t.Fatalf("active set missing: %+v", got)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[0][2] = "active"
	if err = saveSetPages(base, snap.Revision, layout); err == nil {
		t.Fatal("accepted active page edit")
	}
}
