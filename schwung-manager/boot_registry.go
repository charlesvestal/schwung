// schwung-manager/boot_registry.go
package main

import (
	"fmt"
	"os"
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

// writeRegistryEntry writes <dir>/<id>/boot.json FLAT: string values only,
// no nesting, one field per line.
//
// What the selector's readers can survive was MEASURED, not assumed
// (2026-09-09, tests/host/test_boot_target_manager_json.sh). bt_json_field
// (awk, src/host/boot_target_lib.sh) is a text matcher, and two of its rules
// bite:
//
//   - VALUES MUST BE QUOTED STRINGS. `"version": 3` reads back as EMPTY --
//     the pattern requires a quote after the colon. So even a version number
//     is written as a string here.
//   - THE FIRST TEXTUAL OCCURRENCE WINS, so a nested object carrying the same
//     key SHADOWS the real one. Flat is what keeps that impossible.
//
// One field per line is not itself load-bearing (a single-line object reads
// back fine) -- it is the shape shim-entrypoint.sh already writes for the
// "schwung" entry, kept so both producers look alike.
//
// A violation is not a parse error on the device. It is a target that is
// registered and simply does not appear in the picker, with nothing logged.
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

	// Written to a temp file in the SAME directory and renamed over the
	// target, because a rename within one filesystem is atomic and an
	// in-place write is not.
	//
	// A TORN boot.json is not a corrupt file that gets noticed and fixed: the
	// reader takes whatever fields it can parse, and `owner` is the field most
	// likely to be missing because it is written LAST. Owner == "" is the
	// sentinel for "hand-installed by a human, never touch", so a write
	// interrupted by power loss (this codebase already documents torn
	// .boot-attempt stamps as a real field occurrence) converts a
	// manager-owned entry into an IMMORTAL one — reconcile will never rewrite
	// it and never delete it, and it holds a picker slot forever with a
	// possibly-dangling exec.
	path := filepath.Join(target, "boot.json")
	tmp, err := os.CreateTemp(target, ".boot.json.*")
	if err != nil {
		return fmt.Errorf("staging %s: %w", path, err)
	}
	tmpPath := tmp.Name()
	defer func() {
		tmp.Close()
		os.Remove(tmpPath) // no-op once the rename has succeeded
	}()
	if _, err := tmp.WriteString(b.String()); err != nil {
		return fmt.Errorf("writing %s: %w", tmpPath, err)
	}
	// CreateTemp makes the file 0600; the selector reads it as ableton.
	if err := tmp.Chmod(0o644); err != nil {
		return fmt.Errorf("chmod %s: %w", tmpPath, err)
	}
	if err := tmp.Sync(); err != nil {
		return fmt.Errorf("syncing %s: %w", tmpPath, err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("closing %s: %w", tmpPath, err)
	}
	// The selector execs the target as ableton, not root; the manager runs as
	// root, so ownership has to be handed back explicitly. Done BEFORE the
	// rename so the file is never visible at `path` with the wrong owner.
	// chownToAbleton (module_config.go) is per-path, not recursive, so the
	// directory needs its own call.
	chownToAbleton(tmpPath)
	if err := os.Rename(tmpPath, path); err != nil {
		return fmt.Errorf("installing %s: %w", path, err)
	}
	chownToAbleton(target)
	return nil
}

// readRegistryEntry reads one entry, mirroring what the writer guarantees:
// flat, string values, one field per line.
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

// bootPageRows renders the registry in the order bs_build_rows (src/host/
// boot_select_core.c) produces: Schwung first, then targets ascending by id,
// Stock last. The Boot page must show the same order the device shows, or a
// user picking "the third row" on the page and at boot picks two different
// targets.
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

// bootSourceLabel names who put a row there, matching the three shapes
// registryEntry.Owner can take.
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
// back to schwung, then stock) but there is no reason to create one here —
// see spec section 3a: installing a payload can add a picker row, it can
// never cause that row to boot, and neither can this handler.
func (app *App) setBootDefault(id string) error {
	// The id arrives from a form value (main.go handleBootSetDefault) and
	// becomes a path component. filepath.Join CLEANS "..", so an unvalidated
	// "../schwung/modules/tools/evil" reads that directory's boot.json — and a
	// module tarball can ship a file with that name — after which the check
	// passes and the traversal string is written verbatim into
	// boot-targets/default, where the selector's bt_resolve_default /
	// bt_exec_path run whatever it names. Every other id in this feature is
	// regexp-checked; this one was the hole.
	if id != "stock" && !bootTargetIDRe.MatchString(id) {
		return fmt.Errorf("boot target id %q: must match [a-z0-9-]+", id)
	}
	reg := bootTargetsDir()
	if id != "stock" {
		if _, err := readRegistryEntry(reg, id); err != nil {
			return fmt.Errorf("%q is not a registered boot target", id)
		}
	}
	return writeBootDefault(reg, id)
}
