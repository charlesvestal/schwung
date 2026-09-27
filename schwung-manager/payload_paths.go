package main

import (
	"os"
	"path/filepath"
)

// installSubdirs is the full range of getInstallSubdir. It is the ONE list:
// getInstallSubdir maps a component_type into it, and moduleInstallDirs
// enumerates it. Two hand-written lists is how modules/utilities and
// modules/other came to be installable but not discoverable.
var installSubdirs = []string{
	"sound_generators", "audio_fx", "midi_fx",
	"utilities", "overtake", "tools", "other",
}

// moduleInstallDirs is every directory a module can be installed into,
// including the bare modules/ root that built-ins live in. It is the ONLY
// enumeration: findModuleDir, discoverInstalledModules and
// RemoteUI.findModuleWebUI all read it. Each of those three used to keep its
// own copy, and all three copies had drifted the same way (no "utilities", no
// "other") -- so a module installed with component_type "utility" was
// invisible to uninstall, to discovery, and to the Remote UI at once.
func moduleInstallDirs(basePath string) []string {
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
