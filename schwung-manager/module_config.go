// Per-module settings. A module opts in by shipping a
// settings-schema.json in its install directory; schwung-manager
// discovers these files, renders a page per module, and stores the
// user's chosen values in the module's own config.json (non-secret
// fields) or secrets/<key>.txt (password fields).
//
// Nothing in core settings-schema.json changes; this file is strictly
// additive. Modules that don't ship settings-schema.json are ignored.
//
// File layout under <basePath>/modules/<category>/<id>/:
//
//	settings-schema.json   immutable, ships in the module tarball
//	config.json            mutable, {"<key>": <value>, ...}
//	secrets/<key>.txt      0600, contents = raw secret string
//
// Install/upgrade must preserve config.json and secrets/ across
// tarball extraction; settings-schema.json is overwritten.

package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/user"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
)

// chownToAbleton flips a file's owner to ableton:users. schwung-manager
// runs as root on the device, so anything it creates inside a module
// directory is root-owned by default — but the module's ui.js runs as
// ableton and needs to read its own config and secrets. Failures are
// non-fatal in dev environments where ableton:users may not exist.
func chownToAbleton(path string) {
	u, err := user.Lookup("ableton")
	if err != nil {
		return
	}
	uid, _ := strconv.Atoi(u.Uid)
	gid, _ := strconv.Atoi(u.Gid)
	if g, gerr := user.LookupGroup("users"); gerr == nil {
		gid, _ = strconv.Atoi(g.Gid)
	}
	// Lchown so we never follow a (newly-introduced) symlink.
	_ = os.Lchown(path, uid, gid)
}

// ModuleSettingsSchema is one module's settings-schema.json, annotated
// with the module's install directory so handlers know where to write
// values and secrets.
type ModuleSettingsSchema struct {
	ID       string           `json:"id"`
	Label    string           `json:"label"`
	Sections []SettingsSection `json:"sections,omitempty"`
	// ModuleDir is the absolute install path. Not serialized — filled
	// in by the discovery pass.
	ModuleDir string `json:"-"`
	// Name is the module's display name from module.json, used in the
	// Modules index. Not serialized.
	Name string `json:"-"`
	// Category is the directory name under modules/ (e.g.
	// "sound_generators", "tools"), useful for display grouping.
	Category string `json:"-"`
}

// moduleIDPattern matches valid module IDs (mirrors the catalog rules).
// Schemas whose declared id doesn't match this are rejected.
var moduleIDPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]*$`)

// settingKeyPattern matches valid setting keys — module authors use
// snake_case (e.g. "openai_api_key"). Used to prevent path traversal
// through odd secret-filename characters.
var settingKeyPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9_]*$`)

// discoverModuleSchemas walks <basePath>/modules/**/settings-schema.json
// and returns the schemas it could parse. Fragments whose declared ID
// doesn't match the parent directory name (or fails validation) are
// dropped with a warning — this prevents a tampered schema from
// impersonating another module's directory.
func discoverModuleSchemas(basePath string) []ModuleSettingsSchema {
	var out []ModuleSettingsSchema
	categories := []string{
		"", // root-level built-ins like chain/, controller/, store/
		"sound_generators",
		"audio_fx",
		"midi_fx",
		"tools",
		"overtake",
		"utilities",
	}
	for _, cat := range categories {
		catDir := filepath.Join(basePath, "modules", cat)
		entries, err := os.ReadDir(catDir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() {
				continue
			}
			moduleDir := filepath.Join(catDir, e.Name())
			schemaPath := filepath.Join(moduleDir, "settings-schema.json")
			data, err := os.ReadFile(schemaPath)
			if err != nil {
				continue
			}
			var schema ModuleSettingsSchema
			if err := json.Unmarshal(data, &schema); err != nil {
				continue
			}
			// Reject schemas whose declared ID does not match the
			// parent directory name. This is the containment
			// guarantee — a module can only declare settings for
			// itself, not for a neighbor.
			if schema.ID != e.Name() {
				continue
			}
			if !moduleIDPattern.MatchString(schema.ID) {
				continue
			}
			schema.ModuleDir = moduleDir
			schema.Category = cat
			// Pull display name from module.json if present.
			if mjData, err := os.ReadFile(filepath.Join(moduleDir, "module.json")); err == nil {
				var mj struct {
					Name string `json:"name"`
				}
				if json.Unmarshal(mjData, &mj) == nil && mj.Name != "" {
					schema.Name = mj.Name
				}
			}
			if schema.Name == "" {
				schema.Name = schema.ID
			}
			// Flat-list fallback: schemas may ship either a top-level
			// "sections" array or a flat "items" array (shorthand for
			// a single-section schema). Normalize to sections.
			if len(schema.Sections) == 0 {
				var flat struct {
					Items []SettingsItem `json:"items"`
				}
				if json.Unmarshal(data, &flat) == nil && len(flat.Items) > 0 {
					schema.Sections = []SettingsSection{{
						ID:    schema.ID,
						Label: schema.Label,
						Items: flat.Items,
					}}
				}
			}
			out = append(out, schema)
		}
	}
	return out
}

