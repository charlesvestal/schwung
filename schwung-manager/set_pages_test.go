package main

import (
	"os"
	"path/filepath"
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

func TestSetPagesRejectsMetadataForSetOnAnotherPage(t *testing.T) {
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
	if err := os.WriteFile(filepath.Join(b, "xattrs.txt"), []byte("one user.song-color 19\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := readSetPages(root); err == nil {
		t.Fatal("accepted metadata for a Set on a different page")
	}
}
