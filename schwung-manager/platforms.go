// schwung-manager/platforms.go
package main

import (
	"os"
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