// findModuleSchema looks up a schema by ID. Returns nil if not found.
func findModuleSchema(basePath, id string) *ModuleSettingsSchema {
	if !moduleIDPattern.MatchString(id) {
		return nil
	}
	schemas := discoverModuleSchemas(basePath)
	for i := range schemas {
		if schemas[i].ID == id {
			return &schemas[i]
		}
	}
	return nil
}

// findSettingsItem locates a schema item by key. Returns nil if the key
// isn't declared in the schema — callers reject writes for unknown keys.
func (s *ModuleSettingsSchema) findItem(key string) *SettingsItem {
	for si := range s.Sections {
		for ii := range s.Sections[si].Items {
			if s.Sections[si].Items[ii].Key == key {
				return &s.Sections[si].Items[ii]
			}
		}
	}
	return nil
}

// readModuleConfig reads the module's config.json, returning an empty
// map if the file is missing or invalid. Same semantics as readJSONFile
// but scoped to the module directory.
func readModuleConfig(moduleDir string) map[string]any {
	return readJSONFile(filepath.Join(moduleDir, "config.json"))
}

// writeModuleConfigKey atomically updates one key in the module's
// config.json. Uses write-to-temp + rename so a module reading the
// file concurrently sees either the old or the new state, never a
// half-written file.
func writeModuleConfigKey(moduleDir, key string, value any) error {
	cfgPath := filepath.Join(moduleDir, "config.json")
	cfg := readModuleConfig(moduleDir)
	cfg[key] = value
	pretty, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	pretty = append(pretty, '\n')
	tmp, err := os.CreateTemp(moduleDir, ".config.json.tmp.*")
	if err != nil {
		return err
	}
	tmpPath := tmp.Name()
	// Ensure tmp is cleaned up on any early return.
	cleanupTmp := func() { os.Remove(tmpPath) }
	if _, err := tmp.Write(pretty); err != nil {
		tmp.Close()
		cleanupTmp()
		return err
	}
	if err := tmp.Close(); err != nil {
		cleanupTmp()
		return err
	}
	if err := os.Chmod(tmpPath, 0644); err != nil {
		cleanupTmp()
		return err
	}
	if err := os.Rename(tmpPath, cfgPath); err != nil {
		return err
	}
	// schwung-manager runs as root; the module process runs as
	// ableton. Hand ownership over so the module can read its own
	// config back.
	chownToAbleton(cfgPath)
	return nil
}

