package main

import (
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"testing"
)

// The manager and the shim start independently and the manager can WIN.
// Measured on the device: the manager came up at 07:40:41.930 and logged
// "shared memory params: not available", and App.shmParams then stayed nil for
// the life of the process - while RemoteUI independently re-attached a second
// mapping and worked fine. The CPU page read the App field and reported
// "(name unread)" for every position.
func TestParamsAttachesAfterStartup(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "schwung-param")

	orig := shmParamPath
	shmParamPath = path
	t.Cleanup(func() { shmParamPath = orig })

	app := &App{logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	if app.params() != nil {
		t.Fatal("wanted nil while the segment does not exist")
	}

	if err := os.WriteFile(path, make([]byte, shmParamSize), 0o644); err != nil {
		t.Fatal(err)
	}
	got := app.params()
	if got == nil {
		t.Fatal("the consumer must retry - a segment that appears after startup " +
			"is the normal case on a cold boot, not an edge case")
	}
	if app.params() != got {
		t.Fatal("params() must cache the mapping after a successful attach")
	}
}

// Two mappings of one segment is a bug waiting to happen and was the reason
// the CPU page and the Remote UI disagreed about whether params worked.
func TestRemoteUISharesTheAppHandle(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "schwung-param")
	orig := shmParamPath
	shmParamPath = path
	t.Cleanup(func() { shmParamPath = orig })
	if err := os.WriteFile(path, make([]byte, shmParamSize), 0o644); err != nil {
		t.Fatal(err)
	}

	app := &App{logger: slog.New(slog.NewTextHandler(io.Discard, nil))}
	ru := &RemoteUI{app: app, logger: app.logger}
	if ru.ensureShm() != app.params() {
		t.Fatal("RemoteUI must share App's handle, not open a second mapping")
	}
}
