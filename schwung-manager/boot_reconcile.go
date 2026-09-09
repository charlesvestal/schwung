// schwung-manager/boot_reconcile.go
package main

import (
	"encoding/json"
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

	for id := range discoverInstalledModules(app.basePath) {
		dir := app.findModuleDir(id)
		if dir == "" {
			continue
		}
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
	if err := json.Unmarshal(raw, &doc); err != nil {
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
