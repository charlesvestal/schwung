# Cross-Core Module Attribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the CPU page account for a fork-parallel module's real cost — the child processes it runs on cores 0–2 — and take module identity from disk so the page stops depending on a contended param channel.

**Architecture:** Module identity moves to the on-disk set state (`active_set.txt` → `slot_N.json`), with a fallback param read only when disk and telemetry disagree. Forked children are found by walking MoveOriginal's process tree recursively and subtracting the four helpers the shim spawns; what remains is attributed to a module by a declared `capabilities.forks_processes` flag, falling back to inference (visibly marked) and then to an unattributed group. `App.shmParams` gains a lazy accessor so the boot race stops silently disabling consumers.

**Tech Stack:** Go (schwung-manager), `html/template` + htmx, `go test`.

**User decisions (already made):**
- "fix all of it" — cross-core attribution *and* the `(name unread)` failure, as one piece of work.
- Both slot names **and** which modules fork come from disk.
- Fork detection is layered: `capabilities.forks_processes` when declared, inference when not ("2, and 1?").
- `App.shmParams` boot race fixed **for everyone**, including sharing the handle with `RemoteUI`.

**Spec:** `docs/superpowers/specs/2026-09-03-cross-core-attribution-design.md`
**Branch:** `manager-cpu-view` (continues PR #393)

---

## File Structure

`schwung-manager/perf.go` is 650 lines doing four jobs, and this work adds two more. It splits by responsibility:

| File | Responsibility |
|---|---|
| `perf_modules.go` | **New.** Disk identity only: `active_set.txt`, the `slot_N.json` / `master_fx_N.json` schema, `module.json` capabilities. Moves `moduleID` and `moduleIDCache` out of `perf.go`. |
| `perf_forks.go` | **New.** Fork-tree discovery and the attribution decision. Pure functions over `[]ProcStat`. |
| `perf_proc.go` | **Modify.** Gains the raw tree helpers (`scanAllProcesses`, `descendantsOf`) — it already owns `/proc` parsing. |
| `perf.go` | **Modify.** Keeps the view models and handlers; loses identity and gains the fork panel. Shrinks. |
| `main.go` | **Modify.** `App.params()` accessor; `RemoteUI` shares the handle. |
| `remote_ui.go` | **Modify.** `ensureShm()` delegates to `App.params()` instead of opening a second mapping. |
| `templates/partials/cpu_values.html` | **Modify.** Forked-process panel; inferred marking; frame-budget cross-reference. |

---

### Task 1: Disk-backed module identity

**Goal:** `perf_modules.go` reads which module sits in each slot and Master FX position from the on-disk set state, with no param round-trips.

**Files:**
- Create: `schwung-manager/perf_modules.go`
- Create: `schwung-manager/perf_modules_test.go`
- Modify: `schwung-manager/perf.go` (delete `moduleID`, `moduleIDCache`, `moduleIDRefresh`, `(app *App) moduleIDs` — they move)

**Acceptance Criteria:**
- [ ] The set uuid comes from the **first line** of `active_set.txt`, not from directory mtime or glob order.
- [ ] Slot module reads `chain.synth.module`; audio FX read `chain.audio_fx[].type`; MIDI FX read `chain.midi_fx[].type`.
- [ ] Master FX reads `module_id` from `master_fx_N.json`.
- [ ] `{}` and a null `synth` both read as **empty**, distinct from a read failure.
- [ ] A missing `active_set.txt`, or one naming a directory that does not exist, reports failure — not "everything empty".
- [ ] `moduleCapabilities` reads `capabilities.forks_processes` from an installed module's `module.json`, defaulting false when absent and reporting failure when the file cannot be read.

**Verify:** `cd schwung-manager && go test ./... -run TestModules -v` → all PASS

**Steps:**

- [ ] **Step 1: Write the failing tests**

Create `schwung-manager/perf_modules_test.go`:

```go
package main

import (
	"os"
	"path/filepath"
	"testing"
)

// writeSet lays out a set-state tree the way the device actually has it.
func writeSet(t *testing.T, base, uuid string, files map[string]string) {
	t.Helper()
	dir := filepath.Join(base, "set_state", uuid)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	for name, body := range files {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// active_set.txt carries the uuid on line 1 and a human name on line 2.
	if err := os.WriteFile(filepath.Join(base, "active_set.txt"),
		[]byte(uuid+"\nSet 11\n"), 0o644); err != nil {
		t.Fatal(err)
	}
}

// The real shape, taken from the device. Every key here was got wrong once
// before it was checked: the module is under chain.synth.module (not
// synth.module), audio FX use "type" (not "module"), and Master FX uses
// module_id in a different file.
const realSlot0 = `{"name":"s","version":1,"chain":{"custom_name":"Untitled",
 "input":"both","synth":{"module":"jp8000","config":{"state":"..."}},
 "audio_fx":[{"type":"ducker"}],"midi_fx":[{"type":"arp"}]}}`

func TestModulesReadsTheRealSchema(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "a7cbc746", map[string]string{
		"slot_0.json":      realSlot0,
		"slot_1.json":      `{"chain":{"synth":{"module":"9w9"}}}`,
		"slot_2.json":      `{}`,
		"slot_3.json":      `{"chain":{"synth":null}}`,
		"master_fx_0.json": `{"module_id":"ottx"}`,
		"master_fx_1.json": `{}`,
	})

	set, err := readSetState(base)
	if err != nil {
		t.Fatalf("wanted a clean read, got %v", err)
	}
	if got := set.Slots[0].Synth; got != "jp8000" {
		t.Errorf("slot 0 synth = %q, want jp8000 (chain.synth.module)", got)
	}
	if got := set.Slots[1].Synth; got != "9w9" {
		t.Errorf("slot 1 synth = %q, want 9w9", got)
	}
	if got := set.Slots[0].AudioFX; len(got) != 1 || got[0] != "ducker" {
		t.Errorf("slot 0 audio_fx = %v, want [ducker] (the key is \"type\")", got)
	}
	if got := set.Slots[0].MidiFX; len(got) != 1 || got[0] != "arp" {
		t.Errorf("slot 0 midi_fx = %v, want [arp]", got)
	}
	if got := set.MasterFX[0]; got != "ottx" {
		t.Errorf("master_fx 0 = %q, want ottx (the key is module_id)", got)
	}
}

// {} and a null synth are EMPTY. That is a finding, not a failure, and the two
// must not share a representation - an empty slot draws no row, a failed read
// draws one labelled as unread.
func TestModulesEmptyIsNotFailure(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "u", map[string]string{
		"slot_0.json": `{}`,
		"slot_1.json": `{"chain":{"synth":null}}`,
	})
	set, err := readSetState(base)
	if err != nil {
		t.Fatalf("empty slots must not error: %v", err)
	}
	for i := 0; i < 2; i++ {
		if set.Slots[i].Synth != "" {
			t.Errorf("slot %d should be empty, got %q", i, set.Slots[i].Synth)
		}
		if !set.Slots[i].Read {
			t.Errorf("slot %d was READ successfully and happens to be empty; "+
				"Read must stay true so the caller can tell it apart from a failure", i)
		}
	}
}

// The device had 27 sets. Picking by mtime or glob order chose the wrong one
// twice; only active_set.txt is authoritative.
func TestModulesUsesActiveSetNotNewest(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "older", map[string]string{
		"slot_0.json": `{"chain":{"synth":{"module":"correct"}}}`,
	})
	// A second, NEWER set that must be ignored.
	newer := filepath.Join(base, "set_state", "zzz-newer")
	if err := os.MkdirAll(newer, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(newer, "slot_0.json"),
		[]byte(`{"chain":{"synth":{"module":"wrong"}}}`), 0o644); err != nil {
		t.Fatal(err)
	}

	set, err := readSetState(base)
	if err != nil {
		t.Fatal(err)
	}
	if set.Slots[0].Synth != "correct" {
		t.Fatalf("read %q - active_set.txt is the only authority; mtime and "+
			"glob order both point at the wrong set", set.Slots[0].Synth)
	}
}

func TestModulesMissingActiveSetIsAFailure(t *testing.T) {
	if _, err := readSetState(t.TempDir()); err == nil {
		t.Fatal("a missing active_set.txt must report failure, not an empty rig")
	}
}

func TestModulesActiveSetNamingAMissingDirIsAFailure(t *testing.T) {
	base := t.TempDir()
	if err := os.WriteFile(filepath.Join(base, "active_set.txt"),
		[]byte("does-not-exist\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := readSetState(base); err == nil {
		t.Fatal("an active_set.txt pointing at a missing directory must report " +
			"failure, not an empty rig")
	}
}

func TestModuleCapabilitiesReadsForksProcesses(t *testing.T) {
	base := t.TempDir()
	dir := filepath.Join(base, "modules", "sound_generators", "jp8000")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "module.json"),
		[]byte(`{"id":"jp8000","capabilities":{"forks_processes":true}}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if got, ok := moduleForksProcesses(base, "jp8000"); !ok || !got {
		t.Fatalf("jp8000 should declare forks_processes (got %v, ok %v)", got, ok)
	}
	if got, ok := moduleForksProcesses(base, "nosuchmodule"); ok || got {
		t.Fatalf("an unreadable module.json must report ok=false, got %v/%v", got, ok)
	}
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/.claude/worktrees/manager-cpu-view/schwung-manager
go test ./... -run TestModules 2>&1 | head -5
```
Expected: FAIL — `undefined: readSetState`.

- [ ] **Step 3: Write perf_modules.go**

```go
package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Module identity, read from the on-disk set state rather than over the param
// channel.
//
// WHY DISK: naming positions over the param channel costs 12 requests per
// refresh, each SERVED BY THE SHIM ON THE SPI CALLBACK - measurably so, the
// `Param requests` section's max went from ~36us to ~140us once this page
// started polling. The same facts are already on disk, at no realtime cost.
//
// THE SCHEMA IS NOT WHAT YOU WOULD GUESS. Every line below was got wrong once
// before it was checked against the device:
//
//	active_set.txt     line 1 = set uuid    NOT the newest mtime. 27 sets
//	                                        existed; mtime order and glob order
//	                                        each picked a different wrong one.
//	slot_N.json        chain.synth.module   NOT synth.module
//	                   chain.audio_fx[].type   "type", not "module"
//	                   chain.midi_fx[].type
//	master_fx_N.json   module_id            a different key again

// SlotState is one chain slot as recorded on disk.
type SlotState struct {
	Synth   string
	AudioFX []string
	MidiFX  []string
	// Read is true when the file was parsed, even if everything is empty.
	// Empty and unreadable are different findings and must not share a
	// representation.
	Read bool
}

// SetState is the active set's chain, as far as disk knows it.
type SetState struct {
	UUID     string
	Name     string
	Slots    [perfChainSlots]SlotState
	MasterFX [perfMasterFXSlots]string
}

// readSetState loads the ACTIVE set. basePath is the Schwung root
// (/data/UserData/schwung on the device).
func readSetState(basePath string) (*SetState, error) {
	raw, err := os.ReadFile(filepath.Join(basePath, "active_set.txt"))
	if err != nil {
		return nil, fmt.Errorf("active_set.txt: %w", err)
	}
	lines := strings.Split(strings.TrimSpace(string(raw)), "\n")
	uuid := strings.TrimSpace(lines[0])
	if uuid == "" {
		return nil, errors.New("active_set.txt is empty")
	}
	out := &SetState{UUID: uuid}
	if len(lines) > 1 {
		out.Name = strings.TrimSpace(lines[1])
	}

	dir := filepath.Join(basePath, "set_state", uuid)
	if st, err := os.Stat(dir); err != nil || !st.IsDir() {
		return nil, fmt.Errorf("active set %q has no directory", uuid)
	}

	for i := 0; i < perfChainSlots; i++ {
		out.Slots[i] = readSlotFile(filepath.Join(dir, "slot_"+strconv.Itoa(i)+".json"))
	}
	for i := 0; i < perfMasterFXSlots; i++ {
		out.MasterFX[i] = readMasterFXFile(
			filepath.Join(dir, "master_fx_"+strconv.Itoa(i)+".json"))
	}
	return out, nil
}

// slotFile mirrors only the fields we need. Everything else in these files is
// module state and can be large; there is no reason to model it.
type slotFile struct {
	Chain *struct {
		Synth *struct {
			Module string `json:"module"`
		} `json:"synth"`
		AudioFX []struct {
			Type string `json:"type"`
		} `json:"audio_fx"`
		MidiFX []struct {
			Type string `json:"type"`
		} `json:"midi_fx"`
	} `json:"chain"`
}

func readSlotFile(path string) SlotState {
	var out SlotState
	raw, err := os.ReadFile(path)
	if err != nil {
		return out // Read stays false: we could not look.
	}
	var f slotFile
	if err := json.Unmarshal(raw, &f); err != nil {
		return out
	}
	out.Read = true // Parsed. Whatever it holds, including nothing.
	if f.Chain == nil {
		return out
	}
	if f.Chain.Synth != nil {
		out.Synth = f.Chain.Synth.Module
	}
	for _, fx := range f.Chain.AudioFX {
		if fx.Type != "" {
			out.AudioFX = append(out.AudioFX, fx.Type)
		}
	}
	for _, fx := range f.Chain.MidiFX {
		if fx.Type != "" {
			out.MidiFX = append(out.MidiFX, fx.Type)
		}
	}
	return out
}

func readMasterFXFile(path string) string {
	raw, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	var f struct {
		ModuleID string `json:"module_id"`
	}
	if err := json.Unmarshal(raw, &f); err != nil {
		return ""
	}
	return f.ModuleID
}

// moduleForksProcesses reports whether an installed module declares that it
// forks child processes. ok is false when the module.json could not be read at
// all, which is different from a module that simply does not declare it.
func moduleForksProcesses(basePath, id string) (forks bool, ok bool) {
	if id == "" {
		return false, false
	}
	// Modules live under modules/<component_type>s/<id>/. The type is not known
	// here, so glob rather than guess.
	matches, _ := filepath.Glob(filepath.Join(basePath, "modules", "*", id, "module.json"))
	if len(matches) == 0 {
		return false, false
	}
	raw, err := os.ReadFile(matches[0])
	if err != nil {
		return false, false
	}
	var m struct {
		Capabilities struct {
			ForksProcesses bool `json:"forks_processes"`
		} `json:"capabilities"`
	}
	if err := json.Unmarshal(raw, &m); err != nil {
		return false, false
	}
	return m.Capabilities.ForksProcesses, true
}

// --- identity presented to the page ----------------------------------------

// moduleID keeps the THREE answers apart: a name, "this position is empty", and
// "the read did not complete". Collapsing the last two hides a slot that is
// burning CPU, which is the worst failure this page has.
type moduleID struct {
	Name     string
	Answered bool
}

// Loaded reports whether a module is actually there.
func (m moduleID) Loaded() bool { return m.Answered && m.Name != "" }

// moduleIDCache holds identities between polls. Disk reads are cheap, but they
// are still syscalls on every 1 s poll for no benefit - identity changes only
// when someone loads or swaps a module.
type moduleIDCache struct {
	mu     sync.Mutex
	set    *SetState
	readAt time.Time
}

// moduleIDRefresh is how often the set state is re-read from disk.
const moduleIDRefresh = 2 * time.Second
```

- [ ] **Step 4: Remove the moved declarations from perf.go**

Delete from `schwung-manager/perf.go`: the `moduleID` type, its `Loaded` method, `moduleIDCache`, `moduleIDRefresh`, and the whole `func (app *App) moduleIDs()`. They now live in `perf_modules.go` (the first four verbatim; `moduleIDs` is replaced in Task 2).

The build will break until Task 2 — that is expected and is why these two tasks are adjacent. Do not paper over it with a stub.

- [ ] **Step 5: Run the tests**

```bash
cd .../schwung-manager && go test ./... -run TestModules -v 2>&1 | grep -E "^(--- |ok|FAIL)"
```
Expected: all PASS. (`go build ./...` still fails — Task 3 finishes the wiring.)

- [ ] **Step 6: Prove a test can fail**

Change `readSetState` to pick the newest directory instead of reading `active_set.txt`, re-run, and confirm `TestModulesUsesActiveSetNotNewest` FAILS. Restore, confirm PASS. Report both.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/perf_modules.go schwung-manager/perf_modules_test.go schwung-manager/perf.go
git commit -m "manager: read module identity from the on-disk set state

The schema is not what you would guess, and every key here was got wrong
once before being checked against a device: the set comes from
active_set.txt (not the newest mtime - there were 27 sets and mtime and
glob order each chose a different wrong one), the module is at
chain.synth.module, audio FX use \"type\", and Master FX uses module_id in
a separate file."
```

```json:metadata
{"files": ["schwung-manager/perf_modules.go", "schwung-manager/perf_modules_test.go", "schwung-manager/perf.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestModules -v", "acceptanceCriteria": ["set uuid comes from line 1 of active_set.txt, not mtime or glob order", "slot module reads chain.synth.module and FX read .type", "master FX reads module_id", "{} and null synth read as empty with Read=true", "missing or dangling active_set.txt reports failure", "moduleForksProcesses reads capabilities.forks_processes and reports ok=false when unreadable"], "modelTier": "standard"}
```

---

### Task 2: One param handle, lazily attached

**Goal:** `App.params()` exists, every consumer uses it, and `RemoteUI` shares that handle instead of opening a second mapping.

**Files:**
- Modify: `schwung-manager/main.go` (accessor + field)
- Modify: `schwung-manager/remote_ui.go:288-303` (`ensureShm` delegates)
- Create: `schwung-manager/params_test.go`

**Acceptance Criteria:**
- [ ] `App.params()` returns nil while `/dev/shm/schwung-param` is absent and a working handle once it appears, without a restart.
- [ ] `RemoteUI.ensureShm()` returns the same `*ShmParams` pointer as `App.params()`.
- [ ] No code path other than `App.params()` calls `OpenShmParams()`.
- [ ] The startup log no longer claims "not available" permanently — a later attach is reported.

**Verify:** `cd schwung-manager && go test ./... -run TestParams -v && grep -c 'OpenShmParams()' *.go` → tests PASS, count is 2 (the definition and the single accessor)

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `schwung-manager/params_test.go`:

```go
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
```

- [ ] **Step 2: Make `shmParamPath` a var**

In `schwung-manager/shmparams.go`, move `shmParamPath` out of its `const` block to:

```go
// A var, not a const, so a test can point it at a temp file.
var shmParamPath = "/dev/shm/schwung-param"
```

- [ ] **Step 3: Add the accessor**

In `main.go`, add beside `perfMu`:

```go
	paramsMu sync.Mutex // guards the lazy attach of shmParams
```

and beside `perfSegment()` in `perf.go` (or `main.go` next to `render`, whichever the implementer finds tidier — keep it with `perfSegment` for symmetry):

```go
// params returns the shared param channel, attaching lazily.
//
// Same race, same shape, same lesson as perfSegment(): the manager and the shim
// start independently and the manager can win. Measured on the device, it came
// up 2 s before /dev/shm/schwung-param was usable, logged "not available", and
// then held nil for the life of the process while RemoteUI quietly opened its
// OWN second mapping and worked. Every consumer that read App.shmParams
// directly degraded silently; the CPU page was simply the one that noticed.
//
// One handle, attached on demand, shared by everyone.
func (app *App) params() *ShmParams {
	app.paramsMu.Lock()
	defer app.paramsMu.Unlock()
	if app.shmParams == nil {
		if app.shmParams = OpenShmParams(); app.shmParams != nil && app.logger != nil {
			app.logger.Info("shared memory params: connected")
		}
	}
	return app.shmParams
}
```

- [ ] **Step 4: Point every consumer at it**

```bash
grep -rn "app\.shmParams\|ru\.shm\b" schwung-manager/*.go | grep -v "_test.go"
```

Replace direct `app.shmParams` reads with `app.params()`. In `remote_ui.go`, replace the body of `ensureShm` with:

```go
// ensureShm returns the shared param channel. It delegates to App so the
// manager keeps exactly ONE mapping of the segment - this used to open its own,
// which is why the Remote UI could work while the CPU page reported no channel.
func (ru *RemoteUI) ensureShm() *ShmParams {
	return ru.app.params()
}
```

If `RemoteUI` has no `app` field, add one and set it where `RemoteUI` is constructed. Delete the now-unused `ru.shm` field and its `ru.mu` guard **only if** nothing else uses them; otherwise leave the mutex alone.

- [ ] **Step 5: Verify**

```bash
cd .../schwung-manager && go build ./... && go vet ./... && go test ./... 2>&1 | tail -3
grep -c "OpenShmParams()" *.go
```
Expected: builds clean, tests PASS, `OpenShmParams()` appears exactly twice (its definition in `shmparams.go`, and the one call in `params()`).

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/main.go schwung-manager/perf.go schwung-manager/remote_ui.go \
        schwung-manager/shmparams.go schwung-manager/params_test.go
git commit -m "manager: one lazily-attached param handle, shared

App.shmParams was nil for the life of the process whenever the manager won
the boot race, while RemoteUI independently opened a second working
mapping. Every consumer reading the App field degraded silently."
```

```json:metadata
{"files": ["schwung-manager/main.go", "schwung-manager/perf.go", "schwung-manager/remote_ui.go", "schwung-manager/shmparams.go", "schwung-manager/params_test.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestParams -v && grep -c 'OpenShmParams()' *.go", "acceptanceCriteria": ["App.params() returns nil while absent and a handle once present, no restart", "RemoteUI.ensureShm returns the same pointer as App.params()", "OpenShmParams() is called from exactly one place", "a later attach is logged"], "modelTier": "standard"}
```

---

### Task 3: Wire disk identity in, with the freshness guard

**Goal:** The page names positions from disk, falling back to a single param read only when disk and telemetry disagree.

**Files:**
- Modify: `schwung-manager/perf_modules.go` (add the resolver)
- Modify: `schwung-manager/perf.go` (call site)
- Modify: `schwung-manager/perf_modules_test.go` (add the guard tests)

**Acceptance Criteria:**
- [ ] With disk populated, a poll performs **zero** param reads.
- [ ] When disk reports a position empty but the snapshot shows nonzero timing for it, exactly one param read is issued for that position.
- [ ] When disk itself failed, every position is `Answered: false` — never silently empty.
- [ ] `go build ./...` succeeds again.

**Verify:** `cd schwung-manager && go test ./... -run 'TestModules|TestFreshness' -v` → all PASS

**Steps:**

- [ ] **Step 1: Write the failing tests**

Append to `schwung-manager/perf_modules_test.go`:

```go
// fakeParamReader counts reads so a test can assert the steady state costs
// none. The real one is *ShmParams; only this method is needed.
type fakeParamReader struct {
	reads int
	vals  map[string]string
}

func (f *fakeParamReader) TryGetParam(slot uint8, key string) (string, bool, error) {
	f.reads++
	return f.vals[key], true, nil
}

func TestFreshnessNoParamReadsWhenDiskIsPopulated(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "u", map[string]string{
		"slot_0.json": `{"chain":{"synth":{"module":"jp8000"}}}`,
	})
	set, err := readSetState(base)
	if err != nil {
		t.Fatal(err)
	}
	p := &fakeParamReader{}
	snap := &PerfSnapshot{SlotSynthAvg: [perfChainSlots]uint64{290}}

	slots, _ := resolveModuleIDs(set, snap, p)
	if p.reads != 0 {
		t.Fatalf("disk answered; wanted 0 param reads, got %d - the whole point "+
			"is to stop paying SPI-served requests for something already on disk",
			p.reads)
	}
	if slots[0].Name != "jp8000" || !slots[0].Loaded() {
		t.Fatalf("slot 0 = %+v, want jp8000 loaded", slots[0])
	}
}

// Disk lags a hot swap until autosave writes. Timing for a slot disk calls
// empty is a CONTRADICTION, and the only case worth spending a read on.
func TestFreshnessFallsBackWhenDiskAndTelemetryDisagree(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "u", map[string]string{"slot_0.json": `{}`})
	set, err := readSetState(base)
	if err != nil {
		t.Fatal(err)
	}
	p := &fakeParamReader{vals: map[string]string{"synth_module": "justloaded"}}
	snap := &PerfSnapshot{SlotSynthAvg: [perfChainSlots]uint64{290}}

	slots, _ := resolveModuleIDs(set, snap, p)
	if p.reads != 1 {
		t.Fatalf("wanted exactly 1 fallback read, got %d", p.reads)
	}
	if slots[0].Name != "justloaded" {
		t.Fatalf("slot 0 = %q, want the param answer", slots[0].Name)
	}
}

