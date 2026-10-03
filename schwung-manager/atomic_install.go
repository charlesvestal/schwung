package main

// ATOMIC INSTALL (#474).
//
// Every install path used to put new files over old ones IN PLACE: the
// catalog install and the host update ran BusyBox `tar -xzf ... -C <live
// dir>`, and BusyBox tar truncates and rewrites an existing file at the SAME
// INODE. For a dsp.so that MoveOriginal has mapped, that is a write into
// running code -- measured: an in-place copy over Multisampler's dsp.so after
// it had been loaded and unloaded crashed MoveOriginal on the spot (a C++
// module with GNU-unique symbols is never unmapped by dlclose, and a module
// whose threads outlive destroy keeps its code live too). The milder outcome
// is an update that silently does not take: dlopen of the same path returns
// the library already resident.
//
// The fix is the one from the issue: unpack into a staging directory on the
// SAME filesystem, then rename(2) each file into place. rename replaces the
// directory entry atomically and gives the new file a new inode, so a mapped
// library keeps the old one for as long as it is held; nothing is ever written
// through a live mapping.
//
// It is a MERGE, file by file, which is exactly what tar over a directory
// did: files the tarball ships replace their old copies, and everything else
// in the module directory -- ROMs, instruments, soundfonts, config.json --
// stays. (The custom installs used RemoveAll + rename instead: safe for the
// mapping, but it deleted every ROM and instrument a user had put in the
// module's folder on each reinstall. They merge now too.)

import (
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// isNativeLib reports whether a path is a shared library -- the files whose
// replacement needs a restart to take effect.
func isNativeLib(path string) bool {
	base := filepath.Base(path)
	return strings.HasSuffix(base, ".so") || strings.Contains(base, ".so.")
}

// installTreeAtomically moves every non-directory entry under staged to the
// same relative path under dest, creating directories as needed. Each file
// arrives by rename, so a replaced file gets a new inode and anything holding
// the old one (a mapped library, an open FILE*) keeps it intact. Returns the
// native libraries it REPLACED (existed before), relative to dest.
//
// A file where dest has a directory, or a directory where dest has a file, is
// an error -- tar would have failed the same way. Files already moved stay
// moved; the caller's next install repeats the merge.
func installTreeAtomically(staged, dest string) ([]string, error) {
	var replaced []string
	err := filepath.WalkDir(staged, func(path string, d fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		rel, err := filepath.Rel(staged, path)
		if err != nil {
			return err
		}
		if rel == "." {
			return os.MkdirAll(dest, 0755)
		}
		target := filepath.Join(dest, rel)
		if d.IsDir() {
			info, err := d.Info()
			if err != nil {
				return err
			}
			if st, err := os.Lstat(target); err == nil && !st.IsDir() {
				return fmt.Errorf("install: %s is a file, the update has a directory there", rel)
			}
			return os.MkdirAll(target, info.Mode().Perm()|0700)
		}
		existed := false
		if st, err := os.Lstat(target); err == nil {
			if st.IsDir() {
				return fmt.Errorf("install: %s is a directory, the update has a file there", rel)
			}
			existed = true
		}
		if err := os.Rename(path, target); err != nil {
			return fmt.Errorf("install %s: %w", rel, err)
		}
		if existed && isNativeLib(target) {
			replaced = append(replaced, rel)
		}
		return nil
	})
	return replaced, err
}

// extractTarballAtomically unpacks tarPath into a staging directory created
// inside dest (so the renames never cross a filesystem), installs the result
// into dest with installTreeAtomically, and removes the staging directory.
// tarArgs are extra tar arguments (e.g. "--strip-components=1", excludes);
// "-xzf" (or "-xzof" with ownership ignored) is supplied here.
func extractTarballAtomically(tarPath, dest string, ignoreOwner bool, tarArgs ...string) ([]string, error) {
	if err := os.MkdirAll(dest, 0755); err != nil {
		return nil, err
	}
	staging, err := os.MkdirTemp(dest, ".staging-")
	if err != nil {
		return nil, fmt.Errorf("staging dir: %w", err)
	}
	defer os.RemoveAll(staging)

	flags := "-xzf"
	if ignoreOwner {
		flags = "-xzof"
	}
	args := append([]string{flags, tarPath, "-C", staging}, tarArgs...)
	if out, err := exec.Command("tar", args...).CombinedOutput(); err != nil {
		return nil, fmt.Errorf("extracting tarball: %w\noutput: %s", err, out)
	}
	return installTreeAtomically(staging, dest)
}

// THE RESTART NOTICE (#474, part 2). Replacing a module's native library never
// touches the copy MoveOriginal already runs -- that is the point of the
// rename -- so the update takes effect when Move restarts. The manager cannot
// see which libraries are mapped (MoveOriginal carries file capabilities, so
// its /proc maps are unreadable even to its own uid), so it says so whenever
// an install REPLACED native code, which is precisely when it matters.

func (app *App) markRestartNeeded(id string) {
	app.restartMu.Lock()
	defer app.restartMu.Unlock()
	if app.restartNeeded == nil {
		app.restartNeeded = map[string]bool{}
	}
	app.restartNeeded[id] = true
}

// restartSuffix is the flash text to append for one module's install, and
// consumes the mark.
func (app *App) restartSuffix(id string) string {
	app.restartMu.Lock()
	defer app.restartMu.Unlock()
	if !app.restartNeeded[id] {
		return ""
	}
	delete(app.restartNeeded, id)
	return "+-+restart+Move+to+run+the+new+version+if+it+has+been+used+since+boot"
}

// takeAllRestartNeeded consumes every mark and returns how many there were.
func (app *App) takeAllRestartNeeded() int {
	app.restartMu.Lock()
	defer app.restartMu.Unlock()
	n := len(app.restartNeeded)
	app.restartNeeded = nil
	return n
}
