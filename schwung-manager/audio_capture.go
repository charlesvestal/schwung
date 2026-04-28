package main

// Audio behavior diagnostic capture (boot-aware, with safety timeout).
//
// User flow (web UI on /system):
//   1. "Arm capture" → touch /data/UserData/schwung/log_xmos_sysex_on
//      and record an arm timestamp.
//   2. User reboots Move and reproduces the bug. Shim's flag-gated logger
//      writes MIDI_OUT SysEx + MIDI_IN cc=114/115 events to xmos_sysex.txt
//      starting from boot frame 0.
//   3. "Stop & download" → remove flag, allow shim's 1s flag-tick to close
//      its fd, bundle into a tarball, stream it as a download.
//
// Auto-disarm safety: if the flag stays armed longer than
// audioCaptureMaxArmedMinutes (default 5), schwung-manager removes it and
// builds the bundle automatically. Survives manager / device reboots: the
// arm timestamp lives on disk, and on manager startup we either auto-disarm
// immediately (if past the deadline) or schedule it for the remaining time.
//
// The shim's logger has its own size cap as a belt-and-suspenders safety
// net for runaway log growth.
//
// Easy to remove: delete this file, drop the audio-capture mux.HandleFunc
// lines + startupAudioCaptureWatchdog() call in main.go. The shim's
// flag-gated logger in schwung_shim.c is its own removable block.

import (
	"archive/tar"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const (
	audioCaptureFlagName        = "log_xmos_sysex_on"
	audioCaptureLogName         = "xmos_sysex.txt"
	audioCaptureArmStampName    = "log_xmos_sysex_arm_at"
	audioCaptureBundlePfx       = "schwung-audio-capture-"
	audioCaptureFlagDrainS      = 2  // seconds to wait after flag removal before bundling
	audioCaptureMaxArmedMinutes = 5  // safety: auto-disarm after this many minutes
	audioCaptureMaxBundles      = 10 // keep at most N bundles on disk
)

func (app *App) captureFlagPath() string  { return filepath.Join(app.basePath, audioCaptureFlagName) }
func (app *App) captureLogPath() string   { return filepath.Join(app.basePath, audioCaptureLogName) }
func (app *App) captureStampPath() string { return filepath.Join(app.basePath, audioCaptureArmStampName) }

// readArmTime returns the arm timestamp, or zero time if unavailable.
func (app *App) readArmTime() time.Time {
	data, err := os.ReadFile(app.captureStampPath())
	if err != nil {
		return time.Time{}
	}
	t, err := time.Parse(time.RFC3339, strings.TrimSpace(string(data)))
	if err != nil {
		return time.Time{}
	}
	return t
}

// writeArmTime writes the current time as the arm timestamp.
func (app *App) writeArmTime(t time.Time) error {
	return os.WriteFile(app.captureStampPath(), []byte(t.UTC().Format(time.RFC3339)), 0644)
}

// latestCaptureBundle returns the name of the most recent capture tarball
// in basePath, or "" if none exists.
func (app *App) latestCaptureBundle() string {
	entries, err := os.ReadDir(app.basePath)
	if err != nil {
		return ""
	}
	var matches []os.DirEntry
	for _, e := range entries {
		if !e.IsDir() && strings.HasPrefix(e.Name(), audioCaptureBundlePfx) && strings.HasSuffix(e.Name(), ".tgz") {
			matches = append(matches, e)
		}
	}
	if len(matches) == 0 {
		return ""
	}
	sort.Slice(matches, func(i, j int) bool { return matches[i].Name() > matches[j].Name() })
	return matches[0].Name()
}

// trimOldCaptureBundles deletes bundles beyond audioCaptureMaxBundles, oldest
// first, so the disk doesn't fill up after many capture sessions.
func (app *App) trimOldCaptureBundles() {
	entries, err := os.ReadDir(app.basePath)
	if err != nil {
		return
	}
	var matches []string
	for _, e := range entries {
		if !e.IsDir() && strings.HasPrefix(e.Name(), audioCaptureBundlePfx) && strings.HasSuffix(e.Name(), ".tgz") {
			matches = append(matches, e.Name())
		}
	}
	if len(matches) <= audioCaptureMaxBundles {
		return
	}
	sort.Strings(matches) // oldest stamp sorts first
	excess := len(matches) - audioCaptureMaxBundles
	for i := 0; i < excess; i++ {
		_ = os.Remove(filepath.Join(app.basePath, matches[i]))
	}
}

// handleAudioCaptureArm creates the flag file so the shim begins logging
// on the next SPI tick. Persists across reboots. Schedules auto-disarm.
func (app *App) handleAudioCaptureArm(w http.ResponseWriter, r *http.Request) {
	_ = os.Remove(app.captureLogPath())
	if err := os.WriteFile(app.captureFlagPath(), []byte("1"), 0644); err != nil {
		app.logger.Error("audio capture: arm", "err", err)
		http.Error(w, "failed to arm: "+err.Error(), http.StatusInternalServerError)
		return
	}
	if err := app.writeArmTime(time.Now()); err != nil {
		app.logger.Warn("audio capture: write arm timestamp", "err", err)
	}
	app.logger.Info("audio capture armed", "auto_disarm_minutes", audioCaptureMaxArmedMinutes)
	go app.scheduleAudioCaptureAutoDisarm(time.Duration(audioCaptureMaxArmedMinutes) * time.Minute)

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"state":               "armed",
		"auto_disarm_minutes": audioCaptureMaxArmedMinutes,
	})
}

