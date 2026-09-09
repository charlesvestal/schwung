# Boot-Target Registration Through Schwung Manager — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let anything Schwung Manager installs — an ordinary module that ships a binary, or a new "platform" payload with no module in it — register a boot-selector target, and take that target away again when it is uninstalled.

**Architecture:** A payload declares `boot_target: {name, exec}` in its manifest, `exec` relative to its own directory. The manager validates it and writes `/data/UserData/boot-targets/<id>/boot.json` carrying an `owner` field; the manager only ever rewrites or deletes entries whose `owner` it recognises, so hand-installed targets and Schwung's self-registered entry are untouchable. A reconcile pass at manager start and after every install/uninstall makes the registry agree with what is on disk. Every absolute path is composed from three roots held in one place.

**Tech Stack:** Go 1.26 (`schwung-manager`, stdlib only), BusyBox sh (`tests/host/*.sh`), awk (`boot_target_lib.sh`), html/template.

**User decisions (already made):**
- "Both, one mechanism" — a module that ships a binary and a platform with no module both register the same way.
- Declarative `boot_target` block in the manifest; **no publisher scripts run as root**.
- New `platforms[]` catalog section installing to `/data/UserData/platforms/` — "we will install a module that has a platforms category after this ships."
- `exec` is the plain resolved path, **not** a generated launcher script ("the 'generated script' sounds like more things to fail") — staleness is handled by holding the roots in one place and reconciling.
- In scope: Boot page, reconcile on manager start, uninstall heals `default`, platform update detection.

**Spec:** `docs/superpowers/specs/2026-09-09-manager-boot-target-registration-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `schwung-manager/payload_paths.go` (new) | The only place that knows where payloads and the registry live. Category-dir enumeration derived from `getInstallSubdir`. |
| `schwung-manager/boot_target.go` (new) | Manifest `boot_target` block: parse + validate. No I/O beyond stat/chmod of the declared exec. |
| `schwung-manager/boot_registry.go` (new) | Read/write the registry: flat `boot.json`, `default`, entry listing and removal. |
| `schwung-manager/boot_reconcile.go` (new) | Desired-vs-actual reconcile, the one place that decides to create/rewrite/delete. |
| `schwung-manager/platforms.go` (new) | `CatalogPlatform`, platform discovery, install, uninstall, update check. |
| `schwung-manager/main.go` (modify) | Route wiring, handlers, install/uninstall call sites, startup reconcile. |
| `schwung-manager/templates/boot.html`, `platforms.html` (new) | The two new pages. |
| `tests/host/test_boot_target_manager_json.sh` (new) | Cross-language pin: the shell reader against the Go writer's output. |
| `docs/BOOT_TARGETS.md`, `docs/MODULES.md`, `CLAUDE.md` (modify) | Contract documentation. |

---

### Task 1: One source of truth for payload roots

**Goal:** Every install root and category directory is enumerated in one place, and the enumeration cannot disagree with `getInstallSubdir`.

**Why this is first:** `findModuleDir` (`main.go:987`) and `discoverInstalledModules` (`main.go:403`) each restate a six-entry directory list, and **both omit `modules/utilities` and `modules/other`** — the two directories `getInstallSubdir` produces for `component_type: utility` and for an unknown or absent type. Today that is latent (the catalog has none), but reconcile decides an owned target is orphaned by *not finding* its payload, so a module installed into a directory nobody enumerates would have its picker row deleted on every manager start.

**Files:**
- Create: `schwung-manager/payload_paths.go`
- Create: `schwung-manager/payload_paths_test.go`
- Modify: `schwung-manager/main.go:987-996` (`findModuleDir`), `main.go:403-413` (`discoverInstalledModules` dir list), `main.go:1134-1152` (`getInstallSubdir`)

**Acceptance Criteria:**
- [ ] Every value `getInstallSubdir` can return appears in the enumerated module category dirs, asserted by a test that drives `getInstallSubdir` rather than restating its output.
- [ ] A module planted in `modules/utilities/<id>` is found by both `findModuleDir` and `discoverInstalledModules`.
- [ ] `bootTargetsDir()` honours `BOOT_TARGETS_DIR`, defaulting to `/data/UserData/boot-targets` — the same name and default `boot_target_lib.sh` and `boot-select.c` already use.
- [ ] `platformsRoot()` is `/data/UserData/platforms` when `basePath` is `/data/UserData/schwung`.

**Verify:** `cd schwung-manager && go test ./... -run TestPayloadPaths -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/payload_paths_test.go
package main

import (
	"os"
	"path/filepath"
	"testing"
)

// Every subdir getInstallSubdir can produce must be a directory we enumerate.
// Driven off getInstallSubdir so the two cannot drift; restating its output
// here would just be a second copy of the bug this test exists to prevent.
func TestPayloadPathsCoverEveryInstallSubdir(t *testing.T) {
	types := []string{
		"sound_generator", "audio_fx", "midi_fx", "utility",
		"overtake", "tool", "system", "featured", "", "nonsense",
	}
	base := "/data/UserData/schwung"
	dirs := map[string]bool{}
	for _, d := range moduleCategoryDirs(base) {
		dirs[d] = true
	}
	for _, ct := range types {
		want := filepath.Join(base, "modules", getInstallSubdir(ct))
		if !dirs[want] {
			t.Errorf("component_type %q installs to %s, which is not enumerated", ct, want)
		}
	}
}

func TestPayloadPathsFindsUtilityModule(t *testing.T) {
	base := t.TempDir()
	dir := filepath.Join(base, "modules", "utilities", "widget")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"),
		[]byte(`{"id":"widget","name":"Widget","version":"1.0.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	app := &App{basePath: base}
	if got := app.findModuleDir("widget"); got != dir {
		t.Errorf("findModuleDir = %q, want %q", got, dir)
	}
	if _, ok := discoverInstalledModules(base)["widget"]; !ok {
		t.Error("discoverInstalledModules did not find the utility module")
	}
}

func TestPayloadPathsRoots(t *testing.T) {
	if got := platformsRoot("/data/UserData/schwung"); got != "/data/UserData/platforms" {
		t.Errorf("platformsRoot = %q", got)
	}
	t.Setenv("BOOT_TARGETS_DIR", "")
	if got := bootTargetsDir(); got != "/data/UserData/boot-targets" {
		t.Errorf("bootTargetsDir default = %q", got)
	}
	t.Setenv("BOOT_TARGETS_DIR", "/tmp/fixture")
	if got := bootTargetsDir(); got != "/tmp/fixture" {
		t.Errorf("bootTargetsDir env = %q", got)
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestPayloadPaths -v`
Expected: FAIL — `undefined: moduleCategoryDirs`, `undefined: platformsRoot`, `undefined: bootTargetsDir`.

- [ ] **Step 3: Write `payload_paths.go`**

```go
// schwung-manager/payload_paths.go
package main

import (
	"os"
	"path/filepath"
)

// installSubdirs is the full range of getInstallSubdir. It is the ONE list:
// getInstallSubdir maps a component_type into it, and moduleCategoryDirs
// enumerates it. Two hand-written lists is how modules/utilities and
// modules/other came to be installable but not discoverable.
var installSubdirs = []string{
	"sound_generators", "audio_fx", "midi_fx",
	"utilities", "overtake", "tools", "other",
}

// moduleCategoryDirs is every directory a module can be installed into,
// including the bare modules/ root that built-ins live in.
func moduleCategoryDirs(basePath string) []string {
	dirs := []string{filepath.Join(basePath, "modules")}
	for _, sub := range installSubdirs {
		dirs = append(dirs, filepath.Join(basePath, "modules", sub))
	}
	return dirs
}

// platformsRoot is where platform payloads live: a sibling of the Schwung
// tree, deliberately NOT under modules/, so the host's module scanner cannot
// surface a platform as a module with no ui.js.
func platformsRoot(basePath string) string {
	return filepath.Join(filepath.Dir(basePath), "platforms")
}

// bootTargetsDir is the boot-selector registry root. BOOT_TARGETS_DIR is not a
// new name: boot_target_lib.sh and boot-select.c already read it with this
// same default. The manager is the third reader, not a second hardcoded path.
func bootTargetsDir() string {
	if v := os.Getenv("BOOT_TARGETS_DIR"); v != "" {
		return v
	}
	return "/data/UserData/boot-targets"
}
```

- [ ] **Step 4: Point the two existing enumerations at it**

In `main.go`, replace the body of `findModuleDir`:

```go
func (app *App) findModuleDir(id string) string {
	for _, d := range moduleCategoryDirs(app.basePath) {
		candidate := filepath.Join(d, id)
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			return candidate
		}
	}
	return ""
}
```

and in `discoverInstalledModules`, replace the literal `dirs := []string{...}` with:

```go
	dirs := moduleCategoryDirs(base)
```

Rewrite `getInstallSubdir` to return members of `installSubdirs` (same mapping, no behaviour change — the switch stays, so the mapping is still explicit; only the *range* is now shared):

```go
// getInstallSubdir maps component_type to the install subdirectory name.
// Every value returned here MUST be in installSubdirs (payload_paths.go) —
// TestPayloadPathsCoverEveryInstallSubdir fails if one is not.
func getInstallSubdir(componentType string) string {
	switch componentType {
	case "sound_generator":
		return "sound_generators"
	case "audio_fx":
		return "audio_fx"
	case "midi_fx":
		return "midi_fx"
	case "utility":
		return "utilities"
	case "overtake":
		return "overtake"
	case "tool":
		return "tools"
	default:
		return "other"
	}
}
```

- [ ] **Step 5: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestPayloadPaths -v && go test ./...`
Expected: PASS, and the existing suite still green.

- [ ] **Step 6: Prove the test can fail**

Temporarily delete `"utilities"` from `installSubdirs`, re-run — `TestPayloadPathsCoverEveryInstallSubdir` must fail naming `component_type "utility"`. Restore.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/payload_paths.go schwung-manager/payload_paths_test.go schwung-manager/main.go
git commit -m "fix: enumerate every install subdir from one list (utilities/other were installable but not discoverable)"
```

---

### Task 2: Parse and validate the `boot_target` manifest block

**Goal:** A manifest's `boot_target` block becomes a validated value, or a refusal that names its reason — never a half-registered target.

**Files:**
- Create: `schwung-manager/boot_target.go`
- Create: `schwung-manager/boot_target_test.go`

**Acceptance Criteria:**
- [ ] `id` defaults to the payload id; rejected when it is not `^[a-z0-9-]+$`, or is `schwung` or `stock`.
- [ ] `name` is rejected when empty, longer than 24 characters, non-printable-ASCII, or contains `"` or `\` (the two `boot.json` parsers disagree about escapes and neither can unescape — see spec §2a).
- [ ] `exec` is rejected when absolute, when it contains `..`, when it escapes the payload directory via symlink or otherwise, and when it does not exist after extraction.
- [ ] A present-but-non-executable `exec` is chmodded to 0755 rather than rejected.
- [ ] A manifest with no `boot_target` block yields `(nil, nil)` — not an error.
- [ ] Every rejection returns an error whose message names the offending field and value.

**Verify:** `cd schwung-manager && go test ./... -run TestBootTarget -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/boot_target_test.go
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
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestBootTarget -v`
Expected: FAIL — `undefined: parseBootTarget`.

- [ ] **Step 3: Write `boot_target.go`**

```go
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
	ID       string // registry directory name
	Name     string // what the picker displays
	ExecAbs  string // absolute path to the entry script
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
```

- [ ] **Step 4: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestBootTarget -v`
Expected: PASS, every subtest.

- [ ] **Step 5: Commit**

```bash
git add schwung-manager/boot_target.go schwung-manager/boot_target_test.go
git commit -m "feat: parse and validate the boot_target manifest block"
```

---

### Task 3: The registry writer and reader

**Goal:** Registry entries are written in the one shape both selector parsers can read, carry an `owner`, and can be listed, matched and removed by owner.

**Files:**
- Create: `schwung-manager/boot_registry.go`
- Create: `schwung-manager/boot_registry_test.go`
- Create: `schwung-manager/testdata/boot.json.golden`

**Acceptance Criteria:**
- [ ] `boot.json` is written flat: one `"key": "value"` per line, string values only, no nesting.
- [ ] The written entry carries `name`, `exec`, `version`, `owner`.
- [ ] `listEntries` returns every registry directory holding a `boot.json`, including ones with no `owner`.
- [ ] `findEntryByOwner` locates an entry whose id differs from the payload id.
- [ ] `writeDefault` writes a single bare id line; `readDefault` reads it back ignoring trailing whitespace.
- [ ] The committed golden file matches what the writer produces (a drift check, not decoration).

**Verify:** `cd schwung-manager && go test ./... -run TestBootRegistry -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/boot_registry_test.go
package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBootRegistryWriteIsFlat(t *testing.T) {
	dir := t.TempDir()
	e := registryEntry{ID: "v", Name: "V", Exec: "/data/UserData/platforms/v/entry.sh",
		Version: "0.3.0", Owner: "platform:v"}
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(filepath.Join(dir, "v", "boot.json"))
	if err != nil {
		t.Fatal(err)
	}
	got := string(raw)

	// The shell reader is a per-line awk matcher: every field must sit alone
	// on its own line or the selector cannot see it.
	for _, key := range []string{"name", "exec", "version", "owner"} {
		var found int
		for _, line := range strings.Split(got, "\n") {
			if strings.Contains(line, `"`+key+`"`) {
				found++
				if strings.Count(line, `":`) != 1 {
					t.Errorf("line %q carries more than one field", line)
				}
			}
		}
		if found != 1 {
			t.Errorf("key %q appears on %d lines, want 1\n%s", key, found, got)
		}
	}
	if strings.Contains(got, "{\n  \"") == false {
		t.Errorf("not an indented object:\n%s", got)
	}
}

