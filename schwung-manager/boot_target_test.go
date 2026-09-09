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

// FINDING 2. The chmod runs AS ROOT, three syscalls after the check, on a
// directory that has just been handed to user ableton — so ableton can swap a
// path component in between and get root to chmod 0755 an arbitrary file
// (/etc/shadow, a private key). Same class as the setuid-copy bug that needed
// O_NOFOLLOW on its tmp path.
//
// A full race is not reproducible here; what IS testable is the property the
// fix rests on: the chmod applies to a FILE HANDLE opened O_NOFOLLOW, so it
// can never land on the far end of a symlink.
func TestBootExecChmodDoesNotFollowSymlink(t *testing.T) {
	root := t.TempDir()
	outside := filepath.Join(root, "outside.txt")
	if err := os.WriteFile(outside, []byte("secret\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(root, "entry.sh")
	if err := os.Symlink(outside, link); err != nil {
		t.Fatal(err)
	}

	if err := chmodExecNoFollow(link); err == nil {
		t.Error("chmodExecNoFollow followed a symlink instead of refusing it")
	}
	info, err := os.Stat(outside)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0o600 {
		t.Errorf("the symlink's TARGET was chmodded to %v: root just changed a file "+
			"outside the payload directory", info.Mode().Perm())
	}
}

// FINDING 3. Containment was proven against EvalSymlinks(abs) and then the
// UNRESOLVED abs was stored, so a component swapped after install repoints
// what the selector actually runs — the guarantee the function advertises does
// not bind the path that gets executed.
func TestBootTargetStoresResolvedExec(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "v")
	writePayload(t, dir, `{"id":"v","name":"V","version":"0.1.0",`+
		`"boot_target":{"name":"V","exec":"lib/entry.sh"}}`, "real/entry.sh")
	if err := os.Symlink(filepath.Join(dir, "real"), filepath.Join(dir, "lib")); err != nil {
		t.Fatal(err)
	}

	bt, err := parseBootTarget(filepath.Join(dir, "module.json"), "v", dir)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	// The symlinked component INSIDE the payload — the region ableton owns —
	// must be gone from the stored path. The payloadDir prefix is left as the
	// caller composed it (see resolveBootExec).
	want := filepath.Join(dir, "real", "entry.sh")
	if bt.ExecAbs != want {
		t.Errorf("ExecAbs = %q, want the RESOLVED %q", bt.ExecAbs, want)
	}
	if strings.Contains(bt.ExecAbs, "/lib/") {
		t.Errorf("ExecAbs %q still runs through the symlink containment was proven against", bt.ExecAbs)
	}
}

// The spec's testing section names this case and it had no test: an exec whose
// resolved path leaves the payload directory must be refused. "..", which is
// covered above, is caught by the textual check and never reaches the
// symlink-aware one.
func TestBootTargetRefusesSymlinkEscapeAndDirectory(t *testing.T) {
	t.Run("symlink escape", func(t *testing.T) {
		root := t.TempDir()
		outside := filepath.Join(root, "outside.sh")
		if err := os.WriteFile(outside, []byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
		dir := filepath.Join(root, "v")
		writePayload(t, dir, `{"id":"v","name":"V","version":"0.1.0",`+
			`"boot_target":{"name":"V","exec":"entry.sh"}}`, "")
		if err := os.Symlink(outside, filepath.Join(dir, "entry.sh")); err != nil {
			t.Fatal(err)
		}
		_, err := parseBootTarget(filepath.Join(dir, "module.json"), "v", dir)
		if err == nil || !strings.Contains(err.Error(), "outside") {
			t.Fatalf("err = %v, want a refusal naming the payload directory", err)
		}
	})

	t.Run("exec is a directory", func(t *testing.T) {
		dir := filepath.Join(t.TempDir(), "v")
		writePayload(t, dir, `{"id":"v","name":"V","version":"0.1.0",`+
			`"boot_target":{"name":"V","exec":"bin"}}`, "bin/run.sh")
		_, err := parseBootTarget(filepath.Join(dir, "module.json"), "v", dir)
		if err == nil || !strings.Contains(err.Error(), "directory") {
			t.Fatalf("err = %v, want a refusal naming that it is a directory", err)
		}
	})
}

// An id collision between two payloads: one wins deterministically, the other
// is recorded as UNSETTLED rather than dropped silently — an unsettled owner
// is what stops reconcile deleting the live row out from under it.
func TestDesiredBootTargetsIDCollision(t *testing.T) {
	app, base, _ := newReconcileApp(t)
	plantModuleTarget(t, base, "tools", "aaa", "vee", "A")
	plantModuleTarget(t, base, "tools", "bbb", "vee", "B")

	desired, err := app.desiredBootTargets()
	if err == nil {
		t.Fatal("a collision must be reported, got nil")
	}
	for _, want := range []string{"vee", "module:aaa", "module:bbb"} {
		if !strings.Contains(err.Error(), want) {
			t.Errorf("collision error does not name %q: %v", want, err)
		}
	}
	if len(desired.byID) != 1 || desired.byID["vee"].entry.Owner != "module:aaa" {
		t.Fatalf("desired = %+v, want the sort-first claimant module:aaa alone", desired.byID)
	}
	if desired.unsettled["module:bbb"] == "" {
		t.Error("the loser was not recorded as unsettled: reconcile would delete its live row")
	}
	if !desired.installedOwners["module:aaa"] || !desired.installedOwners["module:bbb"] {
		t.Error("both payloads are installed; both owners must be present")
	}
}