// An empty slot with NO timing is genuinely empty. Spending a read on it would
// put the per-second param load straight back.
func TestFreshnessNoFallbackForAGenuinelyEmptySlot(t *testing.T) {
	base := t.TempDir()
	writeSet(t, base, "u", map[string]string{"slot_0.json": `{}`})
	set, _ := readSetState(base)
	p := &fakeParamReader{}
	slots, _ := resolveModuleIDs(set, &PerfSnapshot{}, p)
	if p.reads != 0 {
		t.Fatalf("empty slot with no timing needs no read, got %d", p.reads)
	}
	if slots[0].Loaded() {
		t.Fatal("slot 0 should read as empty")
	}
	if !slots[0].Answered {
		t.Fatal("we DID read it (from disk) and it is empty - Answered must be true")
	}
}

// A failed disk read is not an empty rig.
func TestFreshnessNilSetIsUnansweredNotEmpty(t *testing.T) {
	slots, mfx := resolveModuleIDs(nil, &PerfSnapshot{}, &fakeParamReader{})
	for i := range slots {
		if slots[i].Answered {
			t.Fatalf("slot %d: a failed disk read must be Answered=false", i)
		}
	}
	if mfx[0].Answered {
		t.Fatal("master fx 0: a failed disk read must be Answered=false")
	}
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cd .../schwung-manager && go test ./... -run TestFreshness 2>&1 | head -5
```
Expected: FAIL — `undefined: resolveModuleIDs`.

- [ ] **Step 3: Write the resolver**

Append to `schwung-manager/perf_modules.go`:

```go
// paramReader is the slice of *ShmParams this file needs. An interface so the
// freshness fallback is testable without a device.
type paramReader interface {
	TryGetParam(slot uint8, key string) (string, bool, error)
}

// resolveModuleIDs names every position, preferring disk.
//
// The FRESHNESS GUARD is the only reason a param read ever happens: disk lags a
// hot swap until autosave writes, so a position disk calls empty WHILE THE
// SNAPSHOT SHOWS TIME FOR IT is a contradiction, and worth one read to settle.
// A position that is empty on disk and idle in the telemetry is simply empty -
// spending a read there would put the whole per-second param load back.
func resolveModuleIDs(set *SetState, snap *PerfSnapshot, p paramReader) (
	slots [perfChainSlots]moduleID, mfx [perfMasterFXSlots]moduleID) {

	// A failed disk read leaves everything Answered=false: unread, not empty.
	if set == nil {
		return slots, mfx
	}

	for i := 0; i < perfChainSlots; i++ {
		s := set.Slots[i]
		slots[i] = moduleID{Name: s.Synth, Answered: s.Read}

		if s.Read && s.Synth == "" && p != nil && snapSlotBusy(snap, i) {
			if val, ok, err := p.TryGetParam(uint8(i), "synth_module"); ok && err == nil {
				slots[i] = moduleID{Name: val, Answered: true}
			}
		}
	}

	for i := 0; i < perfMasterFXSlots; i++ {
		name := set.MasterFX[i]
		mfx[i] = moduleID{Name: name, Answered: true}

		if name == "" && p != nil && snapMfxBusy(snap, i) {
			key := "master_fx:" + strconv.Itoa(i) + ":module"
			if val, ok, err := p.TryGetParam(0, key); ok && err == nil {
				mfx[i] = moduleID{Name: val, Answered: true}
			}
		}
	}
	return slots, mfx
}

func snapSlotBusy(snap *PerfSnapshot, i int) bool {
	if snap == nil || i >= perfChainSlots {
		return false
	}
	return snap.SlotSynthAvg[i] > 0 || snap.SlotSynthMax[i] > 0 ||
		snap.SlotFxAvg[i] > 0 || snap.SlotFxMax[i] > 0
}

func snapMfxBusy(snap *PerfSnapshot, i int) bool {
	if snap == nil || i >= perfMasterFXSlots {
		return false
	}
	return snap.MfxAvg[i] > 0 || snap.MfxMax[i] > 0
}

// moduleIDs returns the page's view of identity, re-reading disk on the cache
// interval. The set state is also handed back so the fork attribution can ask
// which modules are loaded.
func (app *App) moduleIDs(snap *PerfSnapshot) (
	slots [perfChainSlots]moduleID, mfx [perfMasterFXSlots]moduleID, set *SetState) {

	app.moduleIDs_.mu.Lock()
	if app.moduleIDs_.set == nil || time.Since(app.moduleIDs_.readAt) > moduleIDRefresh {
		s, err := readSetState(app.basePath)
		if err != nil {
			if app.logger != nil {
				app.logger.Warn("cpu page: cannot read the active set", "err", err)
			}
		}
		app.moduleIDs_.set = s
		app.moduleIDs_.readAt = time.Now()
	}
	set = app.moduleIDs_.set
	app.moduleIDs_.mu.Unlock()

	var p paramReader
	if sp := app.params(); sp != nil {
		p = sp
	}
	slots, mfx = resolveModuleIDs(set, snap, p)
	return slots, mfx, set
}
```

- [ ] **Step 4: Update the call site in perf.go**

In `handleSystemCPUValues`, replace:

```go
		slotMods, mfxMods := app.moduleIDs()
		budget = buildFrameBudget(snap, slotMods, mfxMods)
```

with:

```go
		slotMods, mfxMods, _ = app.moduleIDs(snap)
		budget = buildFrameBudget(snap, slotMods, mfxMods)
```

and declare `var slotMods [perfChainSlots]moduleID`, `var mfxMods [perfMasterFXSlots]moduleID` above the `if`, plus `var setState *SetState` (assigned from the third return; Task 5 consumes it).

`buildFrameBudget` currently takes `map[int]moduleID`. Change its signature to arrays to match:

```go
func buildFrameBudget(snap *PerfSnapshot, slotModules [perfChainSlots]moduleID,
	mfxModules [perfMasterFXSlots]moduleID) []FrameBudgetRow {
```

and inside, replace `slotModules[i]` / `mfxModules[i]` map lookups with direct indexing (the array zero value is `moduleID{}`, i.e. `Answered: false`, which the existing `label` helper already renders as `(name unread)` — the correct behaviour when identity is unknown).

Update `perf_test.go`'s two `buildFrameBudget` callers to pass arrays:

```go
	var mods [perfChainSlots]moduleID
	mods[0] = moduleID{Name: "braids", Answered: true}
	rows := buildFrameBudget(snap, mods, [perfMasterFXSlots]moduleID{})
```

and in `TestCPUFrameBudgetShowsASlotWhoseNameCouldNotBeRead`:

```go
	var mods [perfChainSlots]moduleID
	mods[0] = moduleID{Name: "", Answered: false} // the read failed
	mods[1] = moduleID{Name: "", Answered: true}  // genuinely empty
	rows := buildFrameBudget(snap, mods, [perfMasterFXSlots]moduleID{})
```

- [ ] **Step 5: Verify**

```bash
cd .../schwung-manager && go build ./... && go vet ./... && go test ./... 2>&1 | tail -3
```
Expected: builds clean, all tests PASS.

- [ ] **Step 6: Prove the guard can fail**

Remove the `snapSlotBusy(snap, i)` condition so the fallback always fires; confirm `TestFreshnessNoFallbackForAGenuinelyEmptySlot` FAILS with a nonzero read count. Restore, confirm PASS.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/perf_modules.go schwung-manager/perf_modules_test.go \
        schwung-manager/perf.go schwung-manager/perf_test.go
git commit -m "manager: name positions from disk, param read only on disagreement

Steady state now costs zero param reads. The only fallback is a genuine
contradiction - disk calls a position empty while the snapshot shows time
for it - which is exactly the hot-swap window where disk is stale."
```

```json:metadata
{"files": ["schwung-manager/perf_modules.go", "schwung-manager/perf_modules_test.go", "schwung-manager/perf.go", "schwung-manager/perf_test.go"], "verifyCommand": "cd schwung-manager && go build ./... && go test ./... -run 'TestModules|TestFreshness' -v", "acceptanceCriteria": ["a poll with populated disk issues zero param reads", "disk-empty plus nonzero timing issues exactly one read for that position", "a failed disk read leaves every position Answered=false", "go build succeeds"], "modelTier": "standard"}
```

---

### Task 4: Find the forked children

**Goal:** `perf_forks.go` walks MoveOriginal's descendants recursively and returns the module-forked ones, excluding the helpers the shim spawns.

**Files:**
- Create: `schwung-manager/perf_forks.go`
- Create: `schwung-manager/perf_forks_test.go`
- Modify: `schwung-manager/perf_proc.go` (add `PPid` to `ProcStat`)

**Acceptance Criteria:**
- [ ] `ProcStat` carries `PPid` (proc(5) field 4).
- [ ] The walk is **recursive** — a grandchild of MoveOriginal is found.
- [ ] `display-server`, `schwung-manager`, `link-subscriber` and `shadow_ui` are excluded, **and so are their descendants**.
- [ ] MoveOriginal itself is excluded.
- [ ] A child named `MoveOriginal` or `Audio Main/SPI` IS included — attribution never uses names.
- [ ] A cycle or self-parent in the input cannot loop forever.

**Verify:** `cd schwung-manager && go test ./... -run TestForks -v` → all PASS

**Steps:**

- [ ] **Step 1: Add PPid to the parser**

In `schwung-manager/perf_proc.go`, add `fieldPPid = 4` beside the other field constants, add `PPid int` to `ProcStat`, and set it in the field switch:

```go
		case fieldPPid:
			p.PPid, _ = strconv.Atoi(tok)
```

Extend `makeStatLine` in `perf_proc_test.go` to place a ppid at field 4, and add an assertion to `TestParseProcStatCommWithSpace` that `PPid` parses.

- [ ] **Step 2: Write the failing tests**

Create `schwung-manager/perf_forks_test.go`:

```go
package main

import "testing"

// The device's real tree, plus the JP-8000 shape. Note 975: a forked PROCESS
// wearing the SPI thread's name, observed on hardware. Attribution here must
// never key on names, because a fork inherits its parent's comm and JP-8000
// never calls prctl(PR_SET_NAME).
func deviceTree() []ProcStat {
	return []ProcStat{
		{PID: 785, Comm: "init", PPid: 1},
		{PID: 917, Comm: "MoveOriginal", PPid: 785},
		{PID: 927, Comm: "display-server", PPid: 917},
		{PID: 934, Comm: "schwung-manager", PPid: 917},
		{PID: 973, Comm: "link-subscriber", PPid: 917},
		{PID: 981, Comm: "shadow_ui", PPid: 917},
		{PID: 975, Comm: "Audio Main/SPI", PPid: 917}, // module fork
		// JP-8000: child of the callback, which forks per pipeline stage.
		{PID: 1200, Comm: "MoveOriginal", PPid: 917, CPU: 0},
		{PID: 1201, Comm: "MoveOriginal", PPid: 1200, CPU: 1},
		{PID: 1202, Comm: "MoveOriginal", PPid: 1200, CPU: 2},
		// A helper's own child must not be counted as a module fork.
		{PID: 1300, Comm: "sh", PPid: 934},
	}
}

func pids(ps []ProcStat) map[int]bool {
	out := map[int]bool{}
	for _, p := range ps {
		out[p.PID] = true
	}
	return out
}

func TestForksFindsGrandchildren(t *testing.T) {
	got := pids(findForkedChildren(deviceTree(), 917))
	for _, want := range []int{975, 1200, 1201, 1202} {
		if !got[want] {
			t.Errorf("pid %d must be reported - JP-8000's stage workers are "+
				"GRANDCHILDREN, so a one-level scan misses the actual DSP", want)
		}
	}
}

func TestForksExcludesHelpersAndTheirChildren(t *testing.T) {
	got := pids(findForkedChildren(deviceTree(), 917))
	for _, notWant := range []int{917, 927, 934, 973, 981, 1300} {
		if got[notWant] {
			t.Errorf("pid %d must NOT be reported (MoveOriginal itself, a shim "+
				"helper, or a helper's child)", notWant)
		}
	}
}

// A fork inherits comm. If attribution ever keys on the name, every one of
// these disappears and the page under-reports by exactly the amount that
// matters.
func TestForksDoesNotFilterByName(t *testing.T) {
	got := pids(findForkedChildren(deviceTree(), 917))
	if !got[1200] {
		t.Fatal("a child named MoveOriginal must still be reported")
	}
	if !got[975] {
		t.Fatal("a child named Audio Main/SPI must still be reported")
	}
}

func TestForksTerminatesOnACycle(t *testing.T) {
	cyclic := []ProcStat{
		{PID: 917, Comm: "MoveOriginal", PPid: 785},
		{PID: 10, Comm: "a", PPid: 11},
		{PID: 11, Comm: "b", PPid: 10}, // mutual parents
		{PID: 12, Comm: "c", PPid: 12}, // self-parent
	}
	done := make(chan struct{})
	go func() { findForkedChildren(cyclic, 917); close(done) }()
	select {
	case <-done:
	case <-timeoutAfterOneSecond():
		t.Fatal("findForkedChildren looped on a cyclic tree")
	}
}
```

Add the tiny helper at the bottom of the same file:

```go
func timeoutAfterOneSecond() <-chan struct{} {
	ch := make(chan struct{})
	go func() { time.Sleep(time.Second); close(ch) }()
	return ch
}
```
(import `time`).

- [ ] **Step 3: Run to verify failure**

```bash
cd .../schwung-manager && go test ./... -run TestForks 2>&1 | head -5
```
Expected: FAIL — `undefined: findForkedChildren`.

- [ ] **Step 4: Write perf_forks.go**

```go
package main

// Finding the processes a MODULE forked.
//
// Module entry points ARE the SPI callback, so a module that forks does it from
// inside MoveOriginal, and the children are MoveOriginal's descendants. JP-8000
// is the case that forced this: create_instance forks a child, and that child
// forks again once per pipeline stage, pinning each to a core. The work is on
// cores 0-2 while the frame-budget row shows only the dispatch left behind in
// the callback - about 5%, which is true and deeply misleading.
//
// NOTHING HERE MATCHES ON NAMES. A fork inherits its parent's comm and JP-8000
// never calls prctl(PR_SET_NAME), so its children report as "MoveOriginal", and
// on the device one reported as "Audio Main/SPI" - indistinguishable from the
// real SPI thread to anything that reads a name. The discriminator is the
// TREE: subtract the small, known set of helpers the shim itself spawns, and
// whatever is left was forked by a module.

// shimHelpers are the processes the shim starts. They and their descendants are
// not module forks. Kept short deliberately: anything not on this list shows up
// as module-forked, so a new helper announces itself as a wrong row rather than
// silently swallowing a real one.
var shimHelpers = map[string]bool{
	"display-server":  true,
	"schwung-manager": true,
	"link-subscriber": true,
	"shadow_ui":       true,
}

// findForkedChildren returns every descendant of movePID that a module forked.
//
// The walk is breadth-first from movePID, pruning at any helper, and it visits
// each pid at most once so a malformed tree cannot loop.
func findForkedChildren(all []ProcStat, movePID int) []ProcStat {
	byParent := make(map[int][]ProcStat, len(all))
	for _, p := range all {
		if p.PID == p.PPid {
			continue // self-parent: not a real edge
		}
		byParent[p.PPid] = append(byParent[p.PPid], p)
	}

	seen := map[int]bool{movePID: true}
	queue := []int{movePID}
	var out []ProcStat

	for len(queue) > 0 {
		parent := queue[0]
		queue = queue[1:]
		for _, child := range byParent[parent] {
			if seen[child.PID] {
				continue
			}
			seen[child.PID] = true
			// Prune at a helper: it and everything below it is ours, not a
			// module's.
			if shimHelpers[child.Comm] {
				continue
			}
			out = append(out, child)
			queue = append(queue, child.PID)
		}
	}
	return out
}

// findMoveOriginal returns the pid of the MoveOriginal process, or 0.
//
// The TOP-LEVEL one, by lowest pid: a forked child inherits the name, so
// matching the name alone can find a child and root the walk in the wrong
// place.
func findMoveOriginal(all []ProcStat) int {
	best := 0
	for _, p := range all {
		if p.Comm != "MoveOriginal" {
			continue
		}
		if best == 0 || p.PID < best {
			best = p.PID
		}
	}
	return best
}
```

- [ ] **Step 5: Verify and prove a test can fail**

```bash
cd .../schwung-manager && go test ./... -run TestForks -v 2>&1 | grep -E "^(--- |ok|FAIL)"
```
Expected: all PASS.

Mutation: make the walk one level deep (don't append to `queue`), re-run, confirm `TestForksFindsGrandchildren` FAILS. Restore, confirm PASS. Report both.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/perf_forks.go schwung-manager/perf_forks_test.go \
        schwung-manager/perf_proc.go schwung-manager/perf_proc_test.go
git commit -m "manager: find module-forked processes by tree, never by name

A fork inherits its parent's comm, so JP-8000's children report as
MoveOriginal and one on the device reported as 'Audio Main/SPI'. The
discriminator is the process tree minus the four helpers the shim spawns.
The walk is recursive because the stage workers are grandchildren."
```

```json:metadata
{"files": ["schwung-manager/perf_forks.go", "schwung-manager/perf_forks_test.go", "schwung-manager/perf_proc.go", "schwung-manager/perf_proc_test.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestForks -v", "acceptanceCriteria": ["ProcStat carries PPid from proc(5) field 4", "the walk finds grandchildren", "the four helpers and their descendants are excluded", "MoveOriginal itself is excluded", "a child named MoveOriginal or Audio Main/SPI is still included", "a cyclic tree terminates"], "modelTier": "standard"}
```

---

### Task 5: Attribute the children to a module

**Goal:** Forked processes are grouped and named — declared, else inferred and marked, else unattributed.

**Files:**
- Modify: `schwung-manager/perf_forks.go` (attribution)
- Modify: `schwung-manager/perf_forks_test.go`
- Modify: `schwung-manager/perf.go` (build the panel data)

**Acceptance Criteria:**
- [ ] A module declaring `capabilities.forks_processes` claims the children; `Inferred` is false.
- [ ] With nothing declared and exactly one synth loaded, the children are attributed to it with `Inferred: true`.
- [ ] With nothing declared and two synths loaded, the group is unattributed (`Module == ""`) — no guess.
- [ ] With nothing declared and no synth loaded, the group is unattributed.
- [ ] The group always carries total CPU% and per-PID rows, whatever the naming outcome.
- [ ] Two declaring modules produce one group each, and children are not double-counted.

**Verify:** `cd schwung-manager && go test ./... -run TestAttribute -v` → all PASS

**Steps:**

- [ ] **Step 1: Write the failing tests**

Append to `schwung-manager/perf_forks_test.go`:

```go
func kidsFor(cpu ...float64) []ForkedProc {
	out := make([]ForkedProc, 0, len(cpu))
	for i, c := range cpu {
		out = append(out, ForkedProc{PID: 1200 + i, Comm: "MoveOriginal",
			Core: i, Percent: c})
	}
	return out
}

func TestAttributeDeclaredWins(t *testing.T) {
	got := attributeForks(kidsFor(90, 80, 70),
		[]LoadedModule{{ID: "jp8000", Forks: true}, {ID: "9w9"}})
	if len(got) != 1 {
		t.Fatalf("wanted 1 group, got %d", len(got))
	}
	if got[0].Module != "jp8000" || got[0].Inferred {
		t.Fatalf("got %+v, want jp8000 declared (not inferred)", got[0])
	}
	if got[0].TotalPercent < 239 || got[0].TotalPercent > 241 {
		t.Fatalf("TotalPercent = %.1f, want 240", got[0].TotalPercent)
	}
	if len(got[0].Procs) != 3 {
		t.Fatalf("per-PID rows must survive: got %d", len(got[0].Procs))
	}
}

// Inference is allowed, but it MUST be marked. An unlabelled guess on a CPU
// page is worse than no answer.
func TestAttributeInferredIsMarked(t *testing.T) {
	got := attributeForks(kidsFor(50), []LoadedModule{{ID: "jp8000"}})
	if len(got) != 1 || got[0].Module != "jp8000" {
		t.Fatalf("got %+v, want jp8000", got)
	}
	if !got[0].Inferred {
		t.Fatal("a guess must be flagged as inferred")
	}
}

// The device has jp8000 AND 9w9 loaded. With nothing declared there is no
// honest way to choose, so do not.
func TestAttributeAmbiguousIsUnattributed(t *testing.T) {
	got := attributeForks(kidsFor(50), []LoadedModule{{ID: "jp8000"}, {ID: "9w9"}})
	if len(got) != 1 {
		t.Fatalf("wanted 1 group, got %d", len(got))
	}
	if got[0].Module != "" {
		t.Fatalf("Module = %q, want unattributed - two candidates is not a guess "+
			"worth making", got[0].Module)
	}
	if got[0].TotalPercent < 49 || got[0].TotalPercent > 51 {
		t.Fatal("an unattributed group must still report its real CPU")
	}
}

func TestAttributeNoModulesIsUnattributed(t *testing.T) {
	got := attributeForks(kidsFor(50), nil)
	if len(got) != 1 || got[0].Module != "" {
		t.Fatalf("got %+v, want a single unattributed group", got)
	}
}

func TestAttributeNoChildrenIsNoGroups(t *testing.T) {
	if got := attributeForks(nil, []LoadedModule{{ID: "jp8000", Forks: true}}); len(got) != 0 {
		t.Fatalf("no children means no groups, got %+v", got)
	}
}

// Two declared forkers cannot both own the same pids.
func TestAttributeTwoDeclarersDoNotDoubleCount(t *testing.T) {
	got := attributeForks(kidsFor(10, 20),
		[]LoadedModule{{ID: "a", Forks: true}, {ID: "b", Forks: true}})
	total := 0
	for _, g := range got {
		total += len(g.Procs)
	}
	if total != 2 {
		t.Fatalf("2 children spread over %d group entries - children must not be "+
			"counted twice", total)
	}
}
```

- [ ] **Step 2: Run to verify failure**

Expected: FAIL — `undefined: attributeForks`.

- [ ] **Step 3: Write the attribution**

Append to `schwung-manager/perf_forks.go`:

```go
// ForkedProc is one module-forked process.
type ForkedProc struct {
	PID     int
	Comm    string
	Core    int
	Percent float64 // of one core, over the measured interval
}

// LoadedModule is a module currently in the rig, and whether it declares that
// it forks.
type LoadedModule struct {
	ID    string
	Forks bool
}

// ForkGroup is a set of forked processes and who we believe owns them.
//
// Module == "" means unattributed: we found the cost and will not guess at the
// owner. Inferred means we DID guess, and the page must say so.
type ForkGroup struct {
	Module       string
	Inferred     bool
	TotalPercent float64
	Procs        []ForkedProc
}

// attributeForks names forked processes, in descending order of confidence:
//
//  1. A loaded module DECLARES capabilities.forks_processes. Authoritative.
//  2. Nothing declares, and exactly one synth is loaded. Attribute to it and
//     mark it inferred - an unlabelled guess on a CPU page is worse than no
//     answer at all.
//  3. Anything else. One unattributed group, real CPU, no owner named.
//
// Whatever happens, the processes and their cost are reported. Nothing is ever
// hidden for want of a name.
func attributeForks(kids []ForkedProc, loaded []LoadedModule) []ForkGroup {
	if len(kids) == 0 {
		return nil
	}

	var declarers []string
	for _, m := range loaded {
		if m.Forks && m.ID != "" {
			declarers = append(declarers, m.ID)
		}
	}

	total := func(ps []ForkedProc) float64 {
		var t float64
		for _, p := range ps {
			t += p.Percent
		}
		return t
	}

	switch {
	case len(declarers) == 1:
		return []ForkGroup{{Module: declarers[0], TotalPercent: total(kids), Procs: kids}}

	case len(declarers) > 1:
		// We cannot tell which declarer owns which pid - the tree says only
		// that a module forked them. Split the pids evenly across groups so
		// nothing is double-counted, and name each group.
		groups := make([]ForkGroup, len(declarers))
		for i, id := range declarers {
			groups[i].Module = id
		}
		for i, k := range kids {
			g := &groups[i%len(groups)]
			g.Procs = append(g.Procs, k)
		}
		for i := range groups {
			groups[i].TotalPercent = total(groups[i].Procs)
			groups[i].Inferred = true // the SPLIT is a guess, even if the names are not
		}
		return groups
	}

	// Nothing declared. Infer only when there is exactly one candidate.
	if len(loaded) == 1 && loaded[0].ID != "" {
		return []ForkGroup{{
			Module: loaded[0].ID, Inferred: true,
			TotalPercent: total(kids), Procs: kids,
		}}
	}
	return []ForkGroup{{TotalPercent: total(kids), Procs: kids}}
}
```

- [ ] **Step 4: Build the panel data in perf.go**

In `handleSystemCPUValues`, after the process view is built (so the CPU deltas exist), add:

```go
	// Forked children: a module's real cost when it does its DSP outside the
	// SPI callback. See perf_forks.go.
	var forkGroups []ForkGroup
	if movePID := findMoveOriginal(procs); movePID != 0 {
		pct := map[int]float64{}
		for _, r := range view.Rows {
			pct[r.PID] = r.Percent
		}
		var kids []ForkedProc
		for _, c := range findForkedChildren(procs, movePID) {
			kids = append(kids, ForkedProc{
				PID: c.PID, Comm: c.Comm, Core: c.CPU, Percent: pct[c.PID],
			})
		}
		forkGroups = attributeForks(kids, loadedModules(app.basePath, setState))
	}
```

and add to `perf_modules.go`:

```go
// loadedModules lists what is in the rig and whether each declares that it
// forks. Used only to attribute forked processes.
func loadedModules(basePath string, set *SetState) []LoadedModule {
	if set == nil {
		return nil
	}
	seen := map[string]bool{}
	var out []LoadedModule
	add := func(id string) {
		if id == "" || seen[id] {
			return
		}
		seen[id] = true
		forks, _ := moduleForksProcesses(basePath, id)
		out = append(out, LoadedModule{ID: id, Forks: forks})
	}
	for _, s := range set.Slots {
		add(s.Synth)
	}
	return out
}
```

Pass `"ForkGroups": forkGroups` to the template.

Note the deliberate narrowness: only **synths** are candidates for inference. An audio FX that forks would have to declare it, which is the right default — inference across every FX and MIDI FX in the rig would almost always be ambiguous and therefore useless.

- [ ] **Step 5: Verify + mutation**

```bash
cd .../schwung-manager && go build ./... && go test ./... -run TestAttribute -v 2>&1 | grep -E "^(--- |ok|FAIL)"
```
Expected: all PASS.

Mutation: drop the `Inferred: true` in the single-candidate branch; confirm `TestAttributeInferredIsMarked` FAILS. Restore, confirm PASS.

- [ ] **Step 6: Commit**

```bash
git add schwung-manager/perf_forks.go schwung-manager/perf_forks_test.go \
        schwung-manager/perf.go schwung-manager/perf_modules.go
git commit -m "manager: attribute forked processes - declared, inferred, or neither

Declared wins. Inference happens only with exactly one candidate and is
always marked, because an unlabelled guess on a CPU page is worse than no
answer. Everything else is one unattributed group that still shows its
real cost - nothing is hidden for want of a name."
```

```json:metadata
{"files": ["schwung-manager/perf_forks.go", "schwung-manager/perf_forks_test.go", "schwung-manager/perf.go", "schwung-manager/perf_modules.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestAttribute -v", "acceptanceCriteria": ["a declaring module claims the children with Inferred false", "one candidate and nothing declared infers and marks it", "two candidates yields an unattributed group", "no modules yields an unattributed group", "every group carries total CPU and per-PID rows", "two declarers do not double-count children"], "modelTier": "standard"}
```

---

### Task 6: Show it

**Goal:** The page has a forked-process panel, inferred attributions are visibly marked, and a forker's frame-budget row points at its children.

**Files:**
- Modify: `schwung-manager/templates/partials/cpu_values.html`
- Modify: `schwung-manager/perf.go` (cross-reference note on the budget row)
- Modify: `schwung-manager/perf_render_test.go`

**Acceptance Criteria:**
- [ ] A declared group renders the module name with **no** "inferred" wording.
- [ ] An inferred group renders a visible "inferred" marker.
- [ ] An unattributed group renders its CPU and says the owner is unknown.
- [ ] With no forked processes, the panel says so rather than rendering an empty table.
- [ ] A module with a fork group gets a note on its frame-budget row pointing down to it.
- [ ] Every bar keeps `role="progressbar"` with an `aria-label`, and the table reads without bars.

**Verify:** `cd schwung-manager && go test ./... -run TestCPUValues -v && go build ./...` → PASS

**Steps:**

- [ ] **Step 1: Write the failing render tests**

Append to `schwung-manager/perf_render_test.go`:

```go
func TestCPUValuesMarksAnInferredAttribution(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			Module: "jp8000", Inferred: true, TotalPercent: 240,
			Procs: []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	if !strings.Contains(body, "jp8000") {
		t.Error("the group should name the module it guessed")
	}
	if !strings.Contains(strings.ToLower(body), "inferred") {
		t.Error("a guess MUST be visibly marked - an unlabelled guess on a CPU " +
			"page is worse than no answer")
	}
}

func TestCPUValuesDeclaredIsNotMarkedInferred(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			Module: "jp8000", Inferred: false, TotalPercent: 240,
			Procs: []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	if strings.Contains(strings.ToLower(body), "inferred") {
		t.Error("a declared attribution must not be hedged as inferred")
	}
}

func TestCPUValuesUnattributedStillShowsCost(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{
		"ForkGroups": []ForkGroup{{
			TotalPercent: 240,
			Procs:        []ForkedProc{{PID: 1200, Comm: "MoveOriginal", Core: 1, Percent: 240}},
		}},
	}))
	if !strings.Contains(body, "240") {
		t.Error("an unattributed group must still report its real CPU - nothing " +
			"is hidden for want of a name")
	}
	if !strings.Contains(body, "1200") {
		t.Error("per-PID rows must render even with no owner")
	}
}

func TestCPUValuesNoForksSaysSo(t *testing.T) {
	body := renderCPUValues(t, cpuValuesFixture(map[string]any{"ForkGroups": nil}))
	if !strings.Contains(strings.ToLower(body), "no forked") {
		t.Error("with no forked processes the panel must say so, not render an " +
			"empty table")
	}
}
```

Add the fixture helper beside `renderCPUValues`, and update the two existing render tests to use it so the key set lives in one place:

```go
// cpuValuesFixture is a complete, successful data set; overrides replace keys.
// One place to add a key when the template gains one, so a new key cannot make
// three tests silently render half a page.
func cpuValuesFixture(overrides map[string]any) map[string]any {
	d := map[string]any{
		"Budget":      []FrameBudgetRow{{Label: "Slot 1 synth", Module: "jp8000", AvgUs: 134, MaxUs: 210, Percent: 5.0, MaxPct: 7.8}},
		"PerfError":   "",
		"Headroom":    FrameHeadroom{Valid: true, PeriodUs: 2679, WorkAvgUs: 344, WorkMaxUs: 537, UsedPct: 12.8, UsedMaxPct: 20.0, FreePct: 87.2, IoctlAvgUs: 2306},
		"Snapshot":    &PerfSnapshot{SampleWindowFrames: 1000},
		"Process":     ProcessView{Rows: []ProcessRow{{PID: 42, Comm: "MoveOriginal", Percent: 61.5}}},
		"ProcessOK":   true,
		"Cores":       []CoreRow{{Name: "cpu3", Percent: 18.4, IsSPI: true}},
		"CoresOK":     true,
		"CorePriming": false,
		"Load1":       1.25, "Load5": 1.1, "Load15": 0.9,
		"LoadOK":      true,
		"RTThreads":   []ProcStat{{PID: 900, Comm: "Audio Main/SPI", Policy: schedFIFO, RTPrio: 70, CPU: 3}},
		"MoveFound":   true,
		"ForkGroups":  nil,
		"SPICore":     3,
		"Measuring":   true,
	}
	for k, v := range overrides {
		d[k] = v
	}
	return d
}
```

- [ ] **Step 2: Run to verify failure**

Expected: FAIL — the fork panel does not exist, so "inferred" and "no forked" are absent.

- [ ] **Step 3: Add the panel**

In `templates/partials/cpu_values.html`, insert after the Processes section:

```html
    <section class="info-card">
        <h2>Forked by modules &mdash; cores 0&ndash;3</h2>
        <p class="muted">
            Processes a module forked out of the SPI callback. Their cost does
            <strong>not</strong> appear in the frame budget above &mdash; that
            table measures time spent inside the callback, and this is the work
            that left it.
        </p>
        {{if not .ForkGroups}}
            <p class="muted">No forked processes.</p>
        {{else}}
            {{range .ForkGroups}}
            <h3>
                {{if .Module}}{{.Module}}{{else}}Unattributed{{end}}
                &mdash; {{printf "%.1f" .TotalPercent}}% of one core
                {{if .Inferred}}
                    <span class="muted">(inferred &mdash; no module declares
                    <code>forks_processes</code>, and this was the only
                    candidate)</span>
                {{end}}
            </h3>
            {{if not .Module}}
                <p class="muted">
                    These processes were forked by a module, but more than one
                    could be responsible, so no owner is named. The cost is real
                    either way.
                </p>
            {{end}}
            <div class="table-scroll">
            <table class="data-table">
                <caption class="sr-only">Forked processes and their CPU</caption>
                <thead>
                    <tr>
                        <th scope="col">PID</th>
                        <th scope="col">Name</th>
                        <th scope="col">Core</th>
                        <th scope="col">% of one core</th>
                    </tr>
                </thead>
                <tbody>
                {{range .Procs}}
                    <tr>
                        <th scope="row">{{.PID}}</th>
                        <td>
                            {{.Comm}}
                            <br><small class="muted">name inherited from the
                            parent &mdash; not meaningful</small>
                        </td>
                        <td>{{.Core}}</td>
                        <td>
                            <div class="progress-bar" role="progressbar"
                                 aria-valuenow="{{printf "%.0f" .Percent}}"
                                 aria-valuemin="0" aria-valuemax="100"
                                 aria-label="pid {{.PID}}: {{printf "%.1f" .Percent}} percent of one core">
                                <div class="progress-fill{{if gt .Percent 85.0}} progress-danger{{else if gt .Percent 60.0}} progress-warning{{end}}"
                                     style="width: {{printf "%.1f" .Percent}}%">{{printf "%.1f" .Percent}}%</div>
                            </div>
                        </td>
                    </tr>
                {{end}}
                </tbody>
            </table>
            </div>
            {{end}}
        {{end}}
    </section>
```

- [ ] **Step 4: Cross-reference the budget row**

In `buildFrameBudget`, the `add` call for a slot synth takes a `note`. In `handleSystemCPUValues`, after `forkGroups` is built, annotate matching rows:

```go
	// Point a forker's frame-budget row at its children, so the small number
	// and the real one are visibly connected rather than sitting in two tables
	// that look unrelated.
	for gi := range forkGroups {
		if forkGroups[gi].Module == "" {
			continue
		}
		for bi := range budget {
			if budget[bi].Module != forkGroups[gi].Module {
				continue
			}
			budget[bi].Note = "This module also runs " +
				strconv.Itoa(len(forkGroups[gi].Procs)) +
				" forked process(es) on other cores — see “Forked by modules” below. " +
				"This row is only its cost inside the SPI callback."
		}
	}
```

- [ ] **Step 5: Verify**

```bash
cd .../schwung-manager && gofmt -l . | grep -Ev "module_config|repair_status|shmconfig|shmwebring"
go build ./... && go vet ./... && go test ./... 2>&1 | tail -3
```
Expected: nothing from gofmt, clean build, all PASS.

- [ ] **Step 6: Prove the inferred marker can fail**

Remove the `{{if .Inferred}}` block from the template; confirm `TestCPUValuesMarksAnInferredAttribution` FAILS. Restore, confirm PASS.

- [ ] **Step 7: Commit**

```bash
git add schwung-manager/templates/partials/cpu_values.html \
        schwung-manager/perf.go schwung-manager/perf_render_test.go
git commit -m "manager: show forked processes, and mark an inferred owner

A fork-parallel module's real cost now has a panel, and its frame-budget
row says that it understates and points down to the children. An inferred
attribution is visibly marked; an unattributed group still shows its cost."
```

```json:metadata
{"files": ["schwung-manager/templates/partials/cpu_values.html", "schwung-manager/perf.go", "schwung-manager/perf_render_test.go"], "verifyCommand": "cd schwung-manager && go test ./... -run TestCPUValues -v && go build ./...", "acceptanceCriteria": ["a declared group renders without inferred wording", "an inferred group renders a visible marker", "an unattributed group renders its CPU and per-PID rows", "no forked processes says so rather than an empty table", "a forker's budget row points at its children", "bars keep role=progressbar and aria-label"], "modelTier": "standard"}
```

---

### Task 7: Declare it in JP-8000, and document

**Goal:** JP-8000 declares `capabilities.forks_processes`, and the docs record what this page can and cannot attribute.

**Files:**
- Modify: `../schwung-jp8000/src/module.json` — **a DIFFERENT REPO**
- Modify: `docs/DIAGNOSTICS.md`
- Modify: `docs/MODULES.md` (document the capability)
- Modify: `CLAUDE.md` (one bullet)

**Acceptance Criteria:**
- [ ] `schwung-jp8000/src/module.json` has `capabilities.forks_processes: true`, committed **on its own branch in that repo** with its own PR.
- [ ] `docs/MODULES.md` documents `capabilities.forks_processes`: what it means, that it is metadata only, and that omitting it means the module's forked cost shows as unattributed rather than vanishing.
- [ ] `docs/DIAGNOSTICS.md` gains a subsection on cross-core attribution, including that names are useless and why.
- [ ] `CLAUDE.md` gains **one** bullet under the DIAGNOSTICS hook — no prose inlined.

**Verify:** `grep -c forks_processes docs/MODULES.md docs/DIAGNOSTICS.md ../schwung-jp8000/src/module.json` → all non-zero

**Steps:**

- [ ] **Step 1: Declare it in JP-8000**

**This is a separate repository.** Do not commit it on `manager-cpu-view`.

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-jp8000
git checkout -b declare-forks-processes
```

Add to `src/module.json`'s `capabilities` object:

```json
    "forks_processes": true
```

Commit:

```bash
git add src/module.json
git commit -m "declare capabilities.forks_processes

JE-8086 forks from create_instance and again per pipeline stage, so its
DSP runs in child processes on other cores. Schwung's CPU page uses this
flag to attribute those processes to this module; without it their cost
shows up as unattributed."
```

Do **not** push or open a PR without asking — it is the user's repo and a separate release.

- [ ] **Step 2: Document the capability in MODULES.md**

Add to the capabilities table/section in `docs/MODULES.md`:

```markdown
### `capabilities.forks_processes`

Set `true` when your module creates child **processes** (not threads) to do its
DSP — as JE-8086 does, forking from `create_instance` and again per pipeline
stage.

It is metadata only; nothing about your module changes. It tells the CPU page
(`/system/cpu`) which module owns the forked processes it finds under
MoveOriginal, because **it cannot work that out from their names**: a fork
inherits its parent's `comm`, so your children report as `MoveOriginal` (or
worse, `Audio Main/SPI`) and are indistinguishable from the host's own threads.

Omitting it does not hide your module's cost — the processes still appear, with
their real CPU, as "Unattributed". The flag only lets the page say they are
yours.
```

- [ ] **Step 3: Extend the DIAGNOSTICS section**

Append to the CPU page section of `docs/DIAGNOSTICS.md`:

```markdown
### Cross-core attribution, and why names are useless

The frame budget measures time **inside the SPI callback**. A module that forks
child processes to do its DSP therefore shows only its dispatch-and-collect
cost there — JE-8086 read ~5% while its actual work ran on cores 0-2.

Those children cannot be identified by name. A fork inherits its parent's
`comm`, and JP-8000 never calls `prctl(PR_SET_NAME)`; observed on the device,
one of them reported as **`Audio Main/SPI`**, which is also what six real
MoveOriginal threads are called. So the discriminator is the **process tree**:
walk MoveOriginal's descendants recursively (the stage workers are
grandchildren, so one level is not enough), subtract the four helpers the shim
spawns — `display-server`, `schwung-manager`, `link-subscriber`, `shadow_ui` —
and what remains was forked by a module.

Ownership is then, in order: a module declaring `capabilities.forks_processes`;
failing that, inference when exactly one synth is loaded, **always marked as
inferred on screen**; failing that, an unattributed group. A process is never
hidden because we cannot name it.

### Identity comes from disk

Naming positions over the param channel cost 12 SPI-served requests per refresh.
It is read from the set state instead, and the schema is not what you would
guess — every one of these was got wrong once before being checked:

    active_set.txt     line 1 = set uuid    NOT the newest mtime (27 sets existed;
                                            mtime and glob order each chose a
                                            different wrong one)
    slot_N.json        chain.synth.module   NOT synth.module
                       chain.audio_fx[].type   "type", not "module"
    master_fx_N.json   module_id            a different key again

A param read happens only on a contradiction: disk calls a position empty while
the snapshot shows time for it, i.e. the hot-swap window where disk is stale.
```

- [ ] **Step 4: One CLAUDE.md bullet**

Add after the existing CPU-page bullet under the `docs/DIAGNOSTICS.md` hook:

```markdown
- **A fork-parallel module hides from the frame budget, and names cannot find
  it.** Module DSP that runs in forked child processes (JE-8086) shows only its
  dispatch cost in the SPI-callback timing; the children inherit MoveOriginal's
  `comm` — one reported as `Audio Main/SPI` — so attribution walks the process
  TREE minus the four shim helpers, and ownership comes from
  `capabilities.forks_processes`, else a marked inference, else unattributed.
  Module identity is read from **disk** (`active_set.txt` →
  `chain.synth.module`), not the param channel.
```

- [ ] **Step 5: Verify**

```bash
cd /Volumes/.../manager-cpu-view
grep -c forks_processes docs/MODULES.md docs/DIAGNOSTICS.md CLAUDE.md
make -C tests/host test 2>&1 | tail -2
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
(cd schwung-manager && go test ./... 2>&1 | tail -2)
```
Expected: all counts non-zero, host tests green, no FAIL lines, Go tests PASS.

- [ ] **Step 6: Commit**

```bash
git add docs/DIAGNOSTICS.md docs/MODULES.md CLAUDE.md
git commit -m "docs: cross-core attribution, and the disk schema that surprised us"
```

```json:metadata
{"files": ["../schwung-jp8000/src/module.json", "docs/DIAGNOSTICS.md", "docs/MODULES.md", "CLAUDE.md"], "verifyCommand": "grep -c forks_processes docs/MODULES.md docs/DIAGNOSTICS.md CLAUDE.md", "acceptanceCriteria": ["jp8000 module.json declares forks_processes on its own branch in its own repo", "MODULES.md documents the capability and that omitting it does not hide cost", "DIAGNOSTICS.md covers tree-not-names and the disk schema", "CLAUDE.md gains exactly one bullet with no prose inlined"], "modelTier": "standard"}
```

---

## Deploy and verify on hardware

Not a task, because it needs the device and the user's say-so. When they ask:

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Then, with JP-8000 loaded in slot 0, the page should show `jp8000` named on its
budget row (not `(name unread)`), a note that it understates, and a **Forked by
modules** group naming `jp8000` with its children on cores 0-2. Before the
JP-8000 repo change lands, that group will read *Unattributed* — with `9w9` also
loaded there are two candidates, so inference correctly declines to guess. That
is the expected intermediate state, not a bug.