func TestBootRegistryGoldenMatchesWriter(t *testing.T) {
	dir := t.TempDir()
	e := registryEntry{ID: "v", Name: "V", Exec: "/data/UserData/platforms/v/entry.sh",
		Version: "0.3.0", Owner: "platform:v"}
	if err := writeRegistryEntry(dir, e); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(filepath.Join(dir, "v", "boot.json"))
	if err != nil {
		t.Fatal(err)
	}
	want, err := os.ReadFile("testdata/boot.json.golden")
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(want) {
		t.Errorf("writer output drifted from testdata/boot.json.golden.\ngot:\n%s\nwant:\n%s\n"+
			"tests/host/test_boot_target_manager_json.sh reads the golden with the "+
			"selector's own awk parser — update both together or not at all.", got, want)
	}
}

func TestBootRegistryListAndOwner(t *testing.T) {
	dir := t.TempDir()
	mustWrite := func(e registryEntry) {
		t.Helper()
		if err := writeRegistryEntry(dir, e); err != nil {
			t.Fatal(err)
		}
	}
	mustWrite(registryEntry{ID: "vee", Name: "V", Exec: "/x/entry.sh", Owner: "module:v-platform"})
	mustWrite(registryEntry{ID: "hand", Name: "Hand", Exec: "/y/entry.sh"}) // no owner

	entries, err := listRegistryEntries(dir)
	if err != nil {
		t.Fatal(err)
	}
	if len(entries) != 2 {
		t.Fatalf("listRegistryEntries = %d entries, want 2", len(entries))
	}

	// The payload id is "v-platform" but the target id is "vee": looking up by
	// id would miss it, which is how an uninstall leaves a row behind.
	got, ok := findEntryByOwner(entries, "module:v-platform")
	if !ok || got.ID != "vee" {
		t.Fatalf("findEntryByOwner = %+v, %v; want id vee", got, ok)
	}
	if _, ok := findEntryByOwner(entries, "module:absent"); ok {
		t.Error("findEntryByOwner matched an owner that is not present")
	}
}

func TestBootRegistryDefault(t *testing.T) {
	dir := t.TempDir()
	if got, _ := readBootDefault(dir); got != "" {
		t.Errorf("readBootDefault on empty registry = %q, want empty", got)
	}
	if err := writeBootDefault(dir, "vee"); err != nil {
		t.Fatal(err)
	}
	raw, _ := os.ReadFile(filepath.Join(dir, "default"))
	if string(raw) != "vee\n" {
		t.Errorf("default file = %q, want \"vee\\n\"", raw)
	}
	if got, _ := readBootDefault(dir); got != "vee" {
		t.Errorf("readBootDefault = %q", got)
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestBootRegistry -v`
Expected: FAIL — `undefined: writeRegistryEntry`.

- [ ] **Step 3: Write `boot_registry.go`**

```go
// schwung-manager/boot_registry.go
package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// registryEntry is one boot-selector target as the manager writes it.
//
// owner is the load-bearing field: the manager rewrites or deletes ONLY
// entries whose owner it recognises, which is what makes a hand-installed
// target (documented in docs/BOOT_TARGETS.md) and Schwung's own
// self-registered entry safe from reconcile.
type registryEntry struct {
	ID      string
	Name    string
	Exec    string
	Version string
	Owner   string // "module:<id>" | "platform:<id>" | "" for hand-installed
}

func ownerForModule(id string) string   { return "module:" + id }
func ownerForPlatform(id string) string { return "platform:" + id }

// writeRegistryEntry writes <dir>/<id>/boot.json FLAT: one field per line,
// string values only.
//
// This shape is a contract with two parsers that cannot handle anything else.
// bt_json_field (awk, src/host/boot_target_lib.sh) matches per line;
// bs_json_field (C, src/host/boot_select_core.c) scans a flat buffer. A nested
// object, or two fields sharing a line, is invisible to the selector — which
// presents as a target that vanishes from the picker, not as a parse error.
func writeRegistryEntry(dir string, e registryEntry) error {
	target := filepath.Join(dir, e.ID)
	if err := os.MkdirAll(target, 0o755); err != nil {
		return fmt.Errorf("creating %s: %w", target, err)
	}
	var b strings.Builder
	b.WriteString("{\n")
	fields := [][2]string{
		{"name", e.Name},
		{"exec", e.Exec},
		{"version", e.Version},
		{"owner", e.Owner},
	}
	var written int
	for _, f := range fields {
		if f[1] == "" {
			continue
		}
		if written > 0 {
			b.WriteString(",\n")
		}
		fmt.Fprintf(&b, "  %q: %q", f[0], f[1])
		written++
	}
	b.WriteString("\n}\n")

	path := filepath.Join(target, "boot.json")
	if err := os.WriteFile(path, []byte(b.String()), 0o644); err != nil {
		return fmt.Errorf("writing %s: %w", path, err)
	}
	// The selector execs the target as ableton, not root; the manager runs as
	// root, so ownership has to be handed back explicitly.
	chownToAbleton(target)
	return nil
}

// chownToAbleton is best-effort: on a dev host there is no such user, and the
// tests must not depend on one.
func chownToAbleton(path string) {
	_ = exec.Command("chown", "-R", "ableton:users", path).Run()
}

// readRegistryEntry reads one entry. Values are read with the same
// one-field-per-line assumption the writer guarantees.
func readRegistryEntry(dir, id string) (registryEntry, error) {
	raw, err := os.ReadFile(filepath.Join(dir, id, "boot.json"))
	if err != nil {
		return registryEntry{}, err
	}
	e := registryEntry{ID: id}
	for _, line := range strings.Split(string(raw), "\n") {
		key, val, ok := parseFlatJSONLine(line)
		if !ok {
			continue
		}
		switch key {
		case "name":
			e.Name = val
		case "exec":
			e.Exec = val
		case "version":
			e.Version = val
		case "owner":
			e.Owner = val
		}
	}
	return e, nil
}

// parseFlatJSONLine reads one `"key": "value"` line. Deliberately as dumb as
// the awk matcher it mirrors: no escapes, because validateBootTargetName
// refuses every value that would need one.
func parseFlatJSONLine(line string) (key, val string, ok bool) {
	line = strings.TrimSpace(strings.TrimSuffix(strings.TrimSpace(line), ","))
	if !strings.HasPrefix(line, `"`) {
		return "", "", false
	}
	rest := line[1:]
	i := strings.Index(rest, `"`)
	if i < 0 {
		return "", "", false
	}
	key = rest[:i]
	rest = strings.TrimSpace(rest[i+1:])
	if !strings.HasPrefix(rest, ":") {
		return "", "", false
	}
	rest = strings.TrimSpace(rest[1:])
	if !strings.HasPrefix(rest, `"`) {
		return "", "", false
	}
	rest = rest[1:]
	j := strings.Index(rest, `"`)
	if j < 0 {
		return "", "", false
	}
	return key, rest[:j], true
}

// listRegistryEntries returns every registered target, sorted by id, including
// entries with no owner (which the manager may show but never modify).
func listRegistryEntries(dir string) ([]registryEntry, error) {
	dirents, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil, nil
		}
		return nil, err
	}
	var out []registryEntry
	for _, d := range dirents {
		if !d.IsDir() || strings.HasPrefix(d.Name(), ".") {
			continue
		}
		e, err := readRegistryEntry(dir, d.Name())
		if err != nil {
			continue // no boot.json: not a target
		}
		out = append(out, e)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].ID < out[j].ID })
	return out, nil
}

