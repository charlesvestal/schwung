// schwung-manager/boot_reconcile.go
package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"sync"
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

// desiredSet is what the payloads on disk say the registry should hold, plus
// the two things reconcile cannot work out from `byID` alone.
//
// installedOwners is every payload PRESENT, whether or not it declares a boot
// target. Without it, "is this entry's owner still here?" can only be answered
// by asking whether its ID is desired — which says nothing when a DIFFERENT
// payload has since claimed that id, and that gap is what left an entry with
// an exec pointing into a deleted directory that nothing could remove.
//
// unsettled maps an owner to why its claim could not be acted on this pass: a
// refused boot_target block, or an id another payload won. Neither is the
// payload retiring its block, so neither may delete the live entry.
type desiredSet struct {
	byID            map[string]desiredTarget
	installedOwners map[string]bool
	unsettled       map[string]string
}

// payloadSource is one installed payload, in the shape the walk needs.
type payloadSource struct {
	dir      string
	id       string
	manifest string
	owner    string
}

// desiredBootTargets walks every installed module and platform and returns the
// targets they declare, keyed by target id, alongside the owner bookkeeping
// described on desiredSet.
//
// A collision is reported and SKIPPED, never fatal. Returning (nil, err) on
// the first clash made every later reconcile a no-op device-wide — no
// deregistration on uninstall, no stale-exec rewrite, no default healing — and
// every call site is warn-only, so nothing surfaced.
func (app *App) desiredBootTargets() (desiredSet, error) {
	out := desiredSet{
		byID:            map[string]desiredTarget{},
		installedOwners: map[string]bool{},
		unsettled:       map[string]string{},
	}

	// Sorted by owner so a collision resolves the same way on every pass. A
	// winner decided by map order flip-flops, and each flip rewrites boot.json
	// for a target nobody touched.
	var sources []payloadSource
	for id := range discoverInstalledModules(app.basePath) {
		dir := app.findModuleDir(id)
		if dir == "" {
			continue
		}
		sources = append(sources, payloadSource{dir, id, "module.json", ownerForModule(id)})
	}
	for id, dir := range discoverInstalledPlatforms(app.basePath) {
		sources = append(sources, payloadSource{dir, id, "platform.json", ownerForPlatform(id)})
	}
	sort.Slice(sources, func(i, j int) bool { return sources[i].owner < sources[j].owner })

	var problems error
	for _, src := range sources {
		out.installedOwners[src.owner] = true
		manifest := filepath.Join(src.dir, src.manifest)
		if _, err := os.Stat(manifest); err != nil {
			continue
		}
		bt, err := parseBootTarget(manifest, src.id, src.dir)
		if err != nil {
			// A refused boot target is not a broken payload: the module still
			// works, it just gets no picker row. It is also NOT the payload
			// retiring its block — record it so reconcile keeps the entry it
			// already has instead of deleting a row (and healing the user's
			// default away) over a transient unreadable directory.
			app.logger.Warn("boot target refused", "owner", src.owner, "err", err)
			out.unsettled[src.owner] = "its boot_target block was refused"
			continue
		}
		if bt == nil {
			continue
		}
		if prev, clash := out.byID[bt.ID]; clash {
			problems = errors.Join(problems, fmt.Errorf(
				"boot target id %q is claimed by both %s and %s; %s keeps it",
				bt.ID, prev.entry.Owner, src.owner, prev.entry.Owner))
			out.unsettled[src.owner] = "another payload holds the id it declares"
			continue
		}
		out.byID[bt.ID] = desiredTarget{entry: registryEntry{
			ID:      bt.ID,
			Name:    bt.Name,
			Exec:    bt.ExecAbs,
			Version: payloadVersion(manifest),
			Owner:   src.owner,
		}}
	}
	return out, problems
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

// bootReconcileMu serializes reconcile passes.
//
// Reconcile runs at manager start and from four HTTP handlers. Two overlapping
// passes — a double-clicked install, or a module install overlapping a
// platform update — can have one read a payload directory while the other's
// `tar -xzf` is mid-flight: the manifest has landed and the entry script has
// not, so the target is refused. It also makes two writers race over the same
// boot.json and `default`. This narrows it to one pass at a time; it does NOT
// cover the install critical sections themselves, which still extract outside
// this lock.
var bootReconcileMu sync.Mutex

// bootReconcileInside is a test seam: called with the lock held so a test can
// prove passes do not overlap. nil in production.
var bootReconcileInside func()

// reconcileBootTargets makes the registry agree with what is installed.
//
// It considers ONLY entries carrying an owner it recognises. A hand-installed
// target has no owner and is never touched; the "schwung" entry is excluded
// outright because the selector rewrites it on every boot.
//
// The registry is indexed by OWNER, not by id: boot_target.id is optional and
// may differ from the payload id, so <registry>/<payload id> is the wrong door
// and silently leaves a row behind on uninstall.
//
// It writes `default` in ONE direction only: healing it to "schwung" when the
// target it named has just been deleted. A default is otherwise an explicit
// user choice, never a side effect.
//
// Errors are COLLECTED, not fatal: one payload's refusal, collision or
// overflow must not stop the pass, or a single bad module freezes the registry
// for the whole device.
func (app *App) reconcileBootTargets() error {
	bootReconcileMu.Lock()
	defer bootReconcileMu.Unlock()
	if bootReconcileInside != nil {
		bootReconcileInside()
	}

	reg := bootTargetsDir()
	if err := os.MkdirAll(reg, 0o755); err != nil {
		return fmt.Errorf("creating registry dir: %w", err)
	}

	desired, problems := app.desiredBootTargets()

	existing, err := listRegistryEntries(reg)
	if err != nil {
		return errors.Join(problems, fmt.Errorf("listing registry: %w", err))
	}

	currentDefault, _ := readBootDefault(reg)
	healDefault := false

	// 1. Delete owned entries whose payload is gone or has retired its block.
	//
	// The spec's reconcile table has exactly TWO delete rows, and both are
	// about the OWNER: the owner is absent, or the owner is installed and no
	// longer declares this target. Everything else keeps the entry.
	for _, e := range existing {
		if e.ID == "schwung" || e.Owner == "" {
			continue // never ours to touch
		}
		reason := ""
		switch {
		case !desired.installedOwners[e.Owner]:
			// The recorded owner is not installed at all. This must NOT be
			// conditioned on the id being undesired: when another payload has
			// claimed the same id, the claim check refuses to rewrite the row
			// for the newcomer, so skipping the delete here strands an entry
			// whose exec points into a deleted directory with no way to remove
			// it — and the selector stamps its boot-attempt strike before it
			// discovers the exec is missing, so three boots force the picker
			// on every boot thereafter.
			reason = "payload uninstalled"
		case desired.unsettled[e.Owner] != "":
			// Refused, or lost the id to another payload. NOT a retirement:
			// deleting here throws away the user's boot default over a bad
			// update the publisher fixes next release.
			app.logger.Warn("boot target left in place", "id", e.ID, "owner", e.Owner,
				"reason", desired.unsettled[e.Owner])
			continue
		case desired.byID[e.ID].entry.Owner == e.Owner:
			continue // still declared by this owner
		default:
			// Installed, parsed cleanly, and does not claim this id: the block
			// was retired, or the target was renamed on upgrade.
			reason = "payload no longer declares this target"
		}
		app.logger.Info("deregistering boot target", "id", e.ID, "owner", e.Owner, "reason", reason)
		if err := removeRegistryEntry(reg, e.ID); err != nil {
			return errors.Join(problems, fmt.Errorf("removing %s: %w", e.ID, err))
		}
		if currentDefault == e.ID {
			healDefault = true
		}
	}

	// 2. Create or rewrite. Sorted so a capped run is deterministic rather
	//    than map-order roulette.
	ids := make([]string, 0, len(desired.byID))
	for id := range desired.byID {
		ids = append(ids, id)
	}
	sort.Strings(ids)

	// Counted from what SURVIVED step 1 rather than predicted from `desired`,
	// so a delete and a create in the same pass cannot both hold the slot.
	surviving, err := listRegistryEntries(reg)
	if err != nil {
		return errors.Join(problems, fmt.Errorf("listing registry: %w", err))
	}
	registered := 0
	for _, e := range surviving {
		if e.ID != "schwung" {
			registered++
		}
	}

	for _, id := range ids {
		d := desired.byID[id]
		cur, err := readRegistryEntry(reg, id)
		switch {
		case err == nil && cur.Owner != d.entry.Owner && cur.Owner != "":
			problems = errors.Join(problems, fmt.Errorf(
				"boot target %q is owned by %s; %s cannot claim it",
				id, cur.Owner, d.entry.Owner))
			continue
		case err == nil && cur.Owner == "":
			problems = errors.Join(problems, fmt.Errorf(
				"boot target %q was installed by hand; %s cannot claim it",
				id, d.entry.Owner))
			continue
		case err == nil:
			if cur == d.entry {
				continue // already correct
			}
		default:
			if registered >= bootPickerTargetCap {
				problems = errors.Join(problems, fmt.Errorf(
					"boot target %q not registered: the picker holds %d targets and is full",
					id, bootPickerTargetCap))
				continue
			}
			registered++
		}
		if err := writeRegistryEntry(reg, d.entry); err != nil {
			return errors.Join(problems, fmt.Errorf("writing %s: %w", id, err))
		}
		app.logger.Info("registered boot target", "id", id, "owner", d.entry.Owner)
	}

	if healDefault {
		app.logger.Info("boot default named a removed target; healing to schwung")
		if err := writeBootDefault(reg, "schwung"); err != nil {
			return errors.Join(problems, fmt.Errorf("healing default: %w", err))
		}
	}
	return problems
}
