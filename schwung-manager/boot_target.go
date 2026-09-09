// schwung-manager/boot_target.go
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// bootTargetNameMax is a registration-time cap, and it is a parser constraint
// rather than taste: bs_row_t.name is 64 bytes (src/host/boot_select_core.h)
// and the boot window is 128px wide, so anything long is cut twice over with
// nothing said.
const bootTargetNameMax = 24

var bootTargetIDRe = regexp.MustCompile(`^[a-z0-9-]+$`)

// BootTarget is a validated boot_target block: what the payload declared,
// resolved against where it was installed.
type BootTarget struct {
	ID      string // registry directory name
	Name    string // what the picker displays
	ExecAbs string // absolute path to the entry script
}

type bootTargetBlock struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	Exec string `json:"exec"`
}

// parseBootTarget reads the boot_target block from a manifest and validates it
// against the directory the payload was installed into. Returns (nil, nil)
// when the manifest declares no boot target — that is the common case, not an
// error. Every refusal names the field and the value.
func parseBootTarget(manifestPath, payloadID, payloadDir string) (*BootTarget, error) {
	raw, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil, fmt.Errorf("reading %s: %w", manifestPath, err)
	}
	var doc struct {
		BootTarget *bootTargetBlock `json:"boot_target"`
	}
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("parsing %s: %w", manifestPath, err)
	}
	if doc.BootTarget == nil {
		return nil, nil
	}
	b := doc.BootTarget

	id := b.ID
	if id == "" {
		id = payloadID
	}
	if !bootTargetIDRe.MatchString(id) {
		return nil, fmt.Errorf("boot_target id %q: must match [a-z0-9-]+", id)
	}
	if id == "schwung" || id == "stock" {
		return nil, fmt.Errorf("boot_target id %q is reserved by the selector", id)
	}
	if err := validateBootTargetName(b.Name); err != nil {
		return nil, err
	}
	execAbs, err := resolveBootExec(payloadDir, b.Exec)
	if err != nil {
		return nil, err
	}
	return &BootTarget{ID: id, Name: b.Name, ExecAbs: execAbs}, nil
}

// validateBootTargetName enforces what BOTH boot.json readers can survive.
// bt_json_field (awk, boot_target_lib.sh) truncates a value at its first '"';
// bs_json_field (C, boot_select_core.c) calls the same value malformed and
// falls back to the directory name. Neither can unescape, so a name carrying
// '"' or '\' is refused here rather than escaped downstream.
func validateBootTargetName(name string) error {
	if name == "" {
		return fmt.Errorf("boot_target name: must not be empty")
	}
	if len(name) > bootTargetNameMax {
		return fmt.Errorf("boot_target name %q: longer than %d characters", name, bootTargetNameMax)
	}
	if strings.ContainsAny(name, "\"\\") {
		return fmt.Errorf("boot_target name %q: must not contain a quote or backslash "+
			"(the selector's JSON readers cannot unescape)", name)
	}
	for _, r := range name {
		if r < 0x20 || r > 0x7e {
			return fmt.Errorf("boot_target name %q: printable ASCII only", name)
		}
	}
	return nil
}

// resolveBootExec turns a declared relative exec into an absolute path inside
// payloadDir, or refuses. A payload never states where it is installed; this
// is the only place the two halves are joined.
func resolveBootExec(payloadDir, exec string) (string, error) {
	if exec == "" {
		return "", fmt.Errorf("boot_target exec: must not be empty")
	}
	if filepath.IsAbs(exec) {
		return "", fmt.Errorf("boot_target exec %q: must be relative to the payload directory", exec)
	}
	cleaned := filepath.Clean(exec)
	if cleaned == ".." || strings.HasPrefix(cleaned, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("boot_target exec %q: must not leave the payload directory", exec)
	}
	abs := filepath.Join(payloadDir, cleaned)

	// Symlink-aware containment: resolve both sides before comparing, so a
	// symlink inside the payload cannot point the selector at /bin/sh.
	realDir, err := filepath.EvalSymlinks(payloadDir)
	if err != nil {
		return "", fmt.Errorf("boot_target exec %q: payload directory unreadable: %w", exec, err)
	}
	realExec, err := filepath.EvalSymlinks(abs)
	if err != nil {
		return "", fmt.Errorf("boot_target exec %q: not found after extraction", exec)
	}
	if realExec != realDir && !strings.HasPrefix(realExec, realDir+string(filepath.Separator)) {
		return "", fmt.Errorf("boot_target exec %q: resolves outside the payload directory", exec)
	}

	info, err := os.Stat(realExec)
	if err != nil {
		return "", fmt.Errorf("boot_target exec %q: not found after extraction", exec)
	}
	if info.IsDir() {
		return "", fmt.Errorf("boot_target exec %q: is a directory", exec)
	}
	if info.Mode().Perm()&0o111 == 0 {
		if err := os.Chmod(realExec, 0o755); err != nil {
			return "", fmt.Errorf("boot_target exec %q: not executable and chmod failed: %w", exec, err)
		}
	}
	return abs, nil
}