// findEntryByOwner locates an entry by OWNER, never by id: boot_target.id is
// optional and may differ from the payload id, so <registry>/<payload id> is
// the wrong door and silently leaves the row behind on uninstall.
func findEntryByOwner(entries []registryEntry, owner string) (registryEntry, bool) {
	if owner == "" {
		return registryEntry{}, false
	}
	for _, e := range entries {
		if e.Owner == owner {
			return e, true
		}
	}
	return registryEntry{}, false
}

func removeRegistryEntry(dir, id string) error {
	return os.RemoveAll(filepath.Join(dir, id))
}

func readBootDefault(dir string) (string, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "default"))
	if err != nil {
		if os.IsNotExist(err) {
			return "", nil
		}
		return "", err
	}
	line, _, _ := strings.Cut(string(raw), "\n")
	return strings.TrimSpace(line), nil
}

func writeBootDefault(dir, id string) error {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	path := filepath.Join(dir, "default")
	if err := os.WriteFile(path, []byte(id+"\n"), 0o644); err != nil {
		return err
	}
	chownToAbleton(path)
	return nil
}
```

- [ ] **Step 4: Generate the golden from the writer, then read it by eye**

```bash
cd schwung-manager && mkdir -p testdata && cat > testdata/boot.json.golden <<'EOF'
{
  "name": "V",
  "exec": "/data/UserData/platforms/v/entry.sh",
  "version": "0.3.0",
  "owner": "platform:v"
}
EOF
```

- [ ] **Step 5: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestBootRegistry -v`
Expected: PASS. If the golden mismatches, fix the golden **only** after confirming the writer's output is still one field per line.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/boot_registry.go schwung-manager/boot_registry_test.go schwung-manager/testdata/boot.json.golden
git commit -m "feat: boot-target registry writer/reader, owner-keyed"
```

---

### Task 4: Cross-language pin — the selector's own parser reads the golden

**Goal:** The Go writer and the shell reader cannot drift apart silently.

**Why a separate test:** `TestBootRegistryGoldenMatchesWriter` proves the writer still produces the golden. It cannot prove the *selector* can read it — that is a different language, in a different repo directory, with a parser that truncates rather than errors.

**Files:**
- Create: `tests/host/test_boot_target_manager_json.sh`

**Acceptance Criteria:**
- [ ] The test sources `src/host/boot_target_lib.sh` and reads every field of `schwung-manager/testdata/boot.json.golden` with `bt_json_field`.
- [ ] It asserts `bt_is_registered` and `bt_exec_path` both work against a registry directory containing the golden.
- [ ] It fails if any field comes back empty or truncated.
- [ ] It follows the house rules for `tests/host/*.sh`: no apostrophes inside node/awk scripts, no `head -1` on a producer under `pipefail`.

**Verify:** `bash tests/host/test_boot_target_manager_json.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Read a neighbour for the house style**

Run: `sed -n 1,40p tests/host/test_boot_targets.sh` (or the nearest existing `test_boot_*.sh`) and copy its harness shape — the pass/fail helper and exit convention must match the suite, since CI runs `tests/host/*.sh` as a set.

- [ ] **Step 2: Write the test**

```sh
#!/bin/sh
# test_boot_target_manager_json.sh — the boot.json Schwung Manager writes must
# be readable by the SELECTOR's own parser, not merely by Go.
#
# bt_json_field is a per-line awk matcher: a nested object, or two fields on
# one line, is invisible to it. That failure does not look like a parse error
# on the device — it looks like a target that is registered and does not
# appear in the picker. This test is the only thing that connects the writer
# (schwung-manager/boot_registry.go) to the reader (src/host/boot_target_lib.sh).
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GOLDEN="$ROOT/schwung-manager/testdata/boot.json.golden"

fail() { echo "FAIL: $1"; exit 1; }

[ -f "$GOLDEN" ] || fail "golden missing: $GOLDEN"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/v"
cp "$GOLDEN" "$WORK/v/boot.json"

BOOT_TARGETS_DIR="$WORK"
export BOOT_TARGETS_DIR
. "$ROOT/src/host/boot_target_lib.sh"

bt_is_registered v || fail "bt_is_registered says the manager-written entry is not registered"

name=$(bt_json_field "$WORK/v/boot.json" name) || fail "bt_json_field could not read name"
[ "$name" = "V" ] || fail "name read back as '$name', want 'V'"

execp=$(bt_exec_path v) || fail "bt_exec_path returned nothing"
[ "$execp" = "/data/UserData/platforms/v/entry.sh" ] || fail "exec read back as '$execp'"

owner=$(bt_json_field "$WORK/v/boot.json" owner) || fail "bt_json_field could not read owner"
[ "$owner" = "platform:v" ] || fail "owner read back as '$owner'"

version=$(bt_json_field "$WORK/v/boot.json" version) || fail "bt_json_field could not read version"
[ "$version" = "0.3.0" ] || fail "version read back as '$version'"

# The resolver must pick it up as the default too.
echo v > "$WORK/default"
resolved=$(bt_resolve_default)
[ "$resolved" = "v" ] || fail "bt_resolve_default = '$resolved', want 'v'"

echo "PASS: manager-written boot.json is readable by the selector"
```

- [ ] **Step 3: Run it**

Run: `bash tests/host/test_boot_target_manager_json.sh`
Expected: `PASS: manager-written boot.json is readable by the selector`

- [ ] **Step 4: Prove it can fail**

Temporarily collapse the golden onto one line (`tr -d '\n' < golden > golden.tmp`), re-run — the test must fail on a field it cannot read. Restore the golden afterwards.

- [ ] **Step 5: Commit**

```bash
git add tests/host/test_boot_target_manager_json.sh
git commit -m "test: pin the manager's boot.json against the selector's own parser"
```

---

### Task 5: Reconcile

**Goal:** One function makes the registry agree with what is installed, touching only entries it owns.

**Files:**
- Create: `schwung-manager/boot_reconcile.go`
- Create: `schwung-manager/boot_reconcile_test.go`

**Acceptance Criteria:**
- [ ] An owned entry whose payload is gone is deleted.
- [ ] An owned entry whose payload no longer declares `boot_target` is deleted.
- [ ] An owned entry whose name, version or exec has changed is rewritten.
- [ ] A declared target with no entry is created.
- [ ] An entry with no `owner` is never touched, in any of the above situations.
- [ ] The `schwung` entry is never touched, even if something claims to own it.
- [ ] Deleting an entry that `default` names rewrites `default` to `schwung`; deleting one it does not name leaves `default` alone.
- [ ] Reconcile never writes `default` in any other direction.
- [ ] Registration past the 16-row picker cap is refused, and the refusal is returned rather than logged and forgotten.

**Verify:** `cd schwung-manager && go test ./... -run TestReconcile -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/boot_reconcile_test.go
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

// plantModule writes a module payload with an optional boot_target block.
func plantModule(t *testing.T, base, subdir, id, name string, withTarget bool) {
	t.Helper()
	dir := filepath.Join(base, "modules", subdir, id)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	block := ""
	if withTarget {
		block = fmt.Sprintf(`,"boot_target":{"name":%q,"exec":"entry.sh"}`, name)
		if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	manifest := fmt.Sprintf(`{"id":%q,"name":%q,"version":"1.0.0"%s}`, id, name, block)
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(manifest), 0o644); err != nil {
		t.Fatal(err)
	}
}

func newReconcileApp(t *testing.T) (*App, string, string) {
	t.Helper()
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	reg := filepath.Join(root, "boot-targets")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(reg, 0o755); err != nil {
		t.Fatal(err)
	}
	t.Setenv("BOOT_TARGETS_DIR", reg)
	return &App{basePath: base, logger: testLogger()}, base, reg
}

func TestReconcileCreatesDeclaredTarget(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", true)

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	e, err := readRegistryEntry(reg, "vplat")
	if err != nil {
		t.Fatalf("entry not created: %v", err)
	}
	if e.Name != "V" || e.Owner != "module:vplat" {
		t.Errorf("entry = %+v", e)
	}
	wantExec := filepath.Join(base, "modules", "tools", "vplat", "entry.sh")
	if e.Exec != wantExec {
		t.Errorf("exec = %q, want %q", e.Exec, wantExec)
	}
}

func TestReconcileDeletesOrphanAndHealsDefault(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := writeRegistryEntry(reg, registryEntry{ID: "gone", Name: "Gone",
		Exec: "/nowhere/entry.sh", Owner: "module:gone"}); err != nil {
		t.Fatal(err)
	}
	if err := writeBootDefault(reg, "gone"); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "gone")); !os.IsNotExist(err) {
		t.Error("orphaned entry was not deleted")
	}
	got, _ := readBootDefault(reg)
	if got != "schwung" {
		t.Errorf("default = %q, want schwung", got)
	}
}

func TestReconcileDeletesWhenBlockRetired(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V", false) // installed, no boot_target
	if err := writeRegistryEntry(reg, registryEntry{ID: "vplat", Name: "V",
		Exec: filepath.Join(base, "modules", "tools", "vplat", "entry.sh"),
		Owner: "module:vplat"}); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "vplat")); !os.IsNotExist(err) {
		t.Error("entry survived its payload retiring the boot_target block")
	}
}

func TestReconcileRewritesChangedEntry(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	plantModule(t, base, "tools", "vplat", "V2", true)
	if err := writeRegistryEntry(reg, registryEntry{ID: "vplat", Name: "V1",
		Exec: "/stale/path/entry.sh", Version: "0.0.1", Owner: "module:vplat"}); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	e, _ := readRegistryEntry(reg, "vplat")
	if e.Name != "V2" {
		t.Errorf("name = %q, want V2", e.Name)
	}
	if e.Exec != filepath.Join(base, "modules", "tools", "vplat", "entry.sh") {
		t.Errorf("stale exec not rewritten: %q", e.Exec)
	}
}

// The two entries reconcile must never touch, in the situation where every
// other rule says delete: no payload on disk at all.
func TestReconcileNeverTouchesUnownedOrSchwung(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := writeRegistryEntry(reg, registryEntry{ID: "hand", Name: "Hand",
		Exec: "/opt/hand/entry.sh"}); err != nil { // no owner
		t.Fatal(err)
	}
	if err := writeRegistryEntry(reg, registryEntry{ID: "schwung", Name: "Schwung",
		Exec: "/data/UserData/schwung/schwung-entry.sh", Owner: "module:schwung"}); err != nil {
		t.Fatal(err)
	}
	if err := writeBootDefault(reg, "hand"); err != nil {
		t.Fatal(err)
	}

	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "hand", "boot.json")); err != nil {
		t.Error("hand-installed entry was deleted")
	}
	if _, err := os.Stat(filepath.Join(reg, "schwung", "boot.json")); err != nil {
		t.Error("schwung entry was deleted")
	}
	if got, _ := readBootDefault(reg); got != "hand" {
		t.Errorf("default = %q, want hand (reconcile must not repoint a live default)", got)
	}
}

func TestReconcileRefusesPastRowCap(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	for i := 0; i < bootPickerTargetCap+2; i++ {
		plantModule(t, base, "tools", fmt.Sprintf("mod%02d", i), fmt.Sprintf("M%02d", i), true)
	}
	err := app.reconcileBootTargets()
	if err == nil {
		t.Fatal("want an error naming the row cap, got nil")
	}
	entries, _ := listRegistryEntries(reg)
	if len(entries) > bootPickerTargetCap {
		t.Errorf("registered %d targets, cap is %d", len(entries), bootPickerTargetCap)
	}
}
```

Add a `testLogger()` helper if the package has none:

```go
// in boot_reconcile_test.go
func testLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestReconcile -v`
Expected: FAIL — `undefined: (*App).reconcileBootTargets`.

- [ ] **Step 3: Write `boot_reconcile.go`**

```go
// schwung-manager/boot_reconcile.go
package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
)

// bootPickerTargetCap is how many registered targets the picker can show.
//
// BS_MAX_ROWS is 16 (src/boot-select.c) and covers Stock and Schwung too, so
// 14 is what is left. bs_row_insert_sorted DROPS the overflow SILENTLY, in id
// sort order — the row that disappears has nothing to do with what was
// installed last, and nothing anywhere says so. The manager is the first
// thing that can reach this without a human present, so it refuses instead.
const bootPickerTargetCap = 14

// desiredTarget is a boot target the installed payloads say should exist.
type desiredTarget struct {
	entry registryEntry
}

// desiredBootTargets walks every installed module and platform and returns the
// targets they declare, keyed by target id.
func (app *App) desiredBootTargets() (map[string]desiredTarget, error) {
	out := map[string]desiredTarget{}

	add := func(payloadDir, payloadID, manifestName, owner string) error {
		manifest := filepath.Join(payloadDir, manifestName)
		if _, err := os.Stat(manifest); err != nil {
			return nil
		}
		bt, err := parseBootTarget(manifest, payloadID, payloadDir)
		if err != nil {
			// A refused boot target is not a broken payload: the module still
			// works, it just gets no picker row. Log and carry on.
			app.logger.Warn("boot target refused", "owner", owner, "err", err)
			return nil
		}
		if bt == nil {
			return nil
		}
		if prev, clash := out[bt.ID]; clash && prev.entry.Owner != owner {
			return fmt.Errorf("boot target id %q claimed by both %s and %s",
				bt.ID, prev.entry.Owner, owner)
		}
		out[bt.ID] = desiredTarget{entry: registryEntry{
			ID:      bt.ID,
			Name:    bt.Name,
			Exec:    bt.ExecAbs,
			Version: payloadVersion(manifest),
			Owner:   owner,
		}}
		return nil
	}

	for id, m := range discoverInstalledModules(app.basePath) {
		dir := app.findModuleDir(id)
		if dir == "" {
			continue
		}
		_ = m
		if err := add(dir, id, "module.json", ownerForModule(id)); err != nil {
			return nil, err
		}
	}
	for id, dir := range discoverInstalledPlatforms(app.basePath) {
		if err := add(dir, id, "platform.json", ownerForPlatform(id)); err != nil {
			return nil, err
		}
	}
	return out, nil
}

// payloadVersion reads the manifest's version field, empty if absent.
func payloadVersion(manifestPath string) string {
	raw, err := os.ReadFile(manifestPath)
	if err != nil {
		return ""
	}
	var doc struct {
		Version string `json:"version"`
	}
	if err := jsonUnmarshal(raw, &doc); err != nil {
		return ""
	}
	return doc.Version
}

// reconcileBootTargets makes the registry agree with what is installed.
//
// It considers ONLY entries carrying an owner it recognises. A hand-installed
// target has no owner and is never touched; the "schwung" entry is excluded
// outright because the selector rewrites it on every boot.
//
// It writes `default` in ONE direction only: healing it to "schwung" when the
// target it named has just been deleted. A default is otherwise an explicit
// user choice, never a side effect.
func (app *App) reconcileBootTargets() error {
	reg := bootTargetsDir()
	if err := os.MkdirAll(reg, 0o755); err != nil {
		return fmt.Errorf("creating registry dir: %w", err)
	}

	desired, desiredErr := app.desiredBootTargets()
	if desired == nil {
		return desiredErr
	}

	existing, err := listRegistryEntries(reg)
	if err != nil {
		return fmt.Errorf("listing registry: %w", err)
	}

	currentDefault, _ := readBootDefault(reg)
	healDefault := false

	// 1. Delete owned entries whose payload is gone or has retired its block.
	for _, e := range existing {
		if e.ID == "schwung" || e.Owner == "" {
			continue // never ours to touch
		}
		if _, want := desired[e.ID]; want {
			continue
		}
		app.logger.Info("deregistering boot target", "id", e.ID, "owner", e.Owner)
		if err := removeRegistryEntry(reg, e.ID); err != nil {
			return fmt.Errorf("removing %s: %w", e.ID, err)
		}
		if currentDefault == e.ID {
			healDefault = true
		}
	}

	// 2. Create or rewrite. Sorted so a capped run is deterministic rather
	//    than map-order roulette.
	ids := make([]string, 0, len(desired))
	for id := range desired {
		ids = append(ids, id)
	}
	sort.Strings(ids)

	registered := 0
	for _, e := range existing {
		if e.ID != "schwung" && e.Owner != "" {
			if _, want := desired[e.ID]; !want {
				continue // just deleted
			}
		}
		if e.ID != "schwung" {
			registered++
		}
	}

	var capErr error
	for _, id := range ids {
		d := desired[id]
		cur, err := readRegistryEntry(reg, id)
		switch {
		case err == nil && cur.Owner != d.entry.Owner && cur.Owner != "":
			capErr = errors.Join(capErr, fmt.Errorf(
				"boot target %q is owned by %s; %s cannot claim it",
				id, cur.Owner, d.entry.Owner))
			continue
		case err == nil && cur.Owner == "":
			capErr = errors.Join(capErr, fmt.Errorf(
				"boot target %q was installed by hand; %s cannot claim it",
				id, d.entry.Owner))
			continue
		case err == nil:
			if cur == d.entry {
				continue // already correct
			}
		default:
			if registered >= bootPickerTargetCap {
				capErr = errors.Join(capErr, fmt.Errorf(
					"boot target %q not registered: the picker holds %d targets and is full",
					id, bootPickerTargetCap))
				continue
			}
			registered++
		}
		if err := writeRegistryEntry(reg, d.entry); err != nil {
			return fmt.Errorf("writing %s: %w", id, err)
		}
		app.logger.Info("registered boot target", "id", id, "owner", d.entry.Owner)
	}

	if healDefault {
		app.logger.Info("boot default named a removed target; healing to schwung")
		if err := writeBootDefault(reg, "schwung"); err != nil {
			return fmt.Errorf("healing default: %w", err)
		}
	}
	return errors.Join(desiredErr, capErr)
}
```

Add the small `jsonUnmarshal` alias next to it if `encoding/json` is not already imported in this file — or simply import `encoding/json` and call `json.Unmarshal` directly. Prefer the direct call:

```go
import "encoding/json"
// ... and in payloadVersion:
	if err := json.Unmarshal(raw, &doc); err != nil {
```

- [ ] **Step 4: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestReconcile -v`
Expected: PASS, all six.

- [ ] **Step 5: Prove the never-touch test can fail**

Temporarily delete the `e.Owner == ""` guard in the delete loop, re-run — `TestReconcileNeverTouchesUnownedOrSchwung` must fail. Restore.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/boot_reconcile.go schwung-manager/boot_reconcile_test.go
git commit -m "feat: reconcile boot targets against installed payloads, owner-scoped"
```

---

### Task 6: Platforms — catalog entry, install, uninstall, update check

**Goal:** A payload with no module in it can be installed, updated and removed by the manager.

**Files:**
- Create: `schwung-manager/platforms.go`
- Create: `schwung-manager/platforms_test.go`
- Modify: `schwung-manager/main.go:78-82` (`Catalog` struct), `main.go:1229-1290` (factor the download half out of `installModuleWithDeps`)

**Acceptance Criteria:**
- [ ] `Catalog` carries `Platforms []CatalogPlatform`; a catalog with no `platforms` key parses to an empty slice, not an error.
- [ ] `resolveDownloadURL` is shared by module and platform installs — the release.json fetch, the multi-module `forModule` shape, and the latest-release fallback exist once.
- [ ] A platform installs to `platformsRoot()/<id>/` and its `platform.json` is verified present after extraction; a tarball whose top-level directory is not `<id>/` fails the install with a message naming that.
- [ ] `discoverInstalledPlatforms` returns id → directory for everything under the platforms root holding a `platform.json`.
- [ ] Platform uninstall removes the payload and deregisters its owned target by owner.
- [ ] Update detection compares the installed `platform.json` version against `release.json` exactly as modules do.
- [ ] User state (`config.json`, `secrets/`) is snapshotted and restored around a platform upgrade, reusing `snapshotModuleUserState`.

**Verify:** `cd schwung-manager && go test ./... -run TestPlatform -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/platforms_test.go
package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPlatformCatalogParsesWithAndWithout(t *testing.T) {
	var withOut Catalog
	if err := json.Unmarshal([]byte(`{"catalog_version":2,"modules":[]}`), &withOut); err != nil {
		t.Fatal(err)
	}
	if len(withOut.Platforms) != 0 {
		t.Errorf("absent platforms key parsed to %d entries", len(withOut.Platforms))
	}

	var with Catalog
	raw := `{"catalog_version":2,"modules":[],"platforms":[
	  {"id":"v","name":"V","github_repo":"who/v","default_branch":"main",
	   "asset_name":"v-platform.tar.gz","min_host_version":"1.3.2"}]}`
	if err := json.Unmarshal([]byte(raw), &with); err != nil {
		t.Fatal(err)
	}
	if len(with.Platforms) != 1 || with.Platforms[0].ID != "v" {
		t.Fatalf("platforms = %+v", with.Platforms)
	}
}

func TestPlatformDiscovery(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	dir := filepath.Join(root, "platforms", "v")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "platform.json"),
		[]byte(`{"id":"v","name":"V","version":"0.3.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	// A directory with no manifest is not a platform.
	if err := os.MkdirAll(filepath.Join(root, "platforms", "junk"), 0o755); err != nil {
		t.Fatal(err)
	}

	got := discoverInstalledPlatforms(base)
	if len(got) != 1 || got["v"] != dir {
		t.Fatalf("discoverInstalledPlatforms = %+v", got)
	}
}

// A tarball whose top-level directory is not <id>/ otherwise strews itself
// across the install root and then fails to register for a reason that reads
// as unrelated to the real problem.
func TestPlatformExtractionVerified(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	if err := os.MkdirAll(filepath.Join(root, "platforms", "wrongname"), 0o755); err != nil {
		t.Fatal(err)
	}
	err := verifyPlatformExtraction(base, "v")
	if err == nil {
		t.Fatal("want an error naming the missing platform.json, got nil")
	}
	if !strings.Contains(err.Error(), "platform.json") {
		t.Errorf("error %q does not name platform.json", err)
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestPlatform -v`
Expected: FAIL — `undefined: discoverInstalledPlatforms`, `Catalog has no field Platforms`.

- [ ] **Step 3: Add the catalog type**

In `main.go`, extend `Catalog` (line 78) and add the platform entry type next to `CatalogModule`:

```go
type Catalog struct {
	CatalogVersion int               `json:"catalog_version"`
	Host           CatalogHost       `json:"host"`
	Modules        []CatalogModule   `json:"modules"`
	Platforms      []CatalogPlatform `json:"platforms,omitempty"`
}

// CatalogPlatform is a boot target with no module in it: an alternative
// platform that wants the manager's install plumbing. It has no
// component_type because it is never loaded by the Schwung host — it replaces
// it at boot.
type CatalogPlatform struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Description   string `json:"description"`
	Author        string `json:"author"`
	GithubRepo    string `json:"github_repo"`
	DefaultBranch string `json:"default_branch"`
	AssetName     string `json:"asset_name"`
	MinHostVer    string `json:"min_host_version"`
	Requires      string `json:"requires,omitempty"`
}
```

- [ ] **Step 4: Factor the download half out of `installModuleWithDeps`**

Cut lines 1229-1263 of `main.go` (the release.json fetch through the fallback URL) into a shared helper, and call it from `installModuleWithDeps` in place of the removed block:

```go
// resolveDownloadURL fetches release.json for a payload and returns the URL to
// download plus the version that URL represents. Falls back to the repo's
// latest-release asset when release.json is missing or does not carry this id.
// Shared by module and platform installs: one release.json contract, one
// multi-module shape, one fallback.
func (app *App) resolveDownloadURL(repo, branch, id, assetName string) (url, version string) {
	client := &http.Client{Timeout: 120 * time.Second}
	releaseURL := fmt.Sprintf("https://raw.githubusercontent.com/%s/%s/release.json", repo, branch)
	app.logger.Info("fetching release.json", "url", releaseURL)

	if resp, err := client.Get(releaseURL); err == nil {
		defer resp.Body.Close()
		if resp.StatusCode == http.StatusOK {
			var rel ReleaseJSON
			if err := json.NewDecoder(resp.Body).Decode(&rel); err == nil {
				if r, ok := rel.forModule(id); ok {
					url, version = r.DownloadURL, r.Version
				} else {
					app.logger.Warn("id missing from multi-module release.json; using fallback URL", "id", id)
				}
			} else {
				app.logger.Warn("decoding release.json", "id", id, "err", err)
			}
		} else {
			app.logger.Warn("release.json not found, using fallback URL", "status", resp.StatusCode)
		}
	} else {
		app.logger.Warn("fetching release.json", "id", id, "err", err)
	}
	if url == "" {
		url = fmt.Sprintf("https://github.com/%s/releases/latest/download/%s", repo, assetName)
	}
	return url, version
}

// downloadToTemp saves a URL to a temp file under basePath (never /tmp — the
// device's root FS is ~463MB and usually full). The caller removes it.
func (app *App) downloadToTemp(url, name string) (string, error) {
	client := &http.Client{Timeout: 120 * time.Second}
	resp, err := client.Get(url)
	if err != nil {
		return "", fmt.Errorf("downloading tarball: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("download returned %d", resp.StatusCode)
	}
	tmpPath := filepath.Join(app.basePath, name)
	f, err := os.Create(tmpPath)
	if err != nil {
		return "", fmt.Errorf("creating temp file: %w", err)
	}
	if _, err := io.Copy(f, resp.Body); err != nil {
		f.Close()
		os.Remove(tmpPath)
		return "", fmt.Errorf("saving tarball: %w", err)
	}
	f.Close()
	return tmpPath, nil
}
```

Then in `installModuleWithDeps`, the block becomes:

```go
	downloadURL, releaseVersion := app.resolveDownloadURL(
		mod.GithubRepo, mod.DefaultBranch, mod.ID, mod.AssetName)
	app.logger.Info("downloading module", "id", mod.ID, "url", downloadURL)
	tmpPath, err := app.downloadToTemp(downloadURL, ".tmp-module-download.tar.gz")
	if err != nil {
		return err
	}
	defer os.Remove(tmpPath)
```

Run `cd schwung-manager && go test ./...` here — the existing module tests must stay green across this refactor before anything is built on top of it.

- [ ] **Step 5: Write `platforms.go`**

```go
// schwung-manager/platforms.go
package main

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// InstalledPlatform is a platform payload present on disk.
type InstalledPlatform struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Version string `json:"version"`
	Author  string `json:"author"`
	Dir     string `json:"-"`
}

// discoverInstalledPlatforms returns id -> payload directory for everything
// under the platforms root carrying a platform.json.
func discoverInstalledPlatforms(basePath string) map[string]string {
	out := map[string]string{}
	root := platformsRoot(basePath)
	entries, err := os.ReadDir(root)
	if err != nil {
		return out
	}
	for _, e := range entries {
		if !e.IsDir() {
			continue
		}
		dir := filepath.Join(root, e.Name())
		if _, err := os.Stat(filepath.Join(dir, "platform.json")); err != nil {
			continue
		}
		out[e.Name()] = dir
	}
	return out
}

func readInstalledPlatform(dir string) (InstalledPlatform, error) {
	raw, err := os.ReadFile(filepath.Join(dir, "platform.json"))
	if err != nil {
		return InstalledPlatform{}, err
	}
	var p InstalledPlatform
	if err := json.Unmarshal(raw, &p); err != nil {
		return InstalledPlatform{}, err
	}
	p.Dir = dir
	return p, nil
}

// verifyPlatformExtraction fails when the tarball did not carry a top-level
// <id>/ directory. Without this the payload is strewn across the platforms
// root and the failure surfaces later as an unrelated-looking registration
// error.
func verifyPlatformExtraction(basePath, id string) error {
	manifest := filepath.Join(platformsRoot(basePath), id, "platform.json")
	if _, err := os.Stat(manifest); err != nil {
		return fmt.Errorf("%s: the tarball must contain a top-level %s/ directory "+
			"with platform.json inside it", manifest, id)
	}
	return nil
}

// installPlatform downloads and extracts a platform, then registers whatever
// boot target it declares.
func (app *App) installPlatform(p *CatalogPlatform) error {
	if err := app.checkHostCompat(p.MinHostVer); err != nil {
		return err
	}
	url, releaseVersion := app.resolveDownloadURL(p.GithubRepo, p.DefaultBranch, p.ID, p.AssetName)
	app.logger.Info("downloading platform", "id", p.ID, "url", url)
	tmpPath, err := app.downloadToTemp(url, ".tmp-platform-download.tar.gz")
	if err != nil {
		return err
	}
	defer os.Remove(tmpPath)

	root := platformsRoot(app.basePath)
	if err := os.MkdirAll(root, 0o755); err != nil {
		return fmt.Errorf("creating platforms dir: %w", err)
	}
	payloadDir := filepath.Join(root, p.ID)

	// Same user-state contract as modules: the tarball ships immutable files,
	// config.json and secrets/ belong to the user.
	preserved, snapErr := snapshotModuleUserState(payloadDir)
	if snapErr != nil {
		app.logger.Warn("failed to snapshot platform user state", "id", p.ID, "err", snapErr)
	}

	app.logger.Info("extracting platform", "id", p.ID, "dest", root)
	if out, err := exec.Command("tar", "-xzf", tmpPath, "-C", root).CombinedOutput(); err != nil {
		return fmt.Errorf("extracting tarball: %w\noutput: %s", err, out)
	}
	if err := verifyPlatformExtraction(app.basePath, p.ID); err != nil {
		return err
	}
	if preserved != nil {
		if err := restoreModuleUserState(payloadDir, preserved); err != nil {
			app.logger.Warn("failed to restore platform user state", "id", p.ID, "err", err)
		}
	}
	if err := pinInstalledPlatformVersion(payloadDir, releaseVersion, app.logger); err != nil {
		app.logger.Warn("failed to pin platform.json version", "id", p.ID, "err", err)
	}
	chownToAbleton(payloadDir)

	return app.reconcileBootTargets()
}

// pinInstalledPlatformVersion mirrors pinInstalledModuleVersion: without it a
// tarball whose manifest lags release.json traps the user in an endless
// "update available" loop.
func pinInstalledPlatformVersion(dir, expectedVersion string, logger *slog.Logger) error {
	if expectedVersion == "" {
		return nil
	}
	path := filepath.Join(dir, "platform.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	var parsed map[string]any
	if err := json.Unmarshal(raw, &parsed); err != nil {
		logger.Warn("platform.json parse failed during version pin", "path", path, "err", err)
		return nil
	}
	if v, _ := parsed["version"].(string); v == expectedVersion {
		return nil
	}
	parsed["version"] = expectedVersion
	out, err := json.MarshalIndent(parsed, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(out, '\n'), 0o644)
}

// uninstallPlatform removes the payload and takes its picker row with it.
func (app *App) uninstallPlatform(id string) error {
	dirs := discoverInstalledPlatforms(app.basePath)
	dir, ok := dirs[id]
	if !ok {
		return fmt.Errorf("platform %q not found on disk", id)
	}
	app.logger.Info("uninstalling platform", "id", id, "path", dir)
	if err := os.RemoveAll(dir); err != nil {
		return err
	}
	return app.reconcileBootTargets()
}
```

Add `"log/slog"` to the imports.

- [ ] **Step 6: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestPlatform -v && go test ./...`
Expected: PASS, and the whole suite still green after the download refactor.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/platforms.go schwung-manager/platforms_test.go schwung-manager/main.go
git commit -m "feat: platform payloads — catalog entry, install, uninstall, shared download path"
```

---

### Task 7: Wire registration into install, uninstall and startup

**Goal:** Installing anything registers its target; uninstalling anything takes it away; starting the manager makes the registry true.

**Files:**
- Modify: `schwung-manager/main.go` — end of `installModuleWithDeps` (~line 1340), `uninstallModule` (~line 1381), `main()` startup (~line 3618), `handleCustomInstall` (~line 1684) and the tarball-upload path (~line 1782)
- Create: `schwung-manager/boot_wiring_test.go`

**Acceptance Criteria:**
- [ ] A module install that declares a boot target leaves a registry entry owned by it.
- [ ] `uninstallModule` removes the entry **found by owner**, including when the target id differs from the module id.
- [ ] Uninstalling a module whose target was the boot default heals `default` to `schwung`.
- [ ] Reconcile runs once at manager start, before the HTTP listener accepts.
- [ ] A registration failure does **not** fail the install: the module is installed, the reason is logged.
- [ ] The custom-URL and tarball-upload install paths register too (they extract the same payloads).

**Verify:** `cd schwung-manager && go test ./... -run TestBootWiring -v` → PASS

**Steps:**

- [ ] **Step 1: Write the failing test**

```go
// schwung-manager/boot_wiring_test.go
package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The target id differs from the module id on purpose: looking the entry up by
// module id is the mistake this test exists to catch.
func TestBootWiringUninstallDeregistersByOwner(t *testing.T) {
	app, base, reg := newReconcileApp(t)
	dir := filepath.Join(base, "modules", "tools", "vmod")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "entry.sh"), []byte("#!/bin/sh\n"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"), []byte(
		`{"id":"vmod","name":"V","version":"1.0.0","boot_target":{"id":"vee","name":"V","exec":"entry.sh"}}`),
		0o644); err != nil {
		t.Fatal(err)
	}
	if err := app.reconcileBootTargets(); err != nil {
		t.Fatal(err)
	}
	if _, err := readRegistryEntry(reg, "vee"); err != nil {
		t.Fatalf("target not registered under its declared id: %v", err)
	}
	if err := writeBootDefault(reg, "vee"); err != nil {
		t.Fatal(err)
	}

	if err := app.uninstallModule("vmod"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(reg, "vee")); !os.IsNotExist(err) {
		t.Error("uninstall left the picker row behind")
	}
	if got, _ := readBootDefault(reg); got != "schwung" {
		t.Errorf("default = %q, want schwung", got)
	}
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestBootWiring -v`
Expected: FAIL — the entry survives, `default` still reads `vee`.

- [ ] **Step 3: Call reconcile from the install path**

At the end of `installModuleWithDeps`, immediately before `return nil`:

```go
	// Register whatever boot target the module declares. A refusal is NOT an
	// install failure: the module works, it just gets no picker row, and the
	// reason is in the log.
	if err := app.reconcileBootTargets(); err != nil {
		app.logger.Warn("boot target registration", "id", mod.ID, "err", err)
	}

	app.logger.Info("module installed", "id", mod.ID, "path", categoryDir)
	return nil
```

- [ ] **Step 4: Call reconcile from the uninstall path**

In `uninstallModule`, after the `os.RemoveAll(modDir)`:

```go
	app.logger.Info("uninstalling module", "id", id, "path", modDir)
	if err := os.RemoveAll(modDir); err != nil {
		return err
	}
	// Takes the picker row with it, found by OWNER — boot_target.id is
	// optional and may differ from the module id.
	return app.reconcileBootTargets()
```

- [ ] **Step 5: Call reconcile at startup and from the two side-load paths**

In `main()`, after the `App` is constructed and before the listener starts:

```go
	if err := app.reconcileBootTargets(); err != nil {
		app.logger.Warn("boot target reconcile at startup", "err", err)
	}
```

In `handleCustomInstall` and the tarball-upload handler, after the extract-and-move succeeds, add the same `app.reconcileBootTargets()` warn-on-error call.

- [ ] **Step 6: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestBootWiring -v && go test ./... && go build ./...`
Expected: PASS, PASS, clean build.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/main.go schwung-manager/boot_wiring_test.go
git commit -m "feat: register/deregister boot targets on install, uninstall and startup"
```

---

### Task 8: The Boot page

**Goal:** A user can see every picker row and choose the default from the web UI.

**Files:**
- Create: `schwung-manager/templates/boot.html`
- Modify: `schwung-manager/main.go` (routes ~line 3688, handlers), `schwung-manager/templates/base.html` (nav entry)
- Create: `schwung-manager/boot_page_test.go`

**Acceptance Criteria:**
- [ ] `GET /boot` lists Stock Move, Schwung, and every registered target, sorted as `bs_build_rows` sorts them (Schwung first, then ascending by id, Stock last).
- [ ] Each row shows name, id, source (`module: <id>` / `platform: <id>` / `installed manually`), and a warning when its `exec` does not resolve.
- [ ] The page shows the registered count against `bootPickerTargetCap` and warns when the cap is reached, naming targets the picker would drop.
- [ ] `POST /boot/default` writes the chosen id, and **refuses an id that is neither `stock` nor registered** — a dangling default is tolerated by the selector but must not be created here.
- [ ] The handler is CSRF-protected the same way every other POST handler is.

**Verify:** `cd schwung-manager && go test ./... -run TestBootPage -v` → PASS

**Steps:**

- [ ] **Step 1: Read the pattern to follow**

Run: `sed -n 1,60p schwung-manager/templates/system.html` and the `handleSystem` handler — copy their structure (template data map, CSRF field, flash redirect helper). Do not invent a new page idiom.

- [ ] **Step 2: Write the failing test**

```go
// schwung-manager/boot_page_test.go
package main

import "testing"

func TestBootPageRowOrdering(t *testing.T) {
	rows := bootPageRows([]registryEntry{
		{ID: "zeta", Name: "Zeta", Owner: "platform:zeta"},
		{ID: "alpha", Name: "Alpha", Owner: "module:alpha"},
		{ID: "schwung", Name: "Schwung"},
	}, "alpha")

	var got []string
	for _, r := range rows {
		got = append(got, r.ID)
	}
	want := []string{"schwung", "alpha", "zeta", "stock"}
	if len(got) != len(want) {
		t.Fatalf("rows = %v, want %v", got, want)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("rows = %v, want %v", got, want)
		}
	}
	for _, r := range rows {
		if r.ID == "alpha" && !r.IsDefault {
			t.Error("current default not marked")
		}
	}
}

func TestBootPageSourceLabels(t *testing.T) {
	rows := bootPageRows([]registryEntry{
		{ID: "a", Name: "A", Owner: "module:amod"},
		{ID: "b", Name: "B", Owner: "platform:bplat"},
		{ID: "c", Name: "C"},
	}, "")
	want := map[string]string{
		"a": "module: amod", "b": "platform: bplat", "c": "installed manually",
	}
	for _, r := range rows {
		if w, ok := want[r.ID]; ok && r.Source != w {
			t.Errorf("row %s source = %q, want %q", r.ID, r.Source, w)
		}
	}
}

func TestBootDefaultRefusesUnregistered(t *testing.T) {
	app, _, reg := newReconcileApp(t)
	if err := app.setBootDefault("ghost"); err == nil {
		t.Fatal("want a refusal for an unregistered id")
	}
	if got, _ := readBootDefault(reg); got != "" {
		t.Errorf("default was written anyway: %q", got)
	}
	if err := app.setBootDefault("stock"); err != nil {
		t.Fatalf("stock is always valid: %v", err)
	}
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestBootPage -v`
Expected: FAIL — `undefined: bootPageRows`.

- [ ] **Step 4: Implement the page model and handlers**

Add to `boot_registry.go` (model) and `main.go` (handlers):

```go
// bootPageRow is one row of the Boot page, in picker order.
type bootPageRow struct {
	ID        string
	Name      string
	Source    string
	Exec      string
	IsDefault bool
	Missing   bool // exec does not resolve
	Removable bool // owned by the manager, so uninstalling the payload removes it
}

// bootPageRows renders the registry in the order bs_build_rows produces:
// Schwung first, then targets ascending by id, Stock last.
func bootPageRows(entries []registryEntry, currentDefault string) []bootPageRow {
	var schwung []bootPageRow
	var others []bootPageRow
	for _, e := range entries {
		row := bootPageRow{
			ID: e.ID, Name: e.Name, Exec: e.Exec,
			Source:    bootSourceLabel(e.Owner),
			IsDefault: e.ID == currentDefault,
			Removable: e.Owner != "",
		}
		if e.Exec != "" {
			if _, err := os.Stat(e.Exec); err != nil {
				row.Missing = true
			}
		}
		if e.ID == "schwung" {
			schwung = append(schwung, row)
			continue
		}
		others = append(others, row)
	}
	sort.Slice(others, func(i, j int) bool { return others[i].ID < others[j].ID })
	rows := append(schwung, others...)
	return append(rows, bootPageRow{
		ID: "stock", Name: "Stock Move", Source: "built in",
		IsDefault: currentDefault == "stock",
	})
}

func bootSourceLabel(owner string) string {
	switch {
	case owner == "":
		return "installed manually"
	case strings.HasPrefix(owner, "module:"):
		return "module: " + strings.TrimPrefix(owner, "module:")
	case strings.HasPrefix(owner, "platform:"):
		return "platform: " + strings.TrimPrefix(owner, "platform:")
	default:
		return owner
	}
}

// setBootDefault writes the boot default, refusing anything that is neither
// "stock" nor registered. The selector tolerates a dangling default (it falls
// back to schwung, then stock) but there is no reason to create one here.
func (app *App) setBootDefault(id string) error {
	reg := bootTargetsDir()
	if id != "stock" {
		if _, err := readRegistryEntry(reg, id); err != nil {
			return fmt.Errorf("%q is not a registered boot target", id)
		}
	}
	return writeBootDefault(reg, id)
}
```

Handlers and routes in `main.go`:

```go
	mux.HandleFunc("GET /boot", app.handleBoot)
	mux.HandleFunc("POST /boot/default", app.handleBootSetDefault)
```

```go
func (app *App) handleBoot(w http.ResponseWriter, r *http.Request) {
	reg := bootTargetsDir()
	entries, err := listRegistryEntries(reg)
	if err != nil {
		app.logger.Error("listing boot targets", "err", err)
	}
	current, _ := readBootDefault(reg)
	rows := bootPageRows(entries, current)

	var registered int
	for _, e := range entries {
		if e.ID != "schwung" {
			registered++
		}
	}
	app.render(w, r, "boot.html", map[string]any{
		"Title":      "Boot",
		"Rows":       rows,
		"Registered": registered,
		"Cap":        bootPickerTargetCap,
		"OverCap":    registered > bootPickerTargetCap,
	})
}

func (app *App) handleBootSetDefault(w http.ResponseWriter, r *http.Request) {
	id := r.FormValue("id")
	if err := app.setBootDefault(id); err != nil {
		http.Redirect(w, r, "/boot?flash="+url.QueryEscape("Could not set default: "+err.Error()),
			http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/boot?flash="+url.QueryEscape("Boot default is now "+id),
		http.StatusSeeOther)
}
```

- [ ] **Step 5: Write `templates/boot.html`**

Follow `system.html`'s structure exactly (same `{{define}}`/`{{template "base"}}` shape, same CSRF hidden input). Body:

```html
{{define "content"}}
<h1>Boot</h1>
<p>What this Move boots into. Press <b>Back</b> during the boot window to change
   it on the device; this page does the same thing.</p>

{{if .OverCap}}
<p class="warn">The picker shows {{.Cap}} targets and {{.Registered}} are
   registered. The ones past the cap do not appear at boot.</p>
{{end}}

<form method="POST" action="/boot/default">
  <input type="hidden" name="csrf_token" value="{{.CSRFToken}}">
  <table>
    <tr><th></th><th>Name</th><th>Id</th><th>Source</th></tr>
    {{range .Rows}}
    <tr>
      <td><input type="radio" name="id" value="{{.ID}}" {{if .IsDefault}}checked{{end}}></td>
      <td>{{.Name}}{{if .Missing}} <span class="warn">(entry script missing)</span>{{end}}</td>
      <td><code>{{.ID}}</code></td>
      <td>{{.Source}}</td>
    </tr>
    {{end}}
  </table>
  <button type="submit">Set default</button>
</form>
{{end}}
```

Add a `Boot` entry to the nav in `base.html` beside `System`.

- [ ] **Step 6: Run the tests**

Run: `cd schwung-manager && go test ./... -run TestBootPage -v && go test ./...`
Expected: PASS. `templates_test.go` parses every template, so a template typo fails here rather than at runtime.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/templates/boot.html schwung-manager/templates/base.html schwung-manager/main.go schwung-manager/boot_registry.go schwung-manager/boot_page_test.go
git commit -m "feat: Boot page — the picker rows and the default, from the web UI"
```

---

### Task 9: The Platforms page

**Goal:** Platforms are installable, updatable and removable from the web UI.

**Files:**
- Create: `schwung-manager/templates/platforms.html`
- Modify: `schwung-manager/main.go` (routes, handlers), `schwung-manager/templates/base.html` (nav)
- Modify: `schwung-manager/platforms_test.go`

**Acceptance Criteria:**
- [ ] `GET /platforms` lists catalog platforms with installed version, available version, and an update badge on the same rule modules use.
- [ ] `POST /platforms/{id}/install`, `/update`, `/uninstall` work and redirect with a flash message.
- [ ] A platform that is installed but absent from the catalog is still listed, with uninstall available.
- [ ] The page states plainly that installing a platform adds a boot option and does **not** change what boots.

**Verify:** `cd schwung-manager && go test ./... -run TestPlatform -v` → PASS, and `go test ./...` green.

**Steps:**

- [ ] **Step 1: Extend the test**

```go
// append to schwung-manager/platforms_test.go
func TestPlatformRowsMergeCatalogAndDisk(t *testing.T) {
	root := t.TempDir()
	base := filepath.Join(root, "schwung")
	dir := filepath.Join(root, "platforms", "orphan")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "platform.json"),
		[]byte(`{"id":"orphan","name":"Orphan","version":"0.1.0"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	cat := []CatalogPlatform{{ID: "v", Name: "V"}}

	rows := platformRows(base, cat, map[string]string{"v": "0.4.0"})
	var haveOrphan, haveV bool
	for _, r := range rows {
		if r.ID == "orphan" {
			haveOrphan = true
			if !r.Installed || !r.Removable {
				t.Errorf("orphan row = %+v; an installed platform absent from the catalog must still be removable", r)
			}
		}
		if r.ID == "v" {
			haveV = true
			if r.Installed {
				t.Errorf("v is not installed but row says it is: %+v", r)
			}
			if r.Available != "0.4.0" {
				t.Errorf("available = %q, want 0.4.0", r.Available)
			}
		}
	}
	if !haveOrphan || !haveV {
		t.Fatalf("rows = %+v", rows)
	}
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd schwung-manager && go test ./... -run TestPlatformRows -v`
Expected: FAIL — `undefined: platformRows`.

- [ ] **Step 3: Implement `platformRows` and the handlers**

```go
// in platforms.go

// platformRow is one row of the Platforms page.
type platformRow struct {
	ID          string
	Name        string
	Description string
	Author      string
	Installed   bool
	Version     string // installed
	Available   string // from release.json
	HasUpdate   bool
	Removable   bool
	InCatalog   bool
}

// platformRows merges the catalog with what is on disk. A platform installed
// but no longer catalogued is still listed and still removable — the catalog
// having moved on must never make a payload unremovable, the same rule
// installedDependentsOf follows for modules.
func platformRows(basePath string, cat []CatalogPlatform, available map[string]string) []platformRow {
	installed := discoverInstalledPlatforms(basePath)
	seen := map[string]bool{}
	var rows []platformRow

	for _, p := range cat {
		row := platformRow{ID: p.ID, Name: p.Name, Description: p.Description,
			Author: p.Author, InCatalog: true, Available: available[p.ID]}
		if dir, ok := installed[p.ID]; ok {
			if ip, err := readInstalledPlatform(dir); err == nil {
				row.Installed, row.Version, row.Removable = true, ip.Version, true
				row.HasUpdate = row.Available != "" && row.Available != ip.Version
			}
		}
		seen[p.ID] = true
		rows = append(rows, row)
	}
	for id, dir := range installed {
		if seen[id] {
			continue
		}
		row := platformRow{ID: id, Name: id, Installed: true, Removable: true}
		if ip, err := readInstalledPlatform(dir); err == nil {
			row.Name, row.Version = ip.Name, ip.Version
		}
		rows = append(rows, row)
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].ID < rows[j].ID })
	return rows
}
```

Handlers mirror the module ones (`handleModules`, `handleModuleInstall`, …) — copy their shape, including the flash-redirect helper and CSRF handling, and route:

```go
	mux.HandleFunc("GET /platforms", app.handlePlatforms)
	mux.HandleFunc("POST /platforms/{id}/install", app.handlePlatformInstall)
	mux.HandleFunc("POST /platforms/{id}/update", app.handlePlatformInstall) // install IS update
	mux.HandleFunc("POST /platforms/{id}/uninstall", app.handlePlatformUninstall)
```

- [ ] **Step 4: Write `templates/platforms.html`**

Follow `modules.html`. Include this sentence verbatim near the top, because it is the guarantee the whole design rests on:

```html
<p>Installing a platform adds it to the boot picker. It does not change what
   this Move boots into — set that on the <a href="/boot">Boot</a> page, or by
   pressing Back during the boot window.</p>
```

- [ ] **Step 5: Run the tests**

Run: `cd schwung-manager && go test ./... && go build ./...`
Expected: PASS, clean build.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/platforms.go schwung-manager/platforms_test.go schwung-manager/templates/platforms.html schwung-manager/templates/base.html schwung-manager/main.go
git commit -m "feat: Platforms page — install, update and remove platform payloads"
```

---

### Task 10: Documentation

**Goal:** The contract is documented where a third-party author will look, and one pre-existing contradiction in it is fixed.

**Files:**
- Modify: `docs/BOOT_TARGETS.md`, `docs/MODULES.md`, `CLAUDE.md`, `scripts/uninstall.sh`

**Acceptance Criteria:**
- [ ] `BOOT_TARGETS.md` gains a "Registering through Schwung Manager" section: the `boot_target` block, both payload shapes, the name/id/exec rules **and why the name rules exist**, the 14-target cap, and the statement that a manager-registered entry must not be hand-edited (reconcile reverts it).
- [ ] `BOOT_TARGETS.md`'s "What the user sees" says **three** failed boots, matching `BT_STRIKE_LIMIT` and its own Watchdog section (it currently says two).
- [ ] `MODULES.md` documents `boot_target` in the module.json reference.
- [ ] `CLAUDE.md` gains **one bullet** under Module Install / Update pointing at `BOOT_TARGETS.md` — the hook, not the prose.
- [ ] `scripts/uninstall.sh` names the platforms directory it deliberately does not remove.

**Verify:** `grep -n "Three" docs/BOOT_TARGETS.md` shows the corrected line; `bash tests/host/test_boot_target_manager_json.sh` still passes.

**Steps:**

- [ ] **Step 1: Add the registration section to `docs/BOOT_TARGETS.md`**

After "Installing / uninstalling your platform", add:

```markdown
## Registering through Schwung Manager

Anything the manager installs — an ordinary module that ships a binary, or a
platform payload with no module in it — can declare a boot target in its
manifest and let the manager register it:

```json
"boot_target": { "name": "V", "exec": "entry.sh" }
```

`exec` is **relative to your own directory**; the manager composes the absolute
path. A payload never states where it is installed, so moving the install roots
is one change plus one reconcile pass rather than an edit per target.

Rules, all enforced at registration with the reason logged:

- `id` (optional, defaults to your payload id) matches `[a-z0-9-]+` and is not
  `schwung` or `stock`.
- `name` is 1–24 characters of printable ASCII and **must not contain `"` or
  `\`**. This is a parser constraint: `bt_json_field` truncates a value at its
  first quote and `bs_json_field` calls the same value malformed — neither can
  unescape, so such a name is refused rather than mangled at boot.
- `exec` is relative, does not contain `..`, resolves inside your directory
  (symlinks included), and exists after extraction. A file that lost its
  executable bit in transit is chmodded rather than refused.
- The picker holds **14 targets** beside Stock and Schwung. Registration past
  that is refused: `bs_row_insert_sorted` drops the overflow silently, in id
  order, so a target that "did not appear" would be unattributable.

The manager writes `boot.json` with an `owner` field and **only ever rewrites
or deletes entries carrying an owner it recognises**. A target you installed by
hand, as described above, is never touched. The reverse is also true: do not
hand-edit an entry the manager owns — a reconcile pass will put it back.

Uninstalling the payload removes its entry, and heals `boot-targets/default` to
`schwung` if it named the removed target. Installing **never** changes the
default: a new target is a new row in the picker, nothing more.

Platform payloads install to `/data/UserData/platforms/<id>/` from a tarball
whose top-level directory is `<id>/`, carrying a `platform.json`
(`id`, `name`, `version`, `author`, and the `boot_target` block).
```

- [ ] **Step 2: Fix the strike-count contradiction**

In "What the user sees", replace:

```
- Two failed boots of any target: the picker, with a failure banner, cursor on
  Stock Move.
```

with:

```
- Three failed boots of any target: the picker, with a failure banner, cursor
  on Stock Move. Three and not two because a power cycle inside the liveness
  window is indistinguishable from a failed boot — see Watchdog.
```

- [ ] **Step 3: Note the uninstall consequence**

In `docs/BOOT_TARGETS.md`, under the registration section, add:

```markdown
Uninstalling Schwung removes the whole registry (`scripts/uninstall.sh`), so
every target is deregistered at once. Platform payloads are left on disk under
`/data/UserData/platforms/`; reinstalling Schwung brings them back as picker
rows at the manager's next reconcile, but the previous default is gone.
```

and in `scripts/uninstall.sh`, beside the `rm -rf /data/UserData/boot-targets` line (~line 204):

```sh
    # Payloads under /data/UserData/platforms are deliberately NOT removed:
    # they are somebody else's software, installed separately. Reinstalling
    # Schwung re-registers them at the manager's next reconcile.
```

- [ ] **Step 4: Document the block in `docs/MODULES.md`**

Add `boot_target` to the module.json reference with the same four rules, and a
one-line pointer to `docs/BOOT_TARGETS.md` for the whole contract.

- [ ] **Step 5: Add the CLAUDE.md hook**

Under **Module Install / Update**, one bullet:

```markdown
**A manager-installed payload can register a BOOT TARGET, and the registry is
OWNER-KEYED.** `boot_target: {name, exec}` in `module.json` (or a platform's
`platform.json`) — `exec` relative, because a payload never states where it is
installed. The manager writes `boot-targets/<id>/boot.json` with an `owner`
field and touches **only** entries whose owner it recognises, so a
hand-installed target and Schwung's self-registered entry survive reconcile. A
name carrying `"` or `\` is REFUSED, not escaped: `bt_json_field` truncates at
the first quote and `bs_json_field` calls it malformed, and neither can
unescape. The picker holds 14 targets beside Stock and Schwung, dropping the
overflow **silently in id order**, so registration past the cap is refused
instead. Installing never changes `default`. See `docs/BOOT_TARGETS.md`.
```

- [ ] **Step 6: Verify and commit**

```bash
bash tests/host/test_boot_target_manager_json.sh
git add docs/BOOT_TARGETS.md docs/MODULES.md CLAUDE.md scripts/uninstall.sh
git commit -m "docs: manager boot-target registration; fix the two-vs-three strike contradiction"
```

---

### Task 11: Hardware verification

**Goal:** Prove on the device that a registered target boots, and that uninstalling takes the row away.

**USER-ORDERED GATE — NON-SKIPPABLE.** Boot-path changes have burned this project before, and a green CI run has never once proven a boot behaviour. Each guard below gets its **own** pass; one end-to-end run does not stand in for all of them.

**Files:** none (verification only)

**Acceptance Criteria:**
- [ ] A test platform tarball installs from the manager, and `/data/UserData/boot-targets/<id>/boot.json` exists with the right `exec` and `owner`.
- [ ] The boot window and picker show the target by name (photo or on-device confirmation).
- [ ] Choosing it in the picker boots it; the device comes back to Schwung after choosing Schwung again.
- [ ] Uninstalling from the manager removes the picker row.
- [ ] With the target as `default`, uninstalling it leaves `default` reading `schwung`, and the next boot is Schwung.
- [ ] A hand-created registry entry (no `owner`) survives an install, an uninstall, and a manager restart.
- [ ] `debug_log_on` is disarmed afterwards if it was armed.

**Verify:** Each criterion checked individually on the device, with the command output or photo captured in the session.

**Steps:**

- [ ] **Step 1: Ask before deploying.** Do not deploy to `move.local` without asking — the device may be in use.

- [ ] **Step 2: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

- [ ] **Step 3: Plant a hand-installed control entry first**

```bash
ssh ableton@move.local 'mkdir -p /data/UserData/boot-targets/handmade && \
  printf "{\n  \"name\": \"Handmade\",\n  \"exec\": \"/data/UserData/handmade/entry.sh\"\n}\n" \
  > /data/UserData/boot-targets/handmade/boot.json'
```

This is the control for the never-touch-unowned guard: it must still be there at the end.

- [ ] **Step 4: Install the test payload and inspect the registry**

```bash
ssh ableton@move.local 'cat /data/UserData/boot-targets/*/boot.json; echo ---; cat /data/UserData/boot-targets/default'
```

Expected: the new entry, flat, with `owner`, and `default` unchanged by the install.

- [ ] **Step 5: Boot it, then come back**

Reboot, press Back during the window, confirm the row's name, select it, confirm it runs. Reboot, press Back, select Schwung.

- [ ] **Step 6: Uninstall with the target as default**

Set it as default on the Boot page, uninstall it, then:

```bash
ssh ableton@move.local 'ls /data/UserData/boot-targets; cat /data/UserData/boot-targets/default'
```

Expected: the entry gone, `default` reading `schwung`, `handmade` still present.

- [ ] **Step 7: Report each guard separately**

State which guards passed and which were not exercised. Do not report "verified" for a guard that was not run on its own.

---

## Self-Review

**Spec coverage:** §1 manifest block → Task 2. §2 registry entry + owner → Task 3. §2a selector limits → Tasks 2 (name), 5 (cap), 4 (parser pin). §3 roots in one place → Task 1. §3a trust guarantee → Tasks 5 (reconcile writes default one way), 8 (setBootDefault), 9 (page copy), 10 (docs). §4 install/uninstall/reconcile → Tasks 5, 7. §5 platforms → Task 6, 9. §6 Boot page → Task 8. Consequences + docs → Task 10. Testing section → Tasks 1–9 inline, plus Task 11 on hardware.

**Type consistency:** `registryEntry{ID,Name,Exec,Version,Owner}` is written in Task 3 and used unchanged in Tasks 5, 7, 8. `BootTarget{ID,Name,ExecAbs}` is Task 2's only export into Task 5. `bootPickerTargetCap` is defined in Task 5 and referenced in Tasks 5 and 8. `ownerForModule`/`ownerForPlatform` are defined in Task 3 and used in Tasks 5, 6, 7.

**Known gap accepted:** `discoverInstalledModules` returns `map[string]InstalledModule` and Task 5 pairs it with `findModuleDir` for the directory. If `InstalledModule` already carries its directory, use that field and drop the second lookup.
