// schwung-manager/platforms.go
package main

import (
	"encoding/json"
	"fmt"
	"io/fs"
	"log/slog"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
)

// discoverInstalledPlatforms returns id -> payload directory for everything
// under the platforms root carrying a platform.json.
//
// This is only the discovery half of Task 6 (platform install/uninstall/rows
// land separately); it exists here because reconcileBootTargets needs it to
// build the desired boot-target set and must compile without the rest of
// Task 6.
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

// InstalledPlatform is a platform payload present on disk.
type InstalledPlatform struct {
	ID      string `json:"id"`
	Name    string `json:"name"`
	Version string `json:"version"`
	Author  string `json:"author"`
	Dir     string `json:"-"`
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

// extractPlatformPayload extracts a platform tarball into <root>/<id>.
//
// It extracts into a STAGING directory and renames into place only after the
// top-level <id>/platform.json is verified. Extracting straight into the
// platforms root meant a tarball whose top-level directory was `bar/` instead
// of `<id>/` failed the check and reported an install failure -- while leaving
// bar/ sitting in the root, where the next reconcile discovers it and
// registers BAR's boot_target under platform:bar: an id the user never
// installed and which is not in the catalog.
//
// The id is validated FIRST because it becomes a path. It comes from the
// catalog, not from the payload, but it is no less remote for that: an id of
// ".." resolved the manifest check to /data/UserData/platform.json and the
// extraction to the parent of the platforms root.
func extractPlatformPayload(root, id, tarballPath string) error {
	if !bootTargetIDRe.MatchString(id) {
		return fmt.Errorf("platform id %q: must match [a-z0-9-]+", id)
	}
	if err := os.MkdirAll(root, 0o755); err != nil {
		return fmt.Errorf("creating platforms dir: %w", err)
	}
	staging, err := os.MkdirTemp(root, ".staging-"+id+"-")
	if err != nil {
		return fmt.Errorf("creating staging dir: %w", err)
	}
	// Removed on EVERY failure path, so a rejected tarball leaves the
	// platforms root exactly as it found it.
	defer os.RemoveAll(staging)

	if out, err := exec.Command("tar", "-xzf", tarballPath, "-C", staging).CombinedOutput(); err != nil {
		return fmt.Errorf("extracting tarball: %w\noutput: %s", err, out)
	}
	staged := filepath.Join(staging, id)
	if _, err := os.Stat(filepath.Join(staged, "platform.json")); err != nil {
		return fmt.Errorf("the tarball must contain a top-level %s/ directory "+
			"with platform.json inside it", id)
	}

	// Rename cannot merge into an existing directory, so the old payload goes
	// first. It is moved aside rather than deleted outright: a failed rename
	// would otherwise leave no payload at all where there had been a working
	// one. (User state was snapshotted by the caller and is restored after.)
	final := filepath.Join(root, id)
	backup := ""
	if _, err := os.Stat(final); err == nil {
		backup = final + ".replacing"
		os.RemoveAll(backup)
		if err := os.Rename(final, backup); err != nil {
			return fmt.Errorf("moving the previous payload aside: %w", err)
		}
	}
	if err := os.Rename(staged, final); err != nil {
		if backup != "" {
			os.Rename(backup, final)
		}
		return fmt.Errorf("installing payload: %w", err)
	}
	if backup != "" {
		os.RemoveAll(backup)
	}
	return nil
}

// chownEach is the per-path chown, a seam so a test can observe the walk
// without needing an `ableton` user (or root) to exist.
var chownEach = chownToAbleton

// chownTreeToAbleton is the platform-side equivalent of the module path's
// `chown -R ableton:users`. docs/BOOT_TARGETS.md is explicit that a target is
// exec'd AS ABLETON: with only the payload directory itself chowned, every
// shipped file and subdirectory stays root:root and any state the platform
// writes at boot fails EACCES -- which, if the platform treats it as fatal, is
// three strikes and a forced picker with the cause invisible.
func chownTreeToAbleton(root string) {
	_ = filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return nil // a vanished entry is not a reason to stop
		}
		chownEach(path)
		return nil
	})
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
	if err := extractPlatformPayload(root, p.ID, tmpPath); err != nil {
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
	chownTreeToAbleton(payloadDir)

	// A refused boot target is NOT an install failure — the payload is on
	// disk and only its picker row is missing, with the reason logged. Same
	// rule as installModuleWithDeps; a platform whose whole point is booting
	// still installs, and the Boot page shows what went wrong.
	if err := app.reconcileBootTargets(); err != nil {
		app.logger.Warn("boot target registration", "id", p.ID, "err", err)
	}
	return nil
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

// findCatalogPlatform looks up one catalog platform by id, mirroring
// findCatalogModule.
func (app *App) findCatalogPlatform(id string) *CatalogPlatform {
	cat, _ := app.catalogSvc.Fetch()
	if cat == nil {
		return nil
	}
	for i := range cat.Platforms {
		if cat.Platforms[i].ID == id {
			return &cat.Platforms[i]
		}
	}
	return nil
}

// platformRedirect mirrors moduleRedirect, but a platform has no detail page
// to fall back to — every destination is the list.
func (app *App) platformRedirect(w http.ResponseWriter, r *http.Request, flash, kind string) {
	dest := safeReturnTo(r.FormValue("return_to"))
	if dest == "" {
		dest = "/platforms"
	}
	if i := strings.Index(dest, "?flash="); i >= 0 {
		dest = dest[:i]
	} else if i := strings.Index(dest, "&flash="); i >= 0 {
		dest = dest[:i]
	}
	sep := "?"
	if strings.Contains(dest, "?") {
		sep = "&"
	}
	dest += sep + "flash=" + flash
	if kind != "" {
		dest += "&flash_type=" + url.QueryEscape(kind)
	}
	http.Redirect(w, r, dest, http.StatusSeeOther)
}

// handlePlatforms renders the Platforms page: catalog entries merged with
// what is installed on disk, plus the update badge (same source as modules'
// — the cached release-metadata.json, not a live per-repo fetch on render).
func (app *App) handlePlatforms(w http.ResponseWriter, r *http.Request) {
	cat, err := app.catalogSvc.Fetch()
	if err != nil {
		app.logger.Warn("catalog fetch failed", "err", err)
	}
	var platforms []CatalogPlatform
	if cat != nil {
		platforms = cat.Platforms
	}
	meta := app.catalogSvc.GetReleaseMeta()
	available := make(map[string]string, len(platforms))
	for _, p := range platforms {
		if rm, ok := meta[p.ID]; ok && rm.Version != "" {
			available[p.ID] = rm.Version
		}
	}
	rows := platformRows(app.basePath, platforms, available)

	data := map[string]any{
		"Title":  "Platforms",
		"Rows":   rows,
		"Active": "platforms",
		"Flash":  r.URL.Query().Get("flash"),
	}
	app.render(w, r, "platforms.html", data)
}

func (app *App) handlePlatformInstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	p := app.findCatalogPlatform(id)
	if p == nil {
		app.platformRedirect(w, r, "Platform+not+found:+"+id, flashError)
		return
	}
	if err := app.installPlatform(p); err != nil {
		app.logger.Error("platform install failed", "id", id, "err", err)
		app.platformRedirect(w, r, "Install+failed:+"+err.Error(), flashError)
		return
	}
	app.platformRedirect(w, r, p.Name+"+installed+successfully", flashSuccess)
}

func (app *App) handlePlatformUninstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := app.uninstallPlatform(id); err != nil {
		app.logger.Error("platform uninstall failed", "id", id, "err", err)
		app.platformRedirect(w, r, "Uninstall+failed:+"+err.Error(), flashError)
		return
	}
	app.platformRedirect(w, r, "Platform+uninstalled", flashSuccess)
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
	// Warn-only: the payload is already gone, so reconcile's error would
	// report a failure for work that succeeded.
	if err := app.reconcileBootTargets(); err != nil {
		app.logger.Warn("boot target deregistration", "id", id, "err", err)
	}
	return nil
}