// writeModuleSecret writes a secret value to <moduleDir>/secrets/<key>.txt
// with mode 0600, using O_EXCL + O_NOFOLLOW to defend against a
// symlink-swap race. If the file already exists it is removed first
// (Remove does not follow final-component symlinks).
//
// Keys are validated to match moduleIDPattern to prevent path traversal
// via odd key names.
func writeModuleSecret(moduleDir, key, value string) error {
	if !settingKeyPattern.MatchString(key) {
		return fmt.Errorf("invalid secret key %q", key)
	}
	secretsDir := filepath.Join(moduleDir, "secrets")
	// Reject if secrets/ is a symlink — mkdir would silently succeed
	// through a symlink to root-owned territory.
	if info, err := os.Lstat(secretsDir); err == nil {
		if info.Mode()&os.ModeSymlink != 0 {
			return fmt.Errorf("secrets dir is a symlink")
		}
		if !info.IsDir() {
			return fmt.Errorf("secrets path is not a directory")
		}
	}
	if err := os.MkdirAll(secretsDir, 0700); err != nil {
		return err
	}
	chownToAbleton(secretsDir)
	secretPath := filepath.Join(secretsDir, key+".txt")
	// Pre-remove any existing file. Remove (not RemoveAll) unlinks the
	// path without following the final-component symlink.
	_ = os.Remove(secretPath)
	fd, err := os.OpenFile(
		secretPath,
		os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW,
		0600,
	)
	if err != nil {
		return err
	}
	trimmed := strings.TrimSpace(value)
	if _, err := fd.Write([]byte(trimmed)); err != nil {
		fd.Close()
		os.Remove(secretPath)
		return err
	}
	if err := fd.Close(); err != nil {
		return err
	}
	chownToAbleton(secretPath)
	return nil
}

// isModuleSecretSet reports whether <moduleDir>/secrets/<key>.txt exists
// and has non-zero size. Used to swap the password input's placeholder
// to dots when the secret is already set, without exposing the secret.
func isModuleSecretSet(moduleDir, key string) bool {
	if !settingKeyPattern.MatchString(key) {
		return false
	}
	st, err := os.Stat(filepath.Join(moduleDir, "secrets", key+".txt"))
	if err != nil {
		return false
	}
	return st.Size() > 0
}

// clearModuleSecret unlinks <moduleDir>/secrets/<key>.txt. Returns nil
// if the file didn't exist (idempotent — repeated clears are fine).
func clearModuleSecret(moduleDir, key string) error {
	if !settingKeyPattern.MatchString(key) {
		return fmt.Errorf("invalid secret key %q", key)
	}
	path := filepath.Join(moduleDir, "secrets", key+".txt")
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}

// resolveModuleDefaultSource reads a default-source file shipped inside
// the module directory. The path must stay within moduleDir after
// symlink resolution and must be under 64KB. Returns an empty string
// on any failure — callers treat that as "no default available".
func resolveModuleDefaultSource(moduleDir, rel string) string {
	if rel == "" {
		return ""
	}
	if strings.Contains(rel, "..") || filepath.IsAbs(rel) {
		return ""
	}
	full := filepath.Join(moduleDir, rel)
	cleaned, err := filepath.EvalSymlinks(full)
	if err != nil {
		return ""
	}
	// Resolve moduleDir too, since its parent (e.g. /var on macOS) may
	// itself be a symlink to /private/var. Without this, a legitimate
	// file inside the module dir appears to be "outside" because the
	// resolved path has a different prefix than the un-resolved
	// moduleDir.
	baseClean, err := filepath.EvalSymlinks(moduleDir)
	if err != nil {
		baseClean = moduleDir
	}
	if !strings.HasPrefix(cleaned, baseClean+string(filepath.Separator)) &&
		cleaned != baseClean {
		return ""
	}
	data, err := os.ReadFile(cleaned)
	if err != nil {
		return ""
	}
	if len(data) > 64*1024 {
		data = data[:64*1024]
	}
	return string(data)
}

// moduleConfigValue returns the current value for a schema item:
// the saved config value if present, else the default_source file,
// else the schema's inline Default, else nil.
func moduleConfigValue(schema *ModuleSettingsSchema, item *SettingsItem, saved map[string]any) any {
	if v, ok := saved[item.Key]; ok && v != nil {
		if s, isStr := v.(string); !isStr || s != "" {
			return v
		}
	}
	if item.DefaultSource != "" {
		if s := resolveModuleDefaultSource(schema.ModuleDir, item.DefaultSource); s != "" {
			return s
		}
	}
	if item.Default != nil {
		return item.Default
	}
	return nil
}

