package main

import (
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// newTestApp builds the minimum App checkHostCompat touches: a base path and
// a logger. The logger matters — the unknown-version branch logs, and a nil
// one would panic on exactly the path that is hardest to reach in the field.
func newTestApp(t *testing.T, hostVersion string) *App {
	t.Helper()
	base := t.TempDir()
	if hostVersion != "" {
		if err := os.MkdirAll(filepath.Join(base, "host"), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(base, "host", "version.txt"),
			[]byte(hostVersion+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	return &App{
		basePath: base,
		logger:   slog.New(slog.NewTextHandler(io.Discard, nil)),
	}
}

func TestCheckHostCompat(t *testing.T) {
	cases := []struct {
		name        string
		hostVersion string // "" means host/version.txt is absent
		minVersion  string
		wantErr     bool
	}{
		{"no minimum declared", "0.9.0", "", false},
		{"host newer than minimum", "0.12.1", "0.7.13", false},
		{"host equals minimum", "0.12.1", "0.12.1", false},
		{"host older than minimum", "0.12.0", "0.12.1", true},
		// 0.9.16 must read as OLDER than 0.12.1: a lexical compare would call
		// it newer and let an incompatible module through.
		{"double-digit minor beats single", "0.9.16", "0.12.1", true},
		// The deliberate fallback: unreadable version.txt installs anyway
		// rather than bricking recovery. It must not panic while logging.
		{"version file missing", "", "0.12.1", false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := newTestApp(t, tc.hostVersion).checkHostCompat(tc.minVersion)
			if tc.wantErr && err == nil {
				t.Fatalf("min %q vs host %q: wanted an error, got nil",
					tc.minVersion, tc.hostVersion)
			}
			if !tc.wantErr && err != nil {
				t.Fatalf("min %q vs host %q: wanted nil, got %v",
					tc.minVersion, tc.hostVersion, err)
			}
			if tc.wantErr && !strings.Contains(err.Error(), tc.minVersion) {
				t.Fatalf("error should name the required version, got %q", err)
			}
		})
	}
}
