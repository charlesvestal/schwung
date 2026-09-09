package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writePayload(t *testing.T, dir, manifest string, execName string) {
	t.Helper()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
	if execName != "" {
		p := filepath.Join(dir, execName)
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte("#!/bin/sh\nexit 0\n"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
}

func TestBootTargetParseAndValidate(t *testing.T) {
	cases := []struct {
		name     string
		block    string
		execName string
		wantErr  string // substring; "" means accepted
		wantID   string
	}{
		{name: "absent", block: "", wantErr: "", wantID: ""},
		{name: "minimal", block: `,"boot_target":{"name":"V","exec":"entry.sh"}`,
			execName: "entry.sh", wantID: "v"},
		{name: "explicit id", block: `,"boot_target":{"id":"vee","name":"V","exec":"entry.sh"}`,
			execName: "entry.sh", wantID: "vee"},
		{name: "reserved schwung", block: `,"boot_target":{"id":"schwung","name":"V","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "reserved"},
		{name: "reserved stock", block: `,"boot_target":{"id":"stock","name":"V","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "reserved"},
		{name: "bad id charset", block: `,"boot_target":{"id":"V Platform","name":"V","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "id"},
		{name: "name with quote", block: `,"boot_target":{"name":"V \"the\" one","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "name"},
		{name: "name with backslash", block: `,"boot_target":{"name":"V\\x","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "name"},
		{name: "name too long", block: `,"boot_target":{"name":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "name"},
		{name: "empty name", block: `,"boot_target":{"name":"","exec":"entry.sh"}`,
			execName: "entry.sh", wantErr: "name"},
		{name: "absolute exec", block: `,"boot_target":{"name":"V","exec":"/bin/sh"}`,
			execName: "entry.sh", wantErr: "exec"},
		{name: "dotdot exec", block: `,"boot_target":{"name":"V","exec":"../evil.sh"}`,
			execName: "entry.sh", wantErr: "exec"},
		{name: "missing exec", block: `,"boot_target":{"name":"V","exec":"nope.sh"}`,
			execName: "entry.sh", wantErr: "exec"},
		{name: "nested exec ok", block: `,"boot_target":{"name":"V","exec":"bin/run.sh"}`,
			execName: "bin/run.sh", wantID: "v"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := filepath.Join(t.TempDir(), "v")
			writePayload(t, dir, `{"id":"v","name":"V","version":"0.1.0"`+tc.block+`}`, tc.execName)
			bt, err := parseBootTarget(filepath.Join(dir, "module.json"), "v", dir)
			if tc.wantErr != "" {
				if err == nil {
					t.Fatalf("want error containing %q, got nil (bt=%+v)", tc.wantErr, bt)
				}
				if !strings.Contains(err.Error(), tc.wantErr) {
					t.Fatalf("error %q does not name %q", err, tc.wantErr)
				}
				return
			}
			if err != nil {
				t.Fatalf("unexpected error: %v", err)
			}
			if tc.wantID == "" {
				if bt != nil {
					t.Fatalf("want no target, got %+v", bt)
				}
				return
			}
			if bt == nil || bt.ID != tc.wantID {
				t.Fatalf("got %+v, want id %q", bt, tc.wantID)
			}
		})
	}
}

// A present but non-executable entry script is chmodded, not refused: a lost
// mode bit in transit would otherwise fall straight through the selector's
// [ ! -x ] check into stock Move, with nothing said anywhere.
func TestBootTargetChmodsExec(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "v")
	writePayload(t, dir, `{"id":"v","name":"V","version":"0.1.0","boot_target":{"name":"V","exec":"entry.sh"}}`, "entry.sh")
	if err := os.Chmod(filepath.Join(dir, "entry.sh"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := parseBootTarget(filepath.Join(dir, "module.json"), "v", dir); err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	info, err := os.Stat(filepath.Join(dir, "entry.sh"))
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm()&0o111 == 0 {
		t.Errorf("exec not made executable: mode %v", info.Mode().Perm())
	}
}