// coerceModuleValue converts a form-submitted string into the
// appropriate typed value for a given schema item. Mirrors the
// core handler's type coercion but kept module-local so core
// semantics never leak into per-module storage.
func coerceModuleValue(item *SettingsItem, raw string) any {
	switch item.Type {
	case "bool":
		return raw == "true"
	case "int":
		n, _ := strconv.Atoi(raw)
		return n
	case "float":
		f, _ := strconv.ParseFloat(raw, 64)
		return f
	case "enum":
		if n, err := strconv.ParseFloat(raw, 64); err == nil {
			return n
		}
		return raw
	case "string":
		return strings.TrimSpace(raw)
	case "textarea":
		// Preserve internal whitespace — multi-line prompts matter.
		// Trim trailing spaces/CR/LF only so a form's accidental
		// trailing newline doesn't grow on every save.
		return strings.TrimRight(raw, " \t\r\n")
	default:
		return raw
	}
}

// ---------------------------------------------------------------------------
// Upgrade safety: preserve user-owned state across tarball extraction
// ---------------------------------------------------------------------------

// moduleUserState is a snapshot of files that must survive upgrade:
// config.json and the contents of secrets/. Only populated if the
// module directory currently contains these files.
type moduleUserState struct {
	configJSON []byte            // nil if config.json doesn't exist
	secrets    map[string][]byte // basename -> raw bytes
}

// snapshotModuleUserState reads config.json and secrets/*.txt from an
// existing module directory so they can be restored after tar
// extraction overwrites the directory. If moduleDir doesn't exist
// (first install), returns (nil, nil).
func snapshotModuleUserState(moduleDir string) (*moduleUserState, error) {
	if _, err := os.Stat(moduleDir); os.IsNotExist(err) {
		return nil, nil
	}
	state := &moduleUserState{}

	if data, err := os.ReadFile(filepath.Join(moduleDir, "config.json")); err == nil {
		state.configJSON = data
	}

	secretsDir := filepath.Join(moduleDir, "secrets")
	if entries, err := os.ReadDir(secretsDir); err == nil {
		state.secrets = make(map[string][]byte)
		for _, e := range entries {
			if e.IsDir() || !strings.HasSuffix(e.Name(), ".txt") {
				continue
			}
			info, err := e.Info()
			if err != nil {
				continue
			}
			// Skip symlinks defensively.
			if info.Mode()&os.ModeSymlink != 0 {
				continue
			}
			data, err := os.ReadFile(filepath.Join(secretsDir, e.Name()))
			if err != nil {
				continue
			}
			state.secrets[e.Name()] = data
		}
	}
	return state, nil
}

