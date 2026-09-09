// schwung-manager/platforms.go
package main

import (
	"encoding/json"
	"fmt"
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
