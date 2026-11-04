package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestSetPagesHandlersReadAndSave(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "set_pages", "page_1")
	if err := os.MkdirAll(filepath.Join(dir, "id", "Song"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "xattrs.txt"), []byte("id user.song-index 0\n"), 0644); err != nil {
		t.Fatal(err)
	}
	app := &App{basePath: root}
	read := httptest.NewRecorder()
	app.handleSetPagesRead(read, httptest.NewRequest("GET", "/api/set-pages", nil))
	if read.Code != http.StatusOK {
		t.Fatal(read.Body.String())
	}
	var snapshot setPageSnapshot
	if err := json.Unmarshal(read.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	layout := make([][]string, 8)
	for i := range layout {
		layout[i] = make([]string, 32)
	}
	layout[2][3] = "id"
	body, err := json.Marshal(map[string]any{"revision": snapshot.Revision, "pages": layout})
	if err != nil {
		t.Fatal(err)
	}
	write := httptest.NewRecorder()
	app.handleSetPagesSave(write, httptest.NewRequest("POST", "/api/set-pages", bytes.NewReader(body)))
	if write.Code != http.StatusNoContent {
		t.Fatalf("save: %d %s", write.Code, write.Body.String())
	}
	if _, err = os.Stat(filepath.Join(root, "set_pages", "page_2", "id", "Song")); err != nil {
		t.Fatal(err)
	}
	retry := httptest.NewRecorder()
	app.handleSetPagesSave(retry, httptest.NewRequest("POST", "/api/set-pages", bytes.NewReader(body)))
	if retry.Code != http.StatusConflict {
		t.Fatalf("stale save returned %d", retry.Code)
	}
}
