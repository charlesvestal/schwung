// schwung-manager/boot_target.go
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"syscall"
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
	if err := checkBootTargetShadowing(raw); err != nil {
		return nil, err
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
		if err := chmodExecNoFollow(realExec); err != nil {
			return "", fmt.Errorf("boot_target exec %q: not executable and chmod failed: %w", exec, err)
		}
	}
	// What gets stored is the RESOLVED path, not `abs`. Containment was proven
	// against realExec, so storing the unresolved one means the guarantee this
	// function advertises does not bind what the selector runs: every
	// component inside the payload belongs to user ableton, and swapping one
	// after install silently repoints the boot entry at anything on the
	// device.
	//
	// Resolution is re-anchored on the CALLER's payloadDir rather than handing
	// back realExec verbatim. Those differ only when a component of payloadDir
	// itself is a symlink — and payloadDir is composed by the manager from its
	// own roots, whose parents are root-owned (a payload directory's own name
	// cannot be swapped without write access to the install root), so there is
	// nothing untrusted in that prefix to resolve. Everything below it, which
	// is the whole attacker-controlled region, is resolved.
	rel, err := filepath.Rel(realDir, realExec)
	if err != nil {
		return "", fmt.Errorf("boot_target exec %q: cannot be resolved inside the payload directory: %w", exec, err)
	}
	return filepath.Join(payloadDir, rel), nil
}

// chmodExecNoFollow makes one FILE executable, never whatever a symlink at
// that path points to.
//
// The manager runs as root and the payload directory has just been handed to
// user ableton, so between the EvalSymlinks/Stat above and this call ableton
// can swap a path component and have root chmod 0755 an arbitrary file —
// /etc/shadow, a private key. os.Chmod resolves the path afresh and would
// follow it. Opening with O_NOFOLLOW and chmodding the HANDLE makes the check
// and the change apply to the same object. Same failure class as the setuid
// copy that needed O_NOFOLLOW on its tmp path.
func chmodExecNoFollow(path string) error {
	f, err := os.OpenFile(path, os.O_RDONLY|syscall.O_NOFOLLOW, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	return f.Chmod(0o755)
}

// checkBootTargetShadowing refuses a manifest whose boot_target block would
// change what the HOST reads as the module's own identity.
//
// json_get_string (src/host/module_manager.c:14) locates a key with strstr and
// takes the FIRST TEXTUAL OCCURRENCE, with no nesting awareness — the same
// first-occurrence hazard documented for the two boot.json readers. It is how
// the host reads "id" and "name" out of module.json, so a boot_target block
// placed before those keys hands the host the block's values instead: the
// module appears under the wrong name, or worse, loads under the wrong id.
//
// Reproduced on hardware 2026-09-09. Registration is refused rather than the
// manifest rewritten, because the host on an OLD device reads the same file
// and no fix of ours reaches it — the author has to move the block.
//
// The detection replicates the host's rule exactly (first occurrence of the
// quoted key in the raw manifest bytes) instead of approximating it, so a
// manifest the host reads correctly is never refused.
// hostReadManifestKeys is every key json_get_string / json_get_int pull out of
// module.json in src/host/module_manager.c. Keep it in step with that file: a
// key read there and missing here is a shadowing route nothing checks.
var hostReadManifestKeys = []string{
	"id", "name", "version", "ui", "dsp", "component_type", "scan_packs",
}

func checkBootTargetShadowing(raw []byte) error {
	start, end, ok := jsonObjectSpan(raw, "boot_target")
	if !ok {
		return nil
	}
	// Every key json_get_string reads out of module.json, not just the two
	// our block happens to carry today. Checking the host's ACTUAL read list
	// against the block's ACTUAL span is exactly as precise as checking two
	// names, and it does not quietly stop covering the block the day it grows
	// a "version" field.
	for _, key := range hostReadManifestKeys {
		i := bytes.Index(raw, []byte(`"`+key+`"`))
		if i >= start && i < end {
			return fmt.Errorf("boot_target block precedes the manifest's own %q key: "+
				"the host reads module.json by first occurrence, so it would read %q "+
				"out of the block; move boot_target below the manifest's own keys", key, key)
		}
	}
	return nil
}

// jsonObjectSpan returns the byte range of `"key": { ... }`, from the opening
// quote of the key through the closing brace, found the way the host finds it
// (first textual occurrence) and delimited by brace counting that skips string
// literals. Reports ok=false when the key is absent or its value is not an
// object we can delimit — in which case there is nothing to refuse.
func jsonObjectSpan(raw []byte, key string) (start, end int, ok bool) {
	start = bytes.Index(raw, []byte(`"`+key+`"`))
	if start < 0 {
		return 0, 0, false
	}
	i := start + len(key) + 2
	for i < len(raw) && (raw[i] == ' ' || raw[i] == '\t' || raw[i] == '\n' || raw[i] == '\r' || raw[i] == ':') {
		i++
	}
	if i >= len(raw) || raw[i] != '{' {
		return 0, 0, false
	}
	depth, inStr, esc := 0, false, false
	for ; i < len(raw); i++ {
		c := raw[i]
		switch {
		case esc:
			esc = false
		case inStr && c == '\\':
			esc = true
		case c == '"':
			inStr = !inStr
		case inStr:
		case c == '{':
			depth++
		case c == '}':
			depth--
			if depth == 0 {
				return start, i + 1, true
			}
		}
	}
	return 0, 0, false
}