// handleAudioCaptureStop removes the flag, builds the bundle, returns
// JSON describing it. Caller GETs /download next.
func (app *App) handleAudioCaptureStop(w http.ResponseWriter, r *http.Request) {
	bundleName, err := app.disarmAndBundle("user")
	if err != nil {
		http.Error(w, "failed to build bundle: "+err.Error(), http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"state":       "idle",
		"bundle_name": bundleName,
	})
}

// disarmAndBundle removes the flag, drains the shim's fd, bundles, and
// trims old bundles. Returns the new bundle filename. Source is logged
// ("user", "auto", "startup") so we can tell automatic disarms apart.
func (app *App) disarmAndBundle(source string) (string, error) {
	if err := os.Remove(app.captureFlagPath()); err != nil && !os.IsNotExist(err) {
		app.logger.Warn("audio capture: stop remove flag", "err", err, "source", source)
	}
	_ = os.Remove(app.captureStampPath())
	time.Sleep(audioCaptureFlagDrainS * time.Second)

	stamp := time.Now().UTC().Format("20060102-150405")
	bundleName := audioCaptureBundlePfx + stamp + ".tgz"
	bundlePath := filepath.Join(app.basePath, bundleName)
	if err := app.buildAudioCaptureBundle(bundlePath); err != nil {
		app.logger.Error("audio capture: build bundle", "err", err, "source", source)
		return "", err
	}
	app.trimOldCaptureBundles()
	app.logger.Info("audio capture bundle ready", "bundle", bundleName, "source", source)
	return bundleName, nil
}

// scheduleAudioCaptureAutoDisarm waits the given duration, then auto-disarms
// only if the flag is still set and the on-disk timestamp matches the value
// at the time we were scheduled. This avoids racing a user-initiated stop
// or a re-arm.
func (app *App) scheduleAudioCaptureAutoDisarm(after time.Duration) {
	armedAt := app.readArmTime()
	if armedAt.IsZero() {
		return
	}
	time.Sleep(after)
	current := app.readArmTime()
	if current.IsZero() || !current.Equal(armedAt) {
		// User stopped or re-armed; nothing to do.
		return
	}
	if _, err := os.Stat(app.captureFlagPath()); err != nil {
		return // flag already gone
	}
	app.logger.Info("audio capture: auto-disarm timeout reached", "minutes", audioCaptureMaxArmedMinutes)
	if _, err := app.disarmAndBundle("auto"); err != nil {
		app.logger.Error("audio capture: auto-disarm bundle", "err", err)
	}
}

// startupAudioCaptureWatchdog runs at server startup. If a stale arm flag
// is left over, either auto-disarm immediately (if past the deadline) or
// schedule auto-disarm for the remaining time.
func (app *App) startupAudioCaptureWatchdog() {
	if _, err := os.Stat(app.captureFlagPath()); err != nil {
		return
	}
	armedAt := app.readArmTime()
	if armedAt.IsZero() {
		// Flag exists but no timestamp — assume stale, disarm now.
		app.logger.Warn("audio capture: stale flag without timestamp, disarming")
		if _, err := app.disarmAndBundle("startup"); err != nil {
			app.logger.Error("audio capture: startup disarm", "err", err)
		}
		return
	}
	deadline := armedAt.Add(time.Duration(audioCaptureMaxArmedMinutes) * time.Minute)
	remaining := time.Until(deadline)
	if remaining <= 0 {
		app.logger.Info("audio capture: startup found expired arm, disarming")
		if _, err := app.disarmAndBundle("startup"); err != nil {
			app.logger.Error("audio capture: startup disarm", "err", err)
		}
		return
	}
	app.logger.Info("audio capture: startup found active arm, rescheduling auto-disarm",
		"remaining", remaining.String())
	go app.scheduleAudioCaptureAutoDisarm(remaining)
}