// restoreModuleUserState writes a previously-snapshotted state back
// into moduleDir, overwriting anything the tarball shipped for those
// paths. secrets/ files are written with mode 0600.
func restoreModuleUserState(moduleDir string, state *moduleUserState) error {
	if state == nil {
		return nil
	}
	if state.configJSON != nil {
		cfgPath := filepath.Join(moduleDir, "config.json")
		if err := os.WriteFile(cfgPath, state.configJSON, 0644); err != nil {
			return err
		}
		chownToAbleton(cfgPath)
	}
	if len(state.secrets) > 0 {
		secretsDir := filepath.Join(moduleDir, "secrets")
		if err := os.MkdirAll(secretsDir, 0700); err != nil {
			return err
		}
		chownToAbleton(secretsDir)
		for name, data := range state.secrets {
			path := filepath.Join(secretsDir, name)
			// O_NOFOLLOW: if the tarball planted a symlink at this
			// path, refuse rather than follow. Remove first so
			// O_EXCL succeeds.
			_ = os.Remove(path)
			fd, err := os.OpenFile(
				path,
				os.O_WRONLY|os.O_CREATE|os.O_EXCL|syscall.O_NOFOLLOW,
				0600,
			)
			if err != nil {
				return err
			}
			if _, err := fd.Write(data); err != nil {
				fd.Close()
				return err
			}
			if err := fd.Close(); err != nil {
				return err
			}
			chownToAbleton(path)
		}
	}
	return nil
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

// moduleIDFromPath extracts the <id> segment from
// /modules/<id>/settings/{values,set}. Returns the empty string if the
// path doesn't match a known shape.
func moduleIDFromPath(p string) string {
	const prefix = "/modules/"
	if !strings.HasPrefix(p, prefix) {
		return ""
	}
	rest := strings.TrimPrefix(p, prefix)
	if i := strings.IndexByte(rest, '/'); i >= 0 {
		rest = rest[:i]
	}
	if !moduleIDPattern.MatchString(rest) {
		return ""
	}
	return rest
}

// handleConfigModuleValues returns current values for polling.
// GET /modules/<id>/settings/values
func (app *App) handleConfigModuleValues(w http.ResponseWriter, r *http.Request) {
	id := moduleIDFromPath(r.URL.Path)
	if id == "" {
		http.NotFound(w, r)
		return
	}
	schema := findModuleSchema(app.basePath, id)
	if schema == nil {
		http.NotFound(w, r)
		return
	}
	saved := readModuleConfig(schema.ModuleDir)
	values := make(map[string]any)
	for _, section := range schema.Sections {
		for i := range section.Items {
			item := &section.Items[i]
			if item.Type == "password" {
				values[item.Key+"__is_set"] = isModuleSecretSet(schema.ModuleDir, item.Key)
				continue
			}
			if v := moduleConfigValue(schema, item, saved); v != nil {
				values[item.Key] = v
			}
		}
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(values)
}

// handleConfigModuleSet writes one key's value.
// POST /modules/<id>/settings/set  (form: key, value)
func (app *App) handleConfigModuleSet(w http.ResponseWriter, r *http.Request) {
	id := moduleIDFromPath(r.URL.Path)
	if id == "" {
		http.NotFound(w, r)
		return
	}
	schema := findModuleSchema(app.basePath, id)
	if schema == nil {
		http.NotFound(w, r)
		return
	}

	key := r.FormValue("key")
	value := r.FormValue("value")
	if key == "" {
		http.Error(w, `{"ok":false,"error":"missing key"}`, http.StatusBadRequest)
		return
	}
	item := schema.findItem(key)
	if item == nil {
		http.Error(w, `{"ok":false,"error":"unknown setting key"}`, http.StatusBadRequest)
		return
	}

	switch item.Type {
	case "password":
		// Blank value means "keep existing secret" — no-op. This
		// matches the placeholder UX where the input is pre-emptied
		// on focus.
		if value == "" {
			w.Header().Set("Content-Type", "application/json")
			w.Write([]byte(`{"ok":true}`))
			return
		}
		if err := writeModuleSecret(schema.ModuleDir, key, value); err != nil {
			app.logger.Error("module secret write failed", "module", id, "key", key, "err", err)
			http.Error(w, `{"ok":false,"error":"write failed"}`, http.StatusInternalServerError)
			return
		}
		app.logger.Info("module secret updated", "module", id, "key", key)
	default:
		if err := writeModuleConfigKey(schema.ModuleDir, key, coerceModuleValue(item, value)); err != nil {
			app.logger.Error("module config write failed", "module", id, "key", key, "err", err)
			http.Error(w, `{"ok":false,"error":"write failed"}`, http.StatusInternalServerError)
			return
		}
	}

	if r.Header.Get("X-CSRF-Token") != "" || r.Header.Get("Content-Type") == "application/json" {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`))
		return
	}
	http.Redirect(w, r, "/modules/"+id, http.StatusSeeOther)
}

// handleConfigModuleClearSecret deletes a stored secret.
// POST /modules/<id>/settings/clear  (form: key)
func (app *App) handleConfigModuleClearSecret(w http.ResponseWriter, r *http.Request) {
	id := moduleIDFromPath(r.URL.Path)
	if id == "" {
		http.NotFound(w, r)
		return
	}
	schema := findModuleSchema(app.basePath, id)
	if schema == nil {
		http.NotFound(w, r)
		return
	}
	key := r.FormValue("key")
	item := schema.findItem(key)
	if item == nil || item.Type != "password" {
		http.Error(w, `{"ok":false,"error":"unknown secret key"}`, http.StatusBadRequest)
		return
	}
	if err := clearModuleSecret(schema.ModuleDir, key); err != nil {
		app.logger.Error("module secret clear failed", "module", id, "key", key, "err", err)
		http.Error(w, `{"ok":false,"error":"clear failed"}`, http.StatusInternalServerError)
		return
	}
	app.logger.Info("module secret cleared", "module", id, "key", key)
	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"ok":true}`))
}
