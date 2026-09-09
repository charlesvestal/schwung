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
	// root, so ownership has to be handed back explicitly. chownToAbleton
	// (module_config.go) is per-path, not recursive, so both the directory
	// and the file it holds need their own call.
	chownToAbleton(target)
	chownToAbleton(path)
	return nil
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