// handleAudioCaptureStatus returns current state derived from disk.
func (app *App) handleAudioCaptureStatus(w http.ResponseWriter, r *http.Request) {
	armed := false
	if _, err := os.Stat(app.captureFlagPath()); err == nil {
		armed = true
	}
	resp := map[string]any{}
	if armed {
		resp["state"] = "armed"
		resp["auto_disarm_minutes"] = audioCaptureMaxArmedMinutes
		if t := app.readArmTime(); !t.IsZero() {
			resp["armed_at"] = t.UTC().Format(time.RFC3339)
			deadline := t.Add(time.Duration(audioCaptureMaxArmedMinutes) * time.Minute)
			resp["seconds_until_auto_disarm"] = int(time.Until(deadline).Seconds())
		}
	} else {
		resp["state"] = "idle"
	}
	if b := app.latestCaptureBundle(); b != "" {
		resp["bundle_name"] = b
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

// handleAudioCaptureDownload streams the most recent capture bundle.
func (app *App) handleAudioCaptureDownload(w http.ResponseWriter, r *http.Request) {
	bundleName := app.latestCaptureBundle()
	if bundleName == "" {
		http.Error(w, "no bundle available", http.StatusNotFound)
		return
	}
	bundlePath := filepath.Join(app.basePath, bundleName)
	w.Header().Set("Content-Type", "application/gzip")
	w.Header().Set("Content-Disposition", `attachment; filename="`+bundleName+`"`)
	http.ServeFile(w, r, bundlePath)
}

// handleAudioCaptureDelete removes all capture bundles and the staged log.
// Useful after the user has downloaded what they need.
func (app *App) handleAudioCaptureDelete(w http.ResponseWriter, r *http.Request) {
	entries, err := os.ReadDir(app.basePath)
	if err != nil {
		http.Error(w, "read base path: "+err.Error(), http.StatusInternalServerError)
		return
	}
	removed := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		if strings.HasPrefix(e.Name(), audioCaptureBundlePfx) && strings.HasSuffix(e.Name(), ".tgz") {
			if err := os.Remove(filepath.Join(app.basePath, e.Name())); err == nil {
				removed++
			}
		}
	}
	_ = os.Remove(app.captureLogPath())
	app.logger.Info("audio capture bundles deleted", "count", removed)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"state":   "idle",
		"removed": removed,
	})
}

// buildAudioCaptureBundle packs xmos_sysex.txt + system info + debug.log
// tail + features.json into a gzipped tar at bundlePath.
func (app *App) buildAudioCaptureBundle(bundlePath string) error {
	out, err := os.Create(bundlePath)
	if err != nil {
		return err
	}
	defer out.Close()
	gz := gzip.NewWriter(out)
	defer gz.Close()
	tw := tar.NewWriter(gz)
	defer tw.Close()

	if err := writeTarBytes(tw, "diagnostics/system-info.txt", []byte(app.collectCaptureSystemInfo())); err != nil {
		return err
	}
	if data, err := os.ReadFile(app.captureLogPath()); err == nil {
		if err := writeTarBytes(tw, "diagnostics/xmos_sysex.txt", data); err != nil {
			return err
		}
	}
	debugPath := filepath.Join(app.basePath, "debug.log")
	if data, err := os.ReadFile(debugPath); err == nil {
		const maxTail = 200 * 1024
		if len(data) > maxTail {
			data = data[len(data)-maxTail:]
		}
		if err := writeTarBytes(tw, "diagnostics/debug.log.tail", data); err != nil {
			return err
		}
	}
	if data, err := os.ReadFile(filepath.Join(app.basePath, "features.json")); err == nil {
		if err := writeTarBytes(tw, "diagnostics/features.json", data); err != nil {
			return err
		}
	}
	return nil
}

func (app *App) collectCaptureSystemInfo() string {
	var b strings.Builder
	fmt.Fprintf(&b, "==== Schwung audio capture ====\n")
	fmt.Fprintf(&b, "captured_at: %s\n", time.Now().UTC().Format(time.RFC3339))
	if t := app.readArmTime(); !t.IsZero() {
		fmt.Fprintf(&b, "armed_at:    %s\n", t.UTC().Format(time.RFC3339))
	}
	fmt.Fprintf(&b, "\n")
	if v, err := os.ReadFile(filepath.Join(app.basePath, "host", "version.txt")); err == nil {
		fmt.Fprintf(&b, "schwung_version: %s\n", strings.TrimSpace(string(v)))
	}
	if out, err := exec.Command("uname", "-a").CombinedOutput(); err == nil {
		fmt.Fprintf(&b, "uname: %s", out)
	}
	if out, err := exec.Command("uptime").CombinedOutput(); err == nil {
		fmt.Fprintf(&b, "uptime: %s", out)
	}
	fmt.Fprintf(&b, "\n==== processes ====\n")
	if out, err := exec.Command("sh", "-c", "ps | grep -iE 'Move|schwung' | grep -v grep").CombinedOutput(); err == nil {
		b.Write(out)
	}
	fmt.Fprintf(&b, "\n==== modules ====\n")
	if out, err := exec.Command("sh", "-c", "ls "+filepath.Join(app.basePath, "modules")).CombinedOutput(); err == nil {
		b.Write(out)
	}
	return b.String()
}

func writeTarBytes(tw *tar.Writer, name string, data []byte) error {
	if err := tw.WriteHeader(&tar.Header{
		Name:    name,
		Mode:    0644,
		Size:    int64(len(data)),
		ModTime: time.Now(),
	}); err != nil {
		return err
	}
	_, err := tw.Write(data)
	return err
}
