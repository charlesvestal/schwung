// schwung-manager/platforms.go
package main

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
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
