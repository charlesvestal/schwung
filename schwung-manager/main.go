package main

import (
	"context"
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"hash/crc32"
	"html/template"
	"io"
	"io/fs"
	"log/slog"
	"net"
	"net/http"
	"net/http/httputil"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/charlesvestal/schwung/schwung-manager/middleware"
)

// ---------------------------------------------------------------------------
// Embedded assets
// ---------------------------------------------------------------------------

//go:embed all:templates
var templatesFS embed.FS

//go:embed static/*
var staticFS embed.FS

// ---------------------------------------------------------------------------
// Domain types
// ---------------------------------------------------------------------------

// CatalogModule describes one entry in the remote module catalog.
type CatalogModule struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Description   string `json:"description"`
	Author        string `json:"author"`
	ComponentType string `json:"component_type"`
	GithubRepo    string `json:"github_repo"`
	DefaultBranch string `json:"default_branch"`
	AssetName     string `json:"asset_name"`
	MinHostVer    string `json:"min_host_version"`
	Requires      string `json:"requires,omitempty"`
}

// CatalogHost describes the host entry in the catalog.
type CatalogHost struct {
	Name           string `json:"name"`
	GithubRepo     string `json:"github_repo"`
	AssetName      string `json:"asset_name"`
	LatestVersion  string `json:"latest_version"`
	DownloadURL    string `json:"download_url"`
	MinHostVersion string `json:"min_host_version"`
}

// Catalog is the top-level catalog structure.
type Catalog struct {
	CatalogVersion int             `json:"catalog_version"`
	Host           CatalogHost     `json:"host"`
	Modules        []CatalogModule `json:"modules"`
}

// ModuleAssets describes user-uploadable assets for a module.
type ModuleAssets struct {
	Path         string        `json:"path"`
	Label        string        `json:"label"`
	Extensions   []string      `json:"extensions"`
	Description  string        `json:"description"`
	Hint         string        `json:"hint"`
	HintURL      string        `json:"hint_url,omitempty"`
	HintURLLabel string        `json:"hint_url_label,omitempty"`
	Optional     bool          `json:"optional"`
	AllowFolders bool          `json:"allowFolders,omitempty"`
	Files        []AssetFile   `json:"files,omitempty"`
	Folders      []AssetFolder `json:"folders,omitempty"`
}

// AssetFile describes a specific expected file within a module's assets.
type AssetFile struct {
	Filename string `json:"filename"`
	Label    string `json:"label"`
	Size     int64  `json:"size"`
	Required bool   `json:"required"`
	CRC32    string `json:"crc32,omitempty"`
}

// AssetFolder describes an expected folder within a module's assets.
type AssetFolder struct {
	Path        string   `json:"path"`
	Label       string   `json:"label"`
	Description string   `json:"description"`
	Extensions  []string `json:"extensions"`
	Required    bool     `json:"required"`
}

// AssetFileStatus is the validation result for a single asset file.
type AssetFileStatus struct {
	AssetFile
	Present    bool
	SizeMatch  bool
	CRCMatch   *bool
	ActualSize int64
	ActualCRC  string
}

// AssetFolderStatus is the validation result for a single asset folder.
type AssetFolderStatus struct {
	AssetFolder
	Exists    bool
	FileCount int
}

// InstalledModule is read from a module.json on disk.
type InstalledModule struct {
	ID            string        `json:"id"`
	Name          string        `json:"name"`
	Version       string        `json:"version"`
	ComponentType string        `json:"component_type"`
	Description   string        `json:"description"`
	Assets        *ModuleAssets `json:"assets,omitempty"`
}

// SettingsSection describes a section of settings from settings-schema.json.
type SettingsSection struct {
	ID    string         `json:"id"`
	Label string         `json:"label"`
	Items []SettingsItem `json:"items"`
}

// SettingsItem describes a single setting within a section.
type SettingsItem struct {
	Key     string  `json:"key"`
	Label   string  `json:"label"`
	Type    string  `json:"type"`
	Options []string `json:"options,omitempty"`
	Values  []any   `json:"values,omitempty"`
	Min     float64 `json:"min,omitempty"`
	Max     float64 `json:"max,omitempty"`
	Step    float64 `json:"step,omitempty"`
}

// HelpNode represents a node in the help content tree.
type HelpNode struct {
	Title    string     `json:"title"`
	Lines    []string   `json:"lines,omitempty"`
	Children []HelpNode `json:"children,omitempty"`
}

// HelpContent is the top-level structure of help_content.json.
type HelpContent struct {
	Sections []HelpNode `json:"sections"`
}

// FileEntry represents a file or directory for the file browser.
type FileEntry struct {
	Name    string
	Path    string
	IsDir   bool
	Size    int64
	ModTime time.Time
}

// ---------------------------------------------------------------------------
// Services
// ---------------------------------------------------------------------------

// FileService provides safe filesystem operations.
type FileService struct {
	AllowedRoots []string
}

func (s *FileService) validate(path string) (string, error) {
	return middleware.ValidatePath(path, s.AllowedRoots)
}

// ListDir returns entries in the given directory.
func (s *FileService) ListDir(dir string) ([]FileEntry, error) {
	clean, err := s.validate(dir)
	if err != nil {
		return nil, err
	}
	entries, err := os.ReadDir(clean)
	if err != nil {
		return nil, err
	}
	var result []FileEntry
	for _, e := range entries {
		info, err := e.Info()
		if err != nil {
			continue
		}
		result = append(result, FileEntry{
			Name:    e.Name(),
			Path:    filepath.Join(clean, e.Name()),
			IsDir:   e.IsDir(),
			Size:    info.Size(),
			ModTime: info.ModTime(),
		})
	}
	// Directories first, then alphabetical.
	sort.Slice(result, func(i, j int) bool {
		if result[i].IsDir != result[j].IsDir {
			return result[i].IsDir
		}
		return strings.ToLower(result[i].Name) < strings.ToLower(result[j].Name)
	})
	return result, nil
}

// ReleaseMeta holds release dates for a module.
type ReleaseMeta struct {
	FirstRelease string `json:"first_release"`
	LastUpdated  string `json:"last_updated"`
	Version      string `json:"version"`
}

// CatalogService fetches and caches the remote module catalog.
type CatalogService struct {
	URL          string
	catalog      *Catalog
	releaseMeta  map[string]ReleaseMeta
	fetched      time.Time
	client       *http.Client
}

const releaseMetaURL = "https://charlesvestal.github.io/schwung-catalog-site/data/release-metadata.json"

func NewCatalogService(url string) *CatalogService {
	return &CatalogService{
		URL:    url,
		client: &http.Client{Timeout: 15 * time.Second},
	}
}

// Fetch retrieves the catalog, caching for 5 minutes.
func (cs *CatalogService) Fetch() (*Catalog, error) {
	if cs.catalog != nil && time.Since(cs.fetched) < 5*time.Minute {
		return cs.catalog, nil
	}
	resp, err := cs.client.Get(cs.URL)
	if err != nil {
		return cs.catalog, fmt.Errorf("fetching catalog: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return cs.catalog, fmt.Errorf("catalog returned %d", resp.StatusCode)
	}
	var cat Catalog
	if err := json.NewDecoder(resp.Body).Decode(&cat); err != nil {
		return cs.catalog, fmt.Errorf("decoding catalog: %w", err)
	}
	cs.catalog = &cat
	cs.fetched = time.Now()

	// Fetch release metadata (best-effort, don't fail if unavailable).
	if metaResp, err := cs.client.Get(releaseMetaURL); err == nil {
		defer metaResp.Body.Close()
		if metaResp.StatusCode == http.StatusOK {
			var meta map[string]ReleaseMeta
			if json.NewDecoder(metaResp.Body).Decode(&meta) == nil {
				cs.releaseMeta = meta
			}
		}
	}

	return cs.catalog, nil
}

// GetReleaseMeta returns cached release metadata.
func (cs *CatalogService) GetReleaseMeta() map[string]ReleaseMeta {
	if cs.releaseMeta == nil {
		return map[string]ReleaseMeta{}
	}
	return cs.releaseMeta
}

// ---------------------------------------------------------------------------
// Installed module discovery
// ---------------------------------------------------------------------------

func discoverInstalledModules(base string) map[string]InstalledModule {
	installed := make(map[string]InstalledModule)
	// Walk known category dirs and the root modules dir.
	dirs := []string{
		filepath.Join(base, "modules"),
		filepath.Join(base, "modules", "sound_generators"),
		filepath.Join(base, "modules", "audio_fx"),
		filepath.Join(base, "modules", "midi_fx"),
		filepath.Join(base, "modules", "tools"),
		filepath.Join(base, "modules", "overtake"),
	}
	for _, dir := range dirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}
		for _, e := range entries {
			if !e.IsDir() {
				continue
			}
			mj := filepath.Join(dir, e.Name(), "module.json")
			data, err := os.ReadFile(mj)
			if err != nil {
				continue
			}
			var m InstalledModule
			if json.Unmarshal(data, &m) == nil && m.ID != "" {
				// Fall back to capabilities.component_type if top-level is empty.
				if m.ComponentType == "" {
					var raw map[string]any
					if json.Unmarshal(data, &raw) == nil {
						if caps, ok := raw["capabilities"].(map[string]any); ok {
							if ct, ok := caps["component_type"].(string); ok {
								m.ComponentType = ct
							}
						}
					}
				}
				installed[m.ID] = m
			}
		}
	}
	return installed
}

// ---------------------------------------------------------------------------
// Template helpers
// ---------------------------------------------------------------------------

var funcMap = template.FuncMap{
	"dict": func(pairs ...any) map[string]any {
		m := make(map[string]any, len(pairs)/2)
		for i := 0; i+1 < len(pairs); i += 2 {
			key, _ := pairs[i].(string)
			m[key] = pairs[i+1]
		}
		return m
	},
	"formatBytes": func(b int64) string {
		const unit = 1024
		if b < unit {
			return fmt.Sprintf("%d B", b)
		}
		div, exp := int64(unit), 0
		for n := b / unit; n >= unit; n /= unit {
			div *= unit
			exp++
		}
		return fmt.Sprintf("%.1f %cB", float64(b)/float64(div), "KMGTPE"[exp])
	},
	"formatTime": func(t time.Time) string {
		if t.IsZero() {
			return "-"
		}
		return t.Format("2006-01-02 15:04")
	},
	"categoryLabel": func(ct string) string {
		labels := map[string]string{
			"sound_generator": "Sound Generator",
			"audio_fx":        "Audio FX",
			"midi_fx":         "MIDI FX",
			"utility":         "Utility",
			"overtake":        "Overtake",
			"tool":            "Tool",
			"system":          "System",
			"featured":        "Featured",
		}
		if l, ok := labels[ct]; ok {
			return l
		}
		return ct
	},
	"isInstalled": func(id string, installed map[string]InstalledModule) bool {
		_, ok := installed[id]
		return ok
	},
	"releaseMeta": func(id string, meta map[string]ReleaseMeta) ReleaseMeta {
		return meta[id]
	},
	"versionStr": func(v string) string {
		if v == "" {
			return ""
		}
		if !strings.HasPrefix(v, "v") {
			return "v" + v
		}
		return v
	},
	"settingValue": func(key string, values map[string]any) any {
		if v, ok := values[key]; ok {
			return v
		}
		return ""
	},
	"settingChecked": func(key string, values map[string]any) bool {
		v, ok := values[key]
		if !ok {
			return false
		}
		switch b := v.(type) {
		case bool:
			return b
		case float64:
			return b != 0
		default:
			return false
		}
	},
	"enumOptions": func(item SettingsItem) []map[string]any {
		var result []map[string]any
		for i, opt := range item.Options {
			var val any
			if i < len(item.Values) {
				val = item.Values[i]
			} else {
				val = opt
			}
			result = append(result, map[string]any{"Label": opt, "Value": val})
		}
		return result
	},
	"settingSelected": func(optVal any, key string, values map[string]any) bool {
		cur, ok := values[key]
		if !ok {
			return false
		}
		// Compare as strings for robustness (JSON numbers are float64).
		return fmt.Sprint(optVal) == fmt.Sprint(cur)
	},
	"joinHelpLines": func(lines []string) template.HTML {
		// Join OLED-width lines into paragraphs for web display.
		// Blank lines become paragraph breaks.
		var paragraphs []string
		var current []string
		for _, line := range lines {
			if line == "" {
				if len(current) > 0 {
					paragraphs = append(paragraphs, strings.Join(current, " "))
					current = nil
				}
			} else {
				current = append(current, strings.TrimRight(line, " "))
			}
		}
		if len(current) > 0 {
			paragraphs = append(paragraphs, strings.Join(current, " "))
		}
		var sb strings.Builder
		for _, p := range paragraphs {
			sb.WriteString("<p>")
			sb.WriteString(template.HTMLEscapeString(p))
			sb.WriteString("</p>\n")
		}
		return template.HTML(sb.String())
	},
	"hasUpdate": func(id string, installed map[string]InstalledModule, meta map[string]ReleaseMeta) bool {
		inst, ok := installed[id]
		if !ok {
			return false
		}
		rm, ok := meta[id]
		if !ok || rm.Version == "" {
			return false // Can't tell — don't show update button
		}
		return isNewerSemver(rm.Version, inst.Version)
	},
	"humanSize": func(b int64) string {
		const unit = 1024
		if b < unit {
			return fmt.Sprintf("%d B", b)
		}
		div, exp := int64(unit), 0
		for n := b / unit; n >= unit; n /= unit {
			div *= unit
			exp++
		}
		return fmt.Sprintf("%.0f %cB", float64(b)/float64(div), "KMGTPE"[exp])
	},
	"derefBool": func(b *bool) bool {
		if b == nil {
			return true
		}
		return *b
	},
}

// validateAssets checks the presence and integrity of declared asset files and folders.
func validateAssets(assetsDir string, assets *ModuleAssets) ([]AssetFileStatus, []AssetFolderStatus) {
	var fileStatuses []AssetFileStatus
	for _, f := range assets.Files {
		status := AssetFileStatus{AssetFile: f}
		path := filepath.Join(assetsDir, f.Filename)
		info, err := os.Stat(path)
		if err == nil && !info.IsDir() {
			status.Present = true
			status.ActualSize = info.Size()
			status.SizeMatch = (f.Size == 0 || info.Size() == f.Size)
			if f.CRC32 != "" {
				data, err := os.ReadFile(path)
				if err == nil {
					actual := fmt.Sprintf("%08X", crc32.ChecksumIEEE(data))
					status.ActualCRC = actual
					match := strings.EqualFold(actual, f.CRC32)
					status.CRCMatch = &match
				}
			}
		}
		fileStatuses = append(fileStatuses, status)
	}

	var folderStatuses []AssetFolderStatus
	for _, f := range assets.Folders {
		status := AssetFolderStatus{AssetFolder: f}
		dir := filepath.Join(assetsDir, f.Path)
		entries, err := os.ReadDir(dir)
		if err == nil {
			status.Exists = true
			for _, e := range entries {
				if e.IsDir() {
					continue
				}
				if len(f.Extensions) == 0 {
					status.FileCount++
					continue
				}
				ext := strings.ToLower(filepath.Ext(e.Name()))
				for _, allowed := range f.Extensions {
					if strings.EqualFold(ext, allowed) {
						status.FileCount++
						break
					}
				}
			}
		}
		folderStatuses = append(folderStatuses, status)
	}

	return fileStatuses, folderStatuses
}

// templateMap maps page template names to their parsed template sets.
// isNewerSemver returns true if `latest` is a newer version than `current`.
// Handles v-prefixed versions. Returns false if versions are equal or
// latest is older (avoids phantom upgrade prompts from stale metadata).
func isNewerSemver(latest, current string) bool {
	latest = strings.TrimPrefix(latest, "v")
	current = strings.TrimPrefix(current, "v")
	if latest == current {
		return false
	}
	lp := strings.Split(latest, ".")
	cp := strings.Split(current, ".")
	for i := 0; i < len(lp) && i < len(cp); i++ {
		l, _ := strconv.Atoi(lp[i])
		c, _ := strconv.Atoi(cp[i])
		if l > c {
			return true
		}
		if l < c {
			return false
		}
	}
	return len(lp) > len(cp)
}

type templateMap map[string]*template.Template

func loadTemplates() (templateMap, error) {
	// Parse shared templates (base layout + partials).
	base, err := template.New("").Funcs(funcMap).ParseFS(templatesFS,
		"templates/base.html",
		"templates/partials/*.html",
	)
	if err != nil {
		return nil, fmt.Errorf("parsing base templates: %w", err)
	}

	// Each page template gets its own clone so "content"/"title" blocks
	// don't collide across pages.
	pages := []string{
		"templates/modules.html",
		"templates/module_detail.html",
		"templates/files.html",
		"templates/config.html",
		"templates/system.html",
		"templates/install.html",
		"templates/help.html",
		"templates/remote_ui.html",
	}

	m := make(templateMap, len(pages))
	for _, page := range pages {
		clone, err := base.Clone()
		if err != nil {
			return nil, fmt.Errorf("cloning base for %s: %w", page, err)
		}
		t, err := clone.ParseFS(templatesFS, page)
		if err != nil {
			return nil, fmt.Errorf("parsing %s: %w", page, err)
		}
		// Key by short name for convenience in render() calls.
		// ParseFS names the template by its full path within the FS,
		// so we store both the short name and the full FS path.
		short := filepath.Base(page)
		m[short] = t
	}
	return m, nil
}

// ---------------------------------------------------------------------------
// App holds shared dependencies.
// ---------------------------------------------------------------------------

type App struct {
	tmpl          templateMap
	fileSvc       *FileService
	catalogSvc    *CatalogService
	basePath      string // e.g. /data/UserData/schwung
	logger        *slog.Logger
	shm           *ShmConfig  // shared memory for live config sync (nil if not on device)
	shmParams     *ShmParams  // shared memory for param get/set (nil if not on device)
	upgradeStatus string      // current upgrade step (empty = not upgrading)
}

func (app *App) render(w http.ResponseWriter, r *http.Request, name string, data map[string]any) {
	t, ok := app.tmpl[name]
	if !ok {
		app.logger.Error("template not found", "template", name)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
		return
	}
	// Inject CSRF token from cookie so forms work without JS.
	if cookie, err := r.Cookie("csrf_token"); err == nil {
		data["CSRFToken"] = cookie.Value
	}
	// Inject mirror enabled state for nav bar.
	if app.shm != nil {
		data["MirrorEnabled"] = app.shm.DisplayMirror()
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	// ParseFS names templates by the base filename, not the full path.
	if err := t.ExecuteTemplate(w, name, data); err != nil {
		app.logger.Error("template render", "template", name, "err", err)
		http.Error(w, "Internal Server Error", http.StatusInternalServerError)
	}
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

// -- Home --

func (app *App) handleHome(w http.ResponseWriter, r *http.Request) {
	http.Redirect(w, r, "/modules", http.StatusSeeOther)
}

// -- Modules --

func (app *App) handleModules(w http.ResponseWriter, r *http.Request) {
	cat, err := app.catalogSvc.Fetch()
	if err != nil {
		app.logger.Warn("catalog fetch failed", "err", err)
	}
	installed := discoverInstalledModules(app.basePath)

	var modules []CatalogModule
	if cat != nil {
		modules = cat.Modules
	}

	// Build catalog ID set and add built-in modules as synthetic catalog entries.
	catalogIDs := make(map[string]bool)
	for _, m := range modules {
		catalogIDs[m.ID] = true
	}

	// Hidden test/infrastructure modules.
	hiddenIDs := map[string]bool{
		"splash-test":        true,
		"standalone-example": true,
		"text-test":          true,
	}

	// Add built-in installed modules to the catalog list so they appear
	// alongside external modules. They won't have install/uninstall/update buttons.
	for id, mod := range installed {
		if catalogIDs[id] || hiddenIDs[id] {
			continue
		}
		modules = append(modules, CatalogModule{
			ID:            id,
			Name:          mod.Name,
			Description:   mod.Description,
			Author:        "Schwung",
			ComponentType: mod.ComponentType,
			GithubRepo:    "charlesvestal/schwung",
			MinHostVer:    "0.1.0",
		})
	}

	releaseMeta := app.catalogSvc.GetReleaseMeta()

	// Check if any installed module has an update available.
	hasAnyUpdate := false
	for id, inst := range installed {
		rm, ok := releaseMeta[id]
		if !ok || rm.Version == "" {
			continue
		}
		if isNewerSemver(rm.Version, inst.Version) {
			hasAnyUpdate = true
			break
		}
	}

	data := map[string]any{
		"Title":        "Modules",
		"Modules":      modules,
		"Installed":    installed,
		"HasInstalled": len(installed) > 0,
		"HasAnyUpdate": hasAnyUpdate,
		"ReleaseMeta":  releaseMeta,
		"Active":       "modules",
	}
	app.render(w, r, "modules.html", data)
}

// findModuleDir locates the installed directory for a module by ID.
func (app *App) findModuleDir(id string) string {
	dirs := []string{"modules", "modules/sound_generators", "modules/audio_fx", "modules/midi_fx", "modules/tools", "modules/overtake"}
	for _, d := range dirs {
		candidate := filepath.Join(app.basePath, d, id)
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			return candidate
		}
	}
	return ""
}

func (app *App) handleModuleDetail(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	cat, _ := app.catalogSvc.Fetch()
	installed := discoverInstalledModules(app.basePath)

	var mod *CatalogModule
	if cat != nil {
		for i := range cat.Modules {
			if cat.Modules[i].ID == id {
				mod = &cat.Modules[i]
				break
			}
		}
	}
	// If not in catalog, check if it's a built-in installed module.
	builtIn := false
	if mod == nil {
		if inst, ok := installed[id]; ok {
			mod = &CatalogModule{
				ID:            id,
				Name:          inst.Name,
				Description:   inst.Description,
				Author:        "Schwung",
				ComponentType: inst.ComponentType,
				GithubRepo:    "charlesvestal/schwung",
				MinHostVer:    "0.1.0",
			}
			builtIn = true
		}
	}
	if mod == nil {
		http.NotFound(w, r)
		return
	}

	modDir := app.findModuleDir(id)

	// Compute assets directory from installed module.json assets.path field.
	assetsDir := ""
	var moduleAssets *ModuleAssets
	if inst, ok := installed[id]; ok && inst.Assets != nil {
		moduleAssets = inst.Assets
		if inst.Assets.Path != "" && inst.Assets.Path != "." {
			assetsDir = filepath.Join(modDir, inst.Assets.Path)
		} else {
			assetsDir = modDir
		}
	}

	var fileStatuses []AssetFileStatus
	var folderStatuses []AssetFolderStatus
	if moduleAssets != nil && len(moduleAssets.Files) > 0 && assetsDir != "" {
		fileStatuses, folderStatuses = validateAssets(assetsDir, moduleAssets)
	}

	data := map[string]any{
		"Title":          mod.Name,
		"Module":         mod,
		"Installed":      installed,
		"ModuleDir":      modDir,
		"AssetsDir":      assetsDir,
		"ModuleAssets":   moduleAssets,
		"FileStatuses":   fileStatuses,
		"FolderStatuses": folderStatuses,
		"BuiltIn":        builtIn,
		"ReleaseMeta":    app.catalogSvc.GetReleaseMeta(),
		"Active":         "modules",
	}
	app.render(w, r, "module_detail.html", data)
}

// getInstallSubdir maps component_type to the install subdirectory name.
func getInstallSubdir(componentType string) string {
	switch componentType {
	case "sound_generator":
		return "sound_generators"
	case "audio_fx":
		return "audio_fx"
	case "midi_fx":
		return "midi_fx"
	case "utility":
		return "utilities"
	case "overtake":
		return "overtake"
	case "tool":
		return "tools"
	default:
		return "other"
	}
}

// ReleaseJSON is the structure of a module's release.json file.
type ReleaseJSON struct {
	Version     string `json:"version"`
	DownloadURL string `json:"download_url"`
}

// installModule downloads and extracts a module from its GitHub release.
func (app *App) installModule(mod *CatalogModule) error {
	client := &http.Client{Timeout: 120 * time.Second}

	// 1. Fetch release.json to get download URL.
	releaseURL := fmt.Sprintf("https://raw.githubusercontent.com/%s/%s/release.json",
		mod.GithubRepo, mod.DefaultBranch)
	app.logger.Info("fetching release.json", "url", releaseURL)

	resp, err := client.Get(releaseURL)
	if err != nil {
		return fmt.Errorf("fetching release.json: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		// Fall back to latest release download URL.
		app.logger.Warn("release.json not found, using fallback URL", "status", resp.StatusCode)
	}

	var downloadURL string
	if resp.StatusCode == http.StatusOK {
		var rel ReleaseJSON
		if err := json.NewDecoder(resp.Body).Decode(&rel); err != nil {
			return fmt.Errorf("decoding release.json: %w", err)
		}
		downloadURL = rel.DownloadURL
	}
	if downloadURL == "" {
		// Fallback: use GitHub releases latest download.
		downloadURL = fmt.Sprintf("https://github.com/%s/releases/latest/download/%s",
			mod.GithubRepo, mod.AssetName)
	}

	// 2. Download the tarball.
	app.logger.Info("downloading module", "id", mod.ID, "url", downloadURL)
	dlResp, err := client.Get(downloadURL)
	if err != nil {
		return fmt.Errorf("downloading tarball: %w", err)
	}
	defer dlResp.Body.Close()
	if dlResp.StatusCode != http.StatusOK {
		return fmt.Errorf("download returned %d", dlResp.StatusCode)
	}

	// Save to temp file in /data/UserData/ (not /tmp which is on rootfs).
	tmpPath := filepath.Join(app.basePath, ".tmp-module-download.tar.gz")
	tmpFile, err := os.Create(tmpPath)
	if err != nil {
		return fmt.Errorf("creating temp file: %w", err)
	}
	defer os.Remove(tmpPath)

	if _, err := io.Copy(tmpFile, dlResp.Body); err != nil {
		tmpFile.Close()
		return fmt.Errorf("saving tarball: %w", err)
	}
	tmpFile.Close()

	// 3. Extract to the correct category directory.
	categoryDir := filepath.Join(app.basePath, "modules", getInstallSubdir(mod.ComponentType))
	if err := os.MkdirAll(categoryDir, 0755); err != nil {
		return fmt.Errorf("creating category dir: %w", err)
	}

	// Extract using tar command (busybox tar on Move).
	app.logger.Info("extracting module", "id", mod.ID, "dest", categoryDir)
	cmd := exec.Command("tar", "-xzf", tmpPath, "-C", categoryDir)
	if output, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("extracting tarball: %w\noutput: %s", err, output)
	}

	app.logger.Info("module installed", "id", mod.ID, "path", categoryDir)
	return nil
}

// uninstallModule removes a module from disk.
func (app *App) uninstallModule(id string) error {
	modDir := app.findModuleDir(id)
	if modDir == "" {
		return fmt.Errorf("module %q not found on disk", id)
	}
	app.logger.Info("uninstalling module", "id", id, "path", modDir)
	return os.RemoveAll(modDir)
}

// findCatalogModule looks up a module by ID in the catalog.
func (app *App) findCatalogModule(id string) *CatalogModule {
	cat, _ := app.catalogSvc.Fetch()
	if cat == nil {
		return nil
	}
	for i := range cat.Modules {
		if cat.Modules[i].ID == id {
			return &cat.Modules[i]
		}
	}
	return nil
}

// moduleRedirect sends the user back to where they came from (Referer),
// falling back to the module detail page.
func (app *App) moduleRedirect(w http.ResponseWriter, r *http.Request, id, flash string) {
	dest := r.Header.Get("Referer")
	if dest == "" {
		dest = "/modules/" + id
	}
	// Append flash as query param
	sep := "?"
	if strings.Contains(dest, "?") {
		sep = "&"
	}
	http.Redirect(w, r, dest+sep+"flash="+flash, http.StatusSeeOther)
}

func (app *App) handleModuleInstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	mod := app.findCatalogModule(id)
	if mod == nil {
		app.moduleRedirect(w, r, id, "Module+not+found:+"+id)
		return
	}
	if err := app.installModule(mod); err != nil {
		app.logger.Error("module install failed", "id", id, "err", err)
		app.moduleRedirect(w, r, id, "Install+failed:+"+err.Error())
		return
	}
	app.moduleRedirect(w, r, id, mod.Name+"+installed+successfully")
}

func (app *App) handleModuleUninstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := app.uninstallModule(id); err != nil {
		app.logger.Error("module uninstall failed", "id", id, "err", err)
		app.moduleRedirect(w, r, id, "Uninstall+failed:+"+err.Error())
		return
	}
	app.moduleRedirect(w, r, id, "Module+uninstalled")
}

func (app *App) handleModuleUpdate(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	mod := app.findCatalogModule(id)
	if mod == nil {
		app.moduleRedirect(w, r, id, "Module+not+found:+"+id)
		return
	}
	if err := app.installModule(mod); err != nil {
		app.logger.Error("module update failed", "id", id, "err", err)
		app.moduleRedirect(w, r, id, "Update+failed:+"+err.Error())
		return
	}
	app.moduleRedirect(w, r, id, mod.Name+"+updated+successfully")
}

func (app *App) handleModuleUpdateAll(w http.ResponseWriter, r *http.Request) {
	installed := discoverInstalledModules(app.basePath)
	var updated, failed int
	for id := range installed {
		mod := app.findCatalogModule(id)
		if mod == nil {
			continue
		}
		if err := app.installModule(mod); err != nil {
			app.logger.Error("update failed", "id", id, "err", err)
			failed++
		} else {
			updated++
		}
	}
	msg := fmt.Sprintf("Updated+%d+modules", updated)
	if failed > 0 {
		msg += fmt.Sprintf(",+%d+failed", failed)
	}
	http.Redirect(w, r, "/modules?flash="+msg, http.StatusSeeOther)
}

func (app *App) handleCustomInstall(w http.ResponseWriter, r *http.Request) {
	source := r.FormValue("source")
	switch source {
	case "github":
		repoInput := r.FormValue("url")
		// Normalize: strip https://github.com/ prefix if present.
		repo := strings.TrimPrefix(repoInput, "https://github.com/")
		repo = strings.TrimPrefix(repo, "http://github.com/")
		repo = strings.TrimSuffix(repo, "/")
		repo = strings.TrimSuffix(repo, ".git")

		if repo == "" || !strings.Contains(repo, "/") {
			http.Redirect(w, r, "/modules?flash=Invalid+GitHub+URL", http.StatusSeeOther)
			return
		}

		// Fetch release.json from the repo (try main, then master).
		client := &http.Client{Timeout: 30 * time.Second}
		var rel ReleaseJSON
		var found bool
		for _, branch := range []string{"main", "master"} {
			u := fmt.Sprintf("https://raw.githubusercontent.com/%s/%s/release.json", repo, branch)
			resp, err := client.Get(u)
			if err != nil {
				continue
			}
			defer resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				if json.NewDecoder(resp.Body).Decode(&rel) == nil && rel.DownloadURL != "" {
					found = true
					break
				}
			}
		}
		if !found {
			http.Redirect(w, r, "/modules?flash=Could+not+find+release.json+in+"+repo, http.StatusSeeOther)
			return
		}

		// Download and extract.
		app.logger.Info("custom install from github", "repo", repo, "url", rel.DownloadURL)
		dlResp, err := client.Get(rel.DownloadURL)
		if err != nil || dlResp.StatusCode != http.StatusOK {
			http.Redirect(w, r, "/modules?flash=Download+failed+for+"+repo, http.StatusSeeOther)
			return
		}
		defer dlResp.Body.Close()

		// Save and extract (we don't know the component_type, extract to a temp location,
		// then read module.json to determine the correct category).
		tmpPath := filepath.Join(app.basePath, ".tmp-custom-download.tar.gz")
		tmpFile, _ := os.Create(tmpPath)
		io.Copy(tmpFile, dlResp.Body)
		tmpFile.Close()
		defer os.Remove(tmpPath)

		// Extract to a temp dir first to read module.json.
		tmpDir := filepath.Join(app.basePath, ".tmp-custom-extract")
		os.MkdirAll(tmpDir, 0755)
		defer os.RemoveAll(tmpDir)

		cmd := exec.Command("tar", "-xzf", tmpPath, "-C", tmpDir)
		if output, err := cmd.CombinedOutput(); err != nil {
			http.Redirect(w, r, "/modules?flash=Extract+failed:+"+string(output), http.StatusSeeOther)
			return
		}

		// Find module.json in the extracted directory.
		entries, _ := os.ReadDir(tmpDir)
		if len(entries) == 0 {
			http.Redirect(w, r, "/modules?flash=Tarball+is+empty", http.StatusSeeOther)
			return
		}
		moduleDir := filepath.Join(tmpDir, entries[0].Name())
		mjData, err := os.ReadFile(filepath.Join(moduleDir, "module.json"))
		if err != nil {
			http.Redirect(w, r, "/modules?flash=No+module.json+found+in+tarball", http.StatusSeeOther)
			return
		}
		var mj struct {
			ID           string `json:"id"`
			Capabilities struct {
				ComponentType string `json:"component_type"`
			} `json:"capabilities"`
		}
		json.Unmarshal(mjData, &mj)

		componentType := mj.Capabilities.ComponentType
		if componentType == "" {
			componentType = "other"
		}

		// Move to the correct category directory.
		categoryDir := filepath.Join(app.basePath, "modules", getInstallSubdir(componentType))
		os.MkdirAll(categoryDir, 0755)
		destDir := filepath.Join(categoryDir, entries[0].Name())
		os.RemoveAll(destDir) // Remove old version if exists.
		if err := os.Rename(moduleDir, destDir); err != nil {
			http.Redirect(w, r, "/modules?flash=Move+failed:+"+err.Error(), http.StatusSeeOther)
			return
		}

		app.logger.Info("custom module installed", "id", mj.ID, "path", destDir)
		http.Redirect(w, r, "/modules?flash=Installed+"+entries[0].Name()+"+from+GitHub", http.StatusSeeOther)

	case "tarball":
		file, header, err := r.FormFile("file")
		if err != nil {
			http.Redirect(w, r, "/modules?flash=No+file+provided", http.StatusSeeOther)
			return
		}
		defer file.Close()

		// Save uploaded tarball.
		tmpPath := filepath.Join(app.basePath, ".tmp-upload-"+header.Filename)
		tmpFile, err := os.Create(tmpPath)
		if err != nil {
			http.Redirect(w, r, "/modules?flash=Failed+to+save+upload", http.StatusSeeOther)
			return
		}
		io.Copy(tmpFile, file)
		tmpFile.Close()
		defer os.Remove(tmpPath)

		// Extract to temp dir to read module.json.
		tmpDir := filepath.Join(app.basePath, ".tmp-tarball-extract")
		os.MkdirAll(tmpDir, 0755)
		defer os.RemoveAll(tmpDir)

		cmd := exec.Command("tar", "-xzf", tmpPath, "-C", tmpDir)
		if output, err := cmd.CombinedOutput(); err != nil {
			http.Redirect(w, r, "/modules?flash=Extract+failed:+"+string(output), http.StatusSeeOther)
			return
		}

		entries, _ := os.ReadDir(tmpDir)
		if len(entries) == 0 {
			http.Redirect(w, r, "/modules?flash=Tarball+is+empty", http.StatusSeeOther)
			return
		}
		moduleDir := filepath.Join(tmpDir, entries[0].Name())
		mjData, err := os.ReadFile(filepath.Join(moduleDir, "module.json"))
		if err != nil {
			http.Redirect(w, r, "/modules?flash=No+module.json+found+in+tarball", http.StatusSeeOther)
			return
		}
		var mj struct {
			ID           string `json:"id"`
			Capabilities struct {
				ComponentType string `json:"component_type"`
			} `json:"capabilities"`
		}
		json.Unmarshal(mjData, &mj)

		componentType := mj.Capabilities.ComponentType
		if componentType == "" {
			componentType = "other"
		}

		categoryDir := filepath.Join(app.basePath, "modules", getInstallSubdir(componentType))
		os.MkdirAll(categoryDir, 0755)
		destDir := filepath.Join(categoryDir, entries[0].Name())
		os.RemoveAll(destDir)
		if err := os.Rename(moduleDir, destDir); err != nil {
			http.Redirect(w, r, "/modules?flash=Move+failed:+"+err.Error(), http.StatusSeeOther)
			return
		}

		app.logger.Info("tarball module installed", "id", mj.ID, "path", destDir)
		http.Redirect(w, r, "/modules?flash=Installed+"+entries[0].Name()+"+from+tarball", http.StatusSeeOther)

	default:
		http.Redirect(w, r, "/modules?flash=Unknown+install+source", http.StatusSeeOther)
	}
}

// -- API (JSON) --

func (app *App) handleAPIModules(w http.ResponseWriter, r *http.Request) {
	cat, err := app.catalogSvc.Fetch()
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadGateway)
		return
	}
	installed := discoverInstalledModules(app.basePath)
	type apiModule struct {
		CatalogModule
		Installed        bool   `json:"installed"`
		InstalledVersion string `json:"installed_version,omitempty"`
	}
	var result []apiModule
	for _, m := range cat.Modules {
		am := apiModule{CatalogModule: m}
		if inst, ok := installed[m.ID]; ok {
			am.Installed = true
			am.InstalledVersion = inst.Version
		}
		result = append(result, am)
	}
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(result)
}

func (app *App) handleAPIModuleInstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	app.logger.Info("API module install", "id", id)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "started", "module": id})
}

// -- Module Assets --

func (app *App) handleModuleAssets(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	modDir := app.findModuleDir(id)
	if modDir == "" {
		http.NotFound(w, r)
		return
	}
	entries, _ := app.fileSvc.ListDir(modDir)
	data := map[string]any{
		"Title":    id + " Assets",
		"ModuleID": id,
		"Dir":      modDir,
		"Entries":  entries,
		"ModDir":   modDir,
		"Active":   "modules",
	}
	app.render(w, r, "files.html", data)
}

func (app *App) handleModuleAssetUpload(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	dest := r.FormValue("path")
	if dest == "" {
		http.Error(w, "missing path", http.StatusBadRequest)
		return
	}
	if _, err := app.fileSvc.validate(dest); err != nil {
		http.Error(w, "Forbidden: "+err.Error(), http.StatusForbidden)
		return
	}
	file, header, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "missing file", http.StatusBadRequest)
		return
	}
	defer file.Close()

	target := filepath.Join(dest, header.Filename)
	out, err := os.Create(target)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer out.Close()
	io.Copy(out, file)

	app.logger.Info("asset uploaded", "module", id, "file", target)
	http.Redirect(w, r, fmt.Sprintf("/modules/%s/assets?flash=Uploaded+%s", id, header.Filename), http.StatusSeeOther)
}

func (app *App) handleModuleAssetUploadSlot(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	targetFilename := r.PathValue("filename")
	modDir := app.findModuleDir(id)
	if modDir == "" {
		http.NotFound(w, r)
		return
	}

	installed := discoverInstalledModules(app.basePath)
	inst, ok := installed[id]
	if !ok || inst.Assets == nil {
		http.Error(w, "module has no asset configuration", http.StatusBadRequest)
		return
	}

	assetsDir := modDir
	if inst.Assets.Path != "" && inst.Assets.Path != "." {
		assetsDir = filepath.Join(modDir, inst.Assets.Path)
	}

	os.MkdirAll(assetsDir, 0755)

	file, _, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "missing file", http.StatusBadRequest)
		return
	}
	defer file.Close()

	target := filepath.Join(assetsDir, targetFilename)
	if !strings.HasPrefix(filepath.Clean(target), filepath.Clean(assetsDir)) {
		http.Error(w, "invalid filename", http.StatusForbidden)
		return
	}

	out, err := os.Create(target)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer out.Close()
	io.Copy(out, file)

	app.logger.Info("asset slot upload", "module", id, "file", target)
	http.Redirect(w, r, fmt.Sprintf("/modules/%s?flash=Uploaded+%s", id, targetFilename), http.StatusSeeOther)
}

func (app *App) handleModuleAssetDelete(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	targetFilename := r.PathValue("filename")
	modDir := app.findModuleDir(id)
	if modDir == "" {
		http.NotFound(w, r)
		return
	}

	installed := discoverInstalledModules(app.basePath)
	inst, ok := installed[id]
	if !ok || inst.Assets == nil {
		http.Error(w, "module has no asset configuration", http.StatusBadRequest)
		return
	}

	assetsDir := modDir
	if inst.Assets.Path != "" && inst.Assets.Path != "." {
		assetsDir = filepath.Join(modDir, inst.Assets.Path)
	}

	target := filepath.Join(assetsDir, targetFilename)
	if !strings.HasPrefix(filepath.Clean(target), filepath.Clean(assetsDir)) {
		http.Error(w, "invalid filename", http.StatusForbidden)
		return
	}

	if err := os.Remove(target); err != nil {
		app.logger.Error("asset delete failed", "module", id, "file", target, "err", err)
	} else {
		app.logger.Info("asset deleted", "module", id, "file", target)
	}

	http.Redirect(w, r, fmt.Sprintf("/modules/%s?flash=Deleted+%s", id, targetFilename), http.StatusSeeOther)
}

// -- Files --

func (app *App) handleFiles(w http.ResponseWriter, r *http.Request) {
	dir := r.URL.Query().Get("path")
	if dir == "" {
		dir = app.fileSvc.AllowedRoots[0]
	}
	entries, err := app.fileSvc.ListDir(dir)
	if err != nil {
		app.logger.Error("list dir", "path", dir, "err", err)
		http.Error(w, "Could not list directory: "+err.Error(), http.StatusForbidden)
		return
	}

	// Build breadcrumbs.
	type crumb struct {
		Name string
		Path string
	}
	var crumbs []crumb
	parts := strings.Split(strings.Trim(dir, "/"), "/")
	for i := range parts {
		p := "/" + strings.Join(parts[:i+1], "/")
		crumbs = append(crumbs, crumb{Name: parts[i], Path: p})
	}

	// Compute parent directory (empty if at an allowed root).
	parentDir := ""
	cleanDir := filepath.Clean(dir)
	for _, root := range app.fileSvc.AllowedRoots {
		if cleanDir != filepath.Clean(root) && strings.HasPrefix(cleanDir, filepath.Clean(root)) {
			parentDir = filepath.Dir(cleanDir)
			break
		}
	}

	data := map[string]any{
		"Title":       "Files",
		"Dir":         dir,
		"ParentDir":   parentDir,
		"Entries":     entries,
		"Breadcrumbs": crumbs,
		"Flash":       r.URL.Query().Get("flash"),
		"Active":      "files",
	}
	app.render(w, r, "files.html", data)
}

func (app *App) handleFileUpload(w http.ResponseWriter, r *http.Request) {
	// Ensure multipart is parsed for drag-drop uploads.
	if r.MultipartForm == nil {
		r.ParseMultipartForm(64 << 20)
	}
	dest := r.FormValue("path")
	if dest == "" {
		app.logger.Error("upload: missing path", "content-type", r.Header.Get("Content-Type"), "form", r.Form)
		http.Error(w, "missing destination", http.StatusBadRequest)
		return
	}
	file, header, err := r.FormFile("file")
	if err != nil {
		app.logger.Error("upload: missing file", "err", err, "path", dest)
		http.Error(w, "missing file field", http.StatusBadRequest)
		return
	}
	defer file.Close()

	if _, err := app.fileSvc.validate(dest); err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}

	target := filepath.Join(dest, header.Filename)
	out, err := os.Create(target)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	defer out.Close()
	io.Copy(out, file)

	app.logger.Info("file uploaded", "target", target)
	// For AJAX/fetch requests, return 200 instead of redirect.
	if r.Header.Get("X-CSRF-Token") != "" {
		w.WriteHeader(http.StatusOK)
		fmt.Fprintf(w, "Uploaded %s", header.Filename)
		return
	}
	http.Redirect(w, r, "/files?path="+dest+"&flash=Uploaded+"+header.Filename, http.StatusSeeOther)
}

func (app *App) handleFileDownload(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	clean, err := app.fileSvc.validate(path)
	if err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	info, err := os.Stat(clean)
	if err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Disposition", "attachment; filename=\""+filepath.Base(clean)+"\"")
	http.ServeFile(w, r, clean)
}

func (app *App) handleFileMkdir(w http.ResponseWriter, r *http.Request) {
	parent := r.FormValue("path")
	name := r.FormValue("name")
	if parent == "" || name == "" {
		http.Error(w, "missing path or name", http.StatusBadRequest)
		return
	}
	target := filepath.Join(parent, name)
	if _, err := app.fileSvc.validate(target); err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	if err := os.MkdirAll(target, 0755); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	app.logger.Info("mkdir", "path", target)
	if r.Header.Get("X-CSRF-Token") != "" {
		w.WriteHeader(http.StatusOK)
		return
	}
	http.Redirect(w, r, "/files?path="+parent+"&flash=Created+"+name, http.StatusSeeOther)
}

func (app *App) handleFileRename(w http.ResponseWriter, r *http.Request) {
	oldPath := r.FormValue("path")
	newName := r.FormValue("name")
	if oldPath == "" || newName == "" {
		http.Error(w, "missing path or name", http.StatusBadRequest)
		return
	}
	clean, err := app.fileSvc.validate(oldPath)
	if err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	newPath := filepath.Join(filepath.Dir(clean), newName)
	if _, err := app.fileSvc.validate(newPath); err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	if err := os.Rename(clean, newPath); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	app.logger.Info("rename", "from", clean, "to", newPath)
	dir := filepath.Dir(clean)
	http.Redirect(w, r, "/files?path="+dir+"&flash=Renamed", http.StatusSeeOther)
}

func (app *App) handleFileDelete(w http.ResponseWriter, r *http.Request) {
	path := r.FormValue("path")
	clean, err := app.fileSvc.validate(path)
	if err != nil {
		http.Error(w, "Forbidden", http.StatusForbidden)
		return
	}
	if err := os.RemoveAll(clean); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	app.logger.Info("delete", "path", clean)
	if r.Header.Get("X-CSRF-Token") != "" {
		w.WriteHeader(http.StatusOK)
		return
	}
	dir := filepath.Dir(clean)
	http.Redirect(w, r, "/files?path="+dir+"&flash=Deleted", http.StatusSeeOther)
}

// -- Config --

// readJSONFile reads a JSON file into a map, returning an empty map if missing or invalid.
func readJSONFile(path string) map[string]any {
	data, err := os.ReadFile(path)
	if err != nil {
		return map[string]any{}
	}
	var m map[string]any
	if err := json.Unmarshal(data, &m); err != nil {
		return map[string]any{}
	}
	return m
}

// writeJSONFile writes a map as pretty-printed JSON to path, creating parent dirs.
func writeJSONFile(path string, m map[string]any) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	pretty, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, append(pretty, '\n'), 0644)
}

// jsonBool extracts a bool from a map key, with a default.
func jsonBool(m map[string]any, key string, def bool) bool {
	v, ok := m[key]
	if !ok {
		return def
	}
	switch b := v.(type) {
	case bool:
		return b
	default:
		return def
	}
}

// jsonFloat extracts a float64 from a map key, with a default.
func jsonFloat(m map[string]any, key string, def float64) float64 {
	v, ok := m[key]
	if !ok {
		return def
	}
	switch f := v.(type) {
	case float64:
		return f
	default:
		return def
	}
}

// loadSettingsSchema reads and parses the settings-schema.json file.
func (app *App) loadSettingsSchema() ([]SettingsSection, error) {
	schemaPath := filepath.Join(app.basePath, "shared", "settings-schema.json")
	data, err := os.ReadFile(schemaPath)
	if err != nil {
		return nil, fmt.Errorf("reading settings schema: %w", err)
	}
	var sections []SettingsSection
	if err := json.Unmarshal(data, &sections); err != nil {
		return nil, fmt.Errorf("parsing settings schema: %w", err)
	}
	return sections, nil
}

// settingsKeyMapping maps schema keys to their config file keys.
// Keys not listed here are assumed to match 1:1 with shadow_config.json.
var settingsToShadowConfig = map[string]string{
	"overlay_knobs":          "overlay_knobs_mode",
	"screen_reader_debounce": "tts_debounce_ms",
	"resample_bridge":        "resample_bridge_mode",
	"link_audio_publish":     "link_audio_publish",
	"pad_typing":             "pad_typing",
	"text_preview":           "text_preview",
	"browser_preview":        "browser_preview",
	"auto_update_check":      "auto_update_check",
	"filebrowser_enabled":    "filebrowser_enabled",
	"screen_reader_enabled":  "screen_reader_enabled",
	"screen_reader_engine":   "screen_reader_engine",
	"screen_reader_speed":    "screen_reader_speed",
	"screen_reader_pitch":    "screen_reader_pitch",
	"screen_reader_volume":   "screen_reader_volume",
}

// settingsToFeatures maps schema keys to features.json keys.
var settingsToFeatures = map[string]string{
	"display_mirror":    "display_mirror_enabled",
	"set_pages_enabled": "set_pages_enabled",
	"link_audio_routing": "link_audio_enabled",
	"skipback_shortcut": "skipback_require_volume",
}

func (app *App) handleConfig(w http.ResponseWriter, r *http.Request) {
	sections, err := app.loadSettingsSchema()
	if err != nil {
		app.logger.Error("failed to load settings schema", "err", err)
		http.Error(w, "Failed to load settings schema", http.StatusInternalServerError)
		return
	}

	shadowPath := filepath.Join(app.basePath, "shadow_config.json")
	featuresPath := filepath.Join(app.basePath, "config", "features.json")

	sc := readJSONFile(shadowPath)
	ft := readJSONFile(featuresPath)

	// Build merged values map using schema keys.
	values := make(map[string]any)

	// Map features.json into schema keys.
	for schemaKey, featKey := range settingsToFeatures {
		if schemaKey == "skipback_shortcut" {
			// skipback_require_volume (bool) -> skipback_shortcut (0 or 1)
			if jsonBool(ft, featKey, false) {
				values[schemaKey] = float64(1)
			} else {
				values[schemaKey] = float64(0)
			}
		} else if schemaKey == "link_audio_routing" {
			// link_audio_enabled -> link_audio_routing (bool)
			values[schemaKey] = jsonBool(ft, featKey, false)
		} else {
			values[schemaKey] = jsonBool(ft, featKey, false)
		}
	}

	// Map shadow_config.json into schema keys.
	for schemaKey, scKey := range settingsToShadowConfig {
		if v, ok := sc[scKey]; ok {
			values[schemaKey] = v
		}
	}

	data := map[string]any{
		"Title":    "Settings",
		"Flash":    r.URL.Query().Get("flash"),
		"Active":   "config",
		"Sections": sections,
		"Values":   values,
	}
	app.render(w, r, "config.html", data)
}

func (app *App) handleConfigValues(w http.ResponseWriter, r *http.Request) {
	sections, _ := app.loadSettingsSchema()
	shadowConfig := readJSONFile(filepath.Join(app.basePath, "shadow_config.json"))
	features := readJSONFile(filepath.Join(app.basePath, "config", "features.json"))

	values := make(map[string]any)
	for _, section := range sections {
		for _, item := range section.Items {
			configKey := item.Key
			if mapped, ok := settingsToShadowConfig[item.Key]; ok {
				configKey = mapped
			}
			if v, ok := shadowConfig[configKey]; ok {
				values[item.Key] = v
			}
			if featKey, ok := settingsToFeatures[item.Key]; ok {
				if v, ok := features[featKey]; ok {
					values[item.Key] = v
				}
			}
		}
	}

	// Overlay with live values from shared memory (source of truth for device state).
	if app.shm != nil {
		values["display_mirror"] = app.shm.DisplayMirror()
		values["overlay_knobs"] = float64(app.shm.OverlayKnobsMode())
		values["screen_reader_enabled"] = app.shm.TTSEnabled()
		if app.shm.TTSEngine() == 1 {
			values["screen_reader_engine"] = "flite"
		} else {
			values["screen_reader_engine"] = "espeak"
		}
		values["screen_reader_speed"] = float64(app.shm.TTSSpeed())
		values["screen_reader_pitch"] = float64(app.shm.TTSPitch())
		values["screen_reader_volume"] = float64(app.shm.TTSVolume())
		values["screen_reader_debounce"] = float64(app.shm.TTSDebounce())
		values["set_pages_enabled"] = app.shm.SetPagesEnabled()
		values["skipback_shortcut"] = float64(boolToInt(app.shm.SkipbackRequireVolume()))
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(values)
}

func boolToInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func (app *App) handleConfigSetSetting(w http.ResponseWriter, r *http.Request) {
	key := r.FormValue("key")
	value := r.FormValue("value")
	if key == "" {
		http.Error(w, `{"ok":false,"error":"missing key"}`, http.StatusBadRequest)
		return
	}

	sections, err := app.loadSettingsSchema()
	if err != nil {
		app.logger.Error("failed to load settings schema for set", "err", err)
		http.Error(w, `{"ok":false,"error":"schema load error"}`, http.StatusInternalServerError)
		return
	}

	// Find the schema item for this key so we know the type.
	var item *SettingsItem
	for _, section := range sections {
		for i := range section.Items {
			if section.Items[i].Key == key {
				item = &section.Items[i]
				break
			}
		}
		if item != nil {
			break
		}
	}
	if item == nil {
		http.Error(w, `{"ok":false,"error":"unknown setting key"}`, http.StatusBadRequest)
		return
	}

	shadowPath := filepath.Join(app.basePath, "shadow_config.json")
	featuresPath := filepath.Join(app.basePath, "config", "features.json")

	if featKey, isFeat := settingsToFeatures[key]; isFeat {
		// Feature flag — read, update, write features.json.
		ft := readJSONFile(featuresPath)
		switch key {
		case "skipback_shortcut":
			val, _ := strconv.Atoi(value)
			ft[featKey] = val != 0
		default:
			ft[featKey] = value == "true"
		}
		if err := writeJSONFile(featuresPath, ft); err != nil {
			http.Error(w, `{"ok":false,"error":"write failed"}`, http.StatusInternalServerError)
			return
		}
		// Also write to shadow_config.json for live sync with shadow UI.
		sc := readJSONFile(shadowPath)
		sc[key] = value == "true"
		writeJSONFile(shadowPath, sc)
	} else {
		// Shadow config — read, update, write shadow_config.json.
		sc := readJSONFile(shadowPath)
		scKey := key
		if mapped, ok := settingsToShadowConfig[key]; ok {
			scKey = mapped
		}
		switch item.Type {
		case "bool":
			sc[scKey] = value == "true"
		case "enum":
			if n, err := strconv.ParseFloat(value, 64); err == nil {
				sc[scKey] = n
			} else {
				sc[scKey] = value
			}
		case "int":
			n, _ := strconv.Atoi(value)
			sc[scKey] = n
		case "float":
			f, _ := strconv.ParseFloat(value, 64)
			sc[scKey] = f
		}
		if err := writeJSONFile(shadowPath, sc); err != nil {
			http.Error(w, `{"ok":false,"error":"write failed"}`, http.StatusInternalServerError)
			return
		}

		// Sync filebrowser flag file (checked by shim-entrypoint.sh at boot).
		if key == "filebrowser_enabled" {
			flagPath := filepath.Join(app.basePath, "filebrowser_enabled")
			if value == "true" {
				os.WriteFile(flagPath, []byte("1"), 0644)
			} else {
				os.Remove(flagPath)
			}
		}
	}

	app.logger.Info("config setting updated", "key", key, "value", value)

	// Apply to shared memory for instant effect (no JS tick() involvement).
	app.applyShmSetting(key, value)

	// JSON response for AJAX callers.
	if r.Header.Get("X-CSRF-Token") != "" {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"ok":true}`))
		return
	}
	// Fallback redirect for non-AJAX.
	http.Redirect(w, r, "/config", http.StatusSeeOther)
}

// applyShmSetting writes a config setting directly to shared memory for
// instant effect. This bypasses the JS tick() path entirely, avoiding the
// SIGABRT that occurred when syncSettingsFromConfigFile() was called from tick().
func (app *App) applyShmSetting(key, value string) {
	if app.shm == nil {
		return
	}
	switch key {
	case "display_mirror":
		app.shm.SetDisplayMirror(value == "true")
	case "overlay_knobs":
		if v, err := strconv.Atoi(value); err == nil {
			app.shm.SetOverlayKnobsMode(uint8(v))
		}
	case "screen_reader_enabled":
		app.shm.SetTTSEnabled(value == "true")
	case "screen_reader_engine":
		if value == "flite" {
			app.shm.SetTTSEngine(1)
		} else {
			app.shm.SetTTSEngine(0)
		}
	case "screen_reader_speed":
		if v, err := strconv.ParseFloat(value, 32); err == nil {
			app.shm.SetTTSSpeed(float32(v))
		}
	case "screen_reader_pitch":
		if v, err := strconv.Atoi(value); err == nil {
			app.shm.SetTTSPitch(uint16(v))
		}
	case "screen_reader_volume":
		if v, err := strconv.Atoi(value); err == nil {
			app.shm.SetTTSVolume(uint8(v))
		}
	case "screen_reader_debounce":
		if v, err := strconv.Atoi(value); err == nil {
			app.shm.SetTTSDebounce(uint16(v))
		}
	case "set_pages_enabled":
		app.shm.SetSetPagesEnabled(value == "true")
	case "skipback_shortcut":
		app.shm.SetSkipbackRequireVolume(value != "0" && value != "false")
	}
}

// -- System --

func (app *App) handleSystem(w http.ResponseWriter, r *http.Request) {
	// Read version.
	verBytes, err := os.ReadFile(filepath.Join(app.basePath, "host", "version.txt"))
	version := "unknown"
	if err == nil {
		version = strings.TrimSpace(string(verBytes))
	}

	// Best-effort catalog fetch for update check.
	var latestVersion string
	var updateAvailable bool
	cat, err := app.catalogSvc.Fetch()
	if err == nil && cat != nil {
		latestVersion = cat.Host.LatestVersion
		updateAvailable = latestVersion != "" && latestVersion != version
	}

	// Disk usage via stat (simplified).
	var diskTotal, diskFree uint64
	var stat syscall.Statfs_t
	if err := syscall.Statfs(app.basePath, &stat); err == nil {
		diskTotal = stat.Blocks * uint64(stat.Bsize)
		diskFree = stat.Bavail * uint64(stat.Bsize)
	}

	data := map[string]any{
		"Title":          "System",
		"Version":        version,
		"LatestVersion":  latestVersion,
		"UpdateAvailable": updateAvailable,
		"DiskTotal":      int64(diskTotal),
		"DiskFree":       int64(diskFree),
		"DiskUsed":       int64(diskTotal - diskFree),
		"DiskPercent":    0,
		"Flash":          r.URL.Query().Get("flash"),
		"Active":         "system",
	}
	if diskTotal > 0 {
		data["DiskPercent"] = int((diskTotal - diskFree) * 100 / diskTotal)
	}
	app.render(w, r, "system.html", data)
}

func (app *App) handleSystemCheckUpdate(w http.ResponseWriter, r *http.Request) {
	cat, err := app.catalogSvc.Fetch()
	if err != nil {
		http.Redirect(w, r, "/system?flash=Failed+to+check:+"+err.Error(), http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/system?flash=Latest+version:+"+cat.Host.LatestVersion, http.StatusSeeOther)
}

func (app *App) setUpgradeStatus(status string) {
	app.upgradeStatus = status
	// Write to file for shadow UI to display on OLED.
	statusPath := filepath.Join(app.basePath, "upgrade_status")
	if status == "" {
		os.Remove(statusPath)
	} else {
		os.WriteFile(statusPath, []byte(status), 0644)
	}
	app.logger.Info("upgrade status", "step", status)
}

func (app *App) handleUpgradeStatus(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": app.upgradeStatus})
}

func (app *App) handleSystemUpgrade(w http.ResponseWriter, r *http.Request) {
	app.logger.Info("system upgrade requested")

	// 1. Fetch catalog.
	cat, err := app.catalogSvc.Fetch()
	if err != nil {
		http.Redirect(w, r, "/system?flash=Failed+to+fetch+catalog:+"+err.Error(), http.StatusSeeOther)
		return
	}

	// 2. Compare versions.
	verBytes, _ := os.ReadFile(filepath.Join(app.basePath, "host", "version.txt"))
	installedVersion := strings.TrimSpace(string(verBytes))
	latestVersion := cat.Host.LatestVersion
	downloadURL := cat.Host.DownloadURL

	if latestVersion != "" && latestVersion == installedVersion {
		http.Redirect(w, r, "/system?flash=Already+up+to+date+("+installedVersion+")", http.StatusSeeOther)
		return
	}

	if downloadURL == "" {
		http.Redirect(w, r, "/system?flash=No+download+URL+in+catalog", http.StatusSeeOther)
		return
	}

	// 3. Download the tarball.
	tarPath := filepath.Join(app.basePath, "schwung-upgrade.tar.gz")
	app.logger.Info("downloading upgrade", "url", downloadURL, "dest", tarPath)

	client := &http.Client{Timeout: 300 * time.Second}
	dlResp, err := client.Get(downloadURL)
	if err != nil {
		http.Redirect(w, r, "/system?flash=Download+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	defer dlResp.Body.Close()
	if dlResp.StatusCode != http.StatusOK {
		http.Redirect(w, r, "/system?flash=Download+returned+"+strconv.Itoa(dlResp.StatusCode), http.StatusSeeOther)
		return
	}

	tarFile, err := os.Create(tarPath)
	if err != nil {
		http.Redirect(w, r, "/system?flash=Failed+to+create+file:+"+err.Error(), http.StatusSeeOther)
		return
	}
	if _, err := io.Copy(tarFile, dlResp.Body); err != nil {
		tarFile.Close()
		os.Remove(tarPath)
		http.Redirect(w, r, "/system?flash=Download+write+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	tarFile.Close()

	app.logger.Info("download complete, starting upgrade", "version", latestVersion)

	// 4. Kick off upgrade in background goroutine.
	// Follows the same flow as the on-device Module Store:
	// extract tarball → chmod u+s shim → run post-update.sh → restart Move.
	go func() {
		// Wait for HTTP response to be sent.
		time.Sleep(2 * time.Second)

		app.setUpgradeStatus("Extracting update...")

		// Extract tarball (same --strip-components=1 as Module Store).
		extractCmd := exec.Command("tar", "-xzf", tarPath, "-C", app.basePath, "--strip-components=1")
		if output, err := extractCmd.CombinedOutput(); err != nil {
			app.setUpgradeStatus("Extract failed")
			app.logger.Error("extract failed", "err", err, "output", string(output))
			return
		}

		app.setUpgradeStatus("Configuring...")

		// Restore setuid bit on shim (required for LD_PRELOAD under AT_SECURE).
		shimPath := filepath.Join(app.basePath, "schwung-shim.so")
		exec.Command("chmod", "u+s", shimPath).Run()

		// Run post-update.sh (symlinks, permissions, entrypoint update).
		postUpdate := filepath.Join(app.basePath, "scripts", "post-update.sh")
		postCmd := exec.Command("sh", postUpdate)
		postCmd.Dir = app.basePath
		if output, err := postCmd.CombinedOutput(); err != nil {
			app.setUpgradeStatus("Post-update failed")
			app.logger.Error("post-update failed", "err", err, "output", string(output))
		}

		// Clean up tarball.
		os.Remove(tarPath)

		app.setUpgradeStatus("Restarting...")

		// Restart Move to pick up new binaries.
		restartScript := filepath.Join(app.basePath, "restart-move.sh")
		if _, err := os.Stat(restartScript); err == nil {
			exec.Command("sh", restartScript).Run()
		} else {
			exec.Command("killall", "MoveOriginal", "MoveLauncher").Run()
		}
	}()

	// 5. Return inline HTML restarting page.
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(http.StatusOK)
	app.setUpgradeStatus("Downloading...")

	fmt.Fprintf(w, `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Upgrading Schwung...</title>
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
         display: flex; justify-content: center; align-items: center; min-height: 100vh;
         margin: 0; background: #1a1a2e; color: #e0e0e0; }
  .container { text-align: center; max-width: 480px; padding: 2rem; }
  h1 { margin-bottom: 0.5rem; }
  .spinner { display: inline-block; width: 48px; height: 48px; border: 4px solid #444;
             border-top-color: #6c63ff; border-radius: 50%%;
             animation: spin 1s linear infinite; margin: 1.5rem 0; }
  @keyframes spin { to { transform: rotate(360deg); } }
  .status { color: #aaa; font-size: 0.9rem; }
  .steps { list-style: none; padding: 0; margin: 1rem 0; text-align: left; display: inline-block; }
  .steps li { padding: 0.25rem 0; color: #666; }
  .steps li.done { color: #4caf50; }
  .steps li.done::before { content: "\2713 "; }
  .steps li.active { color: #e0e0e0; }
  .steps li.active::before { content: "\25B6 "; color: #6c63ff; }
  .steps li.pending::before { content: "\25CB "; }
  #error { display: none; color: #ff6b6b; margin-top: 1rem; }
</style>
</head>
<body>
<div class="container">
  <h1>Upgrading to %s</h1>
  <div class="spinner"></div>
  <ul class="steps">
    <li id="s-download" class="active">Downloading update...</li>
    <li id="s-extract" class="pending">Extracting files</li>
    <li id="s-configure" class="pending">Configuring</li>
    <li id="s-restart" class="pending">Restarting</li>
  </ul>
  <p id="error">The server did not come back. You may need to check the device manually.</p>
</div>
<script>
(function() {
  var start = Date.now();
  var timeout = 180000;
  var steps = {
    "Downloading...": "s-download",
    "Extracting update...": "s-extract",
    "Configuring...": "s-configure",
    "Restarting...": "s-restart"
  };
  var order = ["s-download", "s-extract", "s-configure", "s-restart"];
  var serverDown = false;

  function setStep(id) {
    var idx = order.indexOf(id);
    for (var i = 0; i < order.length; i++) {
      var el = document.getElementById(order[i]);
      if (i < idx) el.className = "done";
      else if (i === idx) el.className = "active";
      else el.className = "pending";
    }
  }

  function pollStatus() {
    fetch("/system/upgrade-status", {cache: "no-store"})
      .then(function(r) { return r.json(); })
      .then(function(data) {
        if (data.status && steps[data.status]) {
          setStep(steps[data.status]);
        }
      })
      .catch(function() {
        // Server went down — it's restarting
        if (!serverDown) {
          serverDown = true;
          for (var i = 0; i < order.length; i++) {
            document.getElementById(order[i]).className = "done";
          }
        }
      });
  }

  function pollReady() {
    if (Date.now() - start > timeout) {
      document.getElementById("error").style.display = "block";
      return;
    }
    fetch("/system", {method: "GET", cache: "no-store"})
      .then(function(r) { if (r.ok) window.location.href = "/system?flash=Upgrade+complete"; else setTimeout(pollReady, 2000); })
      .catch(function() { setTimeout(pollReady, 2000); });
  }

  setInterval(pollStatus, 1000);
  setTimeout(pollReady, 8000);
})();
</script>
</body>
</html>`, latestVersion)
}

func (app *App) handleSystemLogs(w http.ResponseWriter, r *http.Request) {
	logPath := filepath.Join(app.basePath, "debug.log")
	content, err := os.ReadFile(logPath)
	if err != nil {
		content = []byte("(no log file found)")
	}
	// Return last 200 lines.
	lines := strings.Split(string(content), "\n")
	if len(lines) > 200 {
		lines = lines[len(lines)-200:]
	}
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	fmt.Fprint(w, strings.Join(lines, "\n"))
}

// -- Help --

func (app *App) handleHelp(w http.ResponseWriter, r *http.Request) {
	var sections []HelpNode

	// Load main help_content.json (Schwung help).
	helpPath := filepath.Join(app.basePath, "shared", "help_content.json")
	if data, err := os.ReadFile(helpPath); err == nil {
		var hc HelpContent
		if json.Unmarshal(data, &hc) == nil {
			for _, s := range hc.Sections {
				// Skip "Move Manual" and "Notice" — only include Schwung help
				if s.Title == "Move Manual" || s.Title == "Notice" {
					continue
				}
				sections = append(sections, s)
			}
		}
	}

	// Scan installed modules for help.json files.
	installed := discoverInstalledModules(app.basePath)
	// Collect module help in a "Modules" section.
	var moduleHelp []HelpNode
	// Sort module IDs for stable ordering.
	ids := make([]string, 0, len(installed))
	for id := range installed {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		mod := installed[id]
		modDir := app.findModuleDir(id)
		if modDir == "" {
			continue
		}
		helpFile := filepath.Join(modDir, "help.json")
		data, err := os.ReadFile(helpFile)
		if err != nil {
			continue
		}
		var node HelpNode
		if json.Unmarshal(data, &node) == nil {
			// Use the module's display name if the help title matches
			if node.Title == "" {
				node.Title = mod.Name
			}
			moduleHelp = append(moduleHelp, node)
		}
	}
	if len(moduleHelp) > 0 {
		sections = append(sections, HelpNode{
			Title:    "Modules",
			Children: moduleHelp,
		})
	}

	data := map[string]any{
		"Title":    "Help",
		"Sections": sections,
		"Active":   "help",
	}
	app.render(w, r, "help.html", data)
}

// -- Install --

func (app *App) handleInstallPage(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	cat, _ := app.catalogSvc.Fetch()
	var mod *CatalogModule
	if cat != nil {
		for i := range cat.Modules {
			if cat.Modules[i].ID == id {
				mod = &cat.Modules[i]
				break
			}
		}
	}
	if mod == nil {
		http.NotFound(w, r)
		return
	}
	installed := discoverInstalledModules(app.basePath)
	data := map[string]any{
		"Title":     "Install " + mod.Name,
		"Module":    mod,
		"Installed": installed,
		"Active":    "modules",
	}
	app.render(w, r, "install.html", data)
}

func (app *App) handleInstallAction(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	mod := app.findCatalogModule(id)
	if mod == nil {
		http.Redirect(w, r, "/modules?flash=Module+not+found:+"+id, http.StatusSeeOther)
		return
	}
	if err := app.installModule(mod); err != nil {
		app.logger.Error("install failed", "id", id, "err", err)
		http.Redirect(w, r, "/modules/"+id+"?flash=Install+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/modules/"+id+"?flash="+mod.Name+"+installed+successfully", http.StatusSeeOther)
}

// -- Remote UI --

func (app *App) handleRemoteUI(w http.ResponseWriter, r *http.Request) {
	app.render(w, r, "remote_ui.html", map[string]any{
		"Title":  "Remote UI",
		"Active": "remote-ui",
	})
}

// handleModuleWebUIAsset serves static files from a module's install directory.
// Used by custom module web UIs loaded in an iframe on the Remote UI page.
func (app *App) handleModuleWebUIAsset(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	fp := r.PathValue("filepath")

	// Validate module ID and filepath to prevent directory traversal.
	if id == "" || fp == "" ||
		strings.Contains(id, "..") || strings.Contains(id, "/") || strings.Contains(id, "\\") ||
		strings.Contains(fp, "..") {
		http.NotFound(w, r)
		return
	}

	modDir := app.findModuleDir(id)
	if modDir == "" {
		http.NotFound(w, r)
		return
	}

	fullPath := filepath.Join(modDir, fp)

	// Ensure resolved path is within the module directory.
	resolved, err := filepath.EvalSymlinks(fullPath)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	resolvedDir, err := filepath.EvalSymlinks(modDir)
	if err != nil {
		http.NotFound(w, r)
		return
	}
	if !strings.HasPrefix(resolved, resolvedDir+string(filepath.Separator)) && resolved != resolvedDir {
		http.NotFound(w, r)
		return
	}

	// Only serve files, not directories.
	info, err := os.Stat(fullPath)
	if err != nil || info.IsDir() {
		http.NotFound(w, r)
		return
	}

	http.ServeFile(w, r, fullPath)
}

// hostRouter routes requests based on the Host header.
// schwungHost requests go to schwungHandler (with /mirror proxied to displayAddr).
// All other hosts are reverse-proxied to moveAddr (stock Move server).
func hostRouter(schwungHost string, schwungHandler http.Handler, moveAddr, displayAddr string, logger *slog.Logger) http.Handler {
	moveProxy := &httputil.ReverseProxy{
		Director: func(req *http.Request) {
			req.URL.Scheme = "http"
			req.URL.Host = moveAddr
		},
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
			logger.Error("move proxy error", "err", err, "path", r.URL.Path)
			http.Error(w, "Stock Move server unavailable", http.StatusBadGateway)
		},
	}

	displayProxy := &httputil.ReverseProxy{
		Director: func(req *http.Request) {
			req.URL.Scheme = "http"
			req.URL.Host = displayAddr
			// Strip /mirror prefix for proxied requests
			if strings.HasPrefix(req.URL.Path, "/mirror") {
				req.URL.Path = strings.TrimPrefix(req.URL.Path, "/mirror")
				if req.URL.Path == "" {
					req.URL.Path = "/"
				}
			}
			// /stream-auto passes through as-is (no prefix to strip)
		},
		// SSE streams require immediate flushing
		FlushInterval: -1,
		ErrorHandler: func(w http.ResponseWriter, r *http.Request, err error) {
			logger.Error("display proxy error", "err", err, "path", r.URL.Path)
			http.Error(w, "Display server unavailable", http.StatusBadGateway)
		},
	}

	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host := r.Host
		if h, _, err := net.SplitHostPort(host); err == nil {
			host = h
		}

		if host == schwungHost {
			// /mirror → display server (HTML page and sub-resources)
			if r.URL.Path == "/mirror" || strings.HasPrefix(r.URL.Path, "/mirror/") {
				displayProxy.ServeHTTP(w, r)
				return
			}
			// /stream-auto → display server SSE stream
			// (the display server HTML page uses EventSource('/stream-auto'),
			//  so the browser requests this at the root path)
			if r.URL.Path == "/stream-auto" {
				displayProxy.ServeHTTP(w, r)
				return
			}
			schwungHandler.ServeHTTP(w, r)
			return
		}

		// Everything else → stock Move server
		moveProxy.ServeHTTP(w, r)
	})
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	port := flag.Int("port", 80, "HTTP listen port")
	roots := flag.String("roots", "/data/UserData/", "Comma-separated allowed filesystem roots")
	catalogURL := flag.String("catalog-url",
		"https://raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json",
		"URL for the module catalog JSON")
	schwungHost := flag.String("schwung-host", "schwung.local", "Hostname for Schwung Manager")
	moveBackend := flag.String("move-backend", "127.0.0.1:8080", "Address of stock Move web server")
	displayBackend := flag.String("display-backend", "127.0.0.1:7681", "Address of display server")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{Level: slog.LevelInfo}))
	slog.SetDefault(logger)

	allowedRoots := strings.Split(*roots, ",")
	for i := range allowedRoots {
		allowedRoots[i] = strings.TrimSpace(allowedRoots[i])
	}

	// Determine Schwung base path.
	basePath := "/data/UserData/schwung"
	for _, r := range allowedRoots {
		candidate := filepath.Join(r, "schwung")
		if info, err := os.Stat(candidate); err == nil && info.IsDir() {
			basePath = candidate
			break
		}
	}

	tmpl, err := loadTemplates()
	if err != nil {
		logger.Error("failed to load templates", "err", err)
		os.Exit(1)
	}

	shm := OpenShmConfig()
	if shm != nil {
		logger.Info("shared memory config: connected")
	} else {
		logger.Info("shared memory config: not available (not on device)")
	}

	shmParams := OpenShmParams()
	if shmParams != nil {
		logger.Info("shared memory params: connected")
	} else {
		logger.Info("shared memory params: not available (not on device)")
	}

	app := &App{
		tmpl:       tmpl,
		fileSvc:    &FileService{AllowedRoots: allowedRoots},
		catalogSvc: NewCatalogService(*catalogURL),
		basePath:   basePath,
		logger:     logger,
		shm:        shm,
		shmParams:  shmParams,
	}

	mux := http.NewServeMux()

	// Static files.
	staticSub, _ := fs.Sub(staticFS, "static")
	mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(http.FS(staticSub))))

	// Home.
	mux.HandleFunc("GET /{$}", app.handleHome)

	// Modules.
	mux.HandleFunc("GET /modules", app.handleModules)
	mux.HandleFunc("GET /modules/{id}", app.handleModuleDetail)
	mux.HandleFunc("POST /modules/{id}/install", app.handleModuleInstall)
	mux.HandleFunc("POST /modules/{id}/uninstall", app.handleModuleUninstall)
	mux.HandleFunc("POST /modules/{id}/update", app.handleModuleUpdate)
	mux.HandleFunc("POST /modules/update-all", app.handleModuleUpdateAll)
	mux.HandleFunc("POST /modules/install-custom", app.handleCustomInstall)

	// Module assets.
	mux.HandleFunc("GET /modules/{id}/assets", app.handleModuleAssets)
	mux.HandleFunc("POST /modules/{id}/assets/upload", app.handleModuleAssetUpload)
	mux.HandleFunc("POST /modules/{id}/assets/upload/{filename}", app.handleModuleAssetUploadSlot)
	mux.HandleFunc("POST /modules/{id}/assets/delete/{filename}", app.handleModuleAssetDelete)

	// API (JSON).
	mux.HandleFunc("GET /api/modules", app.handleAPIModules)
	mux.HandleFunc("POST /api/modules/{id}/install", app.handleAPIModuleInstall)

	// Files.
	mux.HandleFunc("GET /files", app.handleFiles)
	mux.HandleFunc("POST /files/upload", app.handleFileUpload)
	mux.HandleFunc("GET /files/download", app.handleFileDownload)
	mux.HandleFunc("POST /files/mkdir", app.handleFileMkdir)
	mux.HandleFunc("POST /files/rename", app.handleFileRename)
	mux.HandleFunc("POST /files/delete", app.handleFileDelete)

	// Config.
	mux.HandleFunc("GET /config", app.handleConfig)
	mux.HandleFunc("GET /config/values", app.handleConfigValues)
	mux.HandleFunc("POST /config/set", app.handleConfigSetSetting)

	// System.
	mux.HandleFunc("GET /system", app.handleSystem)
	mux.HandleFunc("POST /system/check-update", app.handleSystemCheckUpdate)
	mux.HandleFunc("POST /system/upgrade", app.handleSystemUpgrade)
	mux.HandleFunc("GET /system/upgrade-status", app.handleUpgradeStatus)
	mux.HandleFunc("GET /system/logs", app.handleSystemLogs)

	// Help.
	mux.HandleFunc("GET /help", app.handleHelp)

	// Remote UI.
	mux.HandleFunc("GET /remote-ui", app.handleRemoteUI)

	// Install.
	mux.HandleFunc("GET /install/{id}", app.handleInstallPage)
	mux.HandleFunc("POST /install/{id}", app.handleInstallAction)

	// Graceful shutdown context — created early so RemoteUI can use it.
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// Remote UI WebSocket (requires shmParams).
	if shmParams != nil {
		remoteUI := NewRemoteUI(shmParams, app.basePath, logger)
		remoteUI.Start(ctx)
		mux.Handle("GET /ws/remote-ui", remoteUI)
	}

	// Module web UI assets (custom web_ui.html and related files).
	mux.HandleFunc("GET /api/remote-ui/module-assets/{id}/{filepath...}", app.handleModuleWebUIAsset)

	// Apply middleware.  WebSocket paths bypass CSRF (upgrades don't carry tokens).
	var handler http.Handler = mux
	handler = middleware.PathTraversalProtection(allowedRoots)(handler)
	handler = middleware.CSRFProtectionWithExemptions(handler, []string{"/ws/"})
	handler = hostRouter(*schwungHost, handler, *moveBackend, *displayBackend, logger)

	addr := fmt.Sprintf(":%d", *port)
	srv := &http.Server{
		Addr:         addr,
		Handler:      handler,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 60 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	// Start mDNS responder for schwung.local.
	startMDNS(*schwungHost, logger)

	fallbackAddr := ":7700"
	go func() {
		// Try the preferred port (80) for up to 30 seconds.
		// If it's unavailable (MoveWebService holding it), fall back to 7700.
		// This ensures schwung-manager is always reachable and never blocks move.local.
		attempts := 0
		maxAttempts := 10 // 10 * 3s = 30s
		for {
			logger.Info("starting schwung-manager", "addr", addr)
			err := srv.ListenAndServe()
			if err == http.ErrServerClosed {
				return
			}
			if err != nil {
				attempts++
				if attempts >= maxAttempts && addr != fallbackAddr {
					logger.Warn("port 80 unavailable after 30s, falling back to 7700")
					addr = fallbackAddr
					srv = &http.Server{
						Addr:         addr,
						Handler:      handler,
						ReadTimeout:  30 * time.Second,
						WriteTimeout: 60 * time.Second,
						IdleTimeout:  120 * time.Second,
					}
					continue
				}
				logger.Error("server bind failed, retrying in 3s", "err", err)
				time.Sleep(3 * time.Second)
				srv = &http.Server{
					Addr:         addr,
					Handler:      handler,
					ReadTimeout:  30 * time.Second,
					WriteTimeout: 60 * time.Second,
					IdleTimeout:  120 * time.Second,
				}
				continue
			}
		}
	}()

	<-ctx.Done()
	logger.Info("shutting down")

	shutdownCtx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		logger.Error("shutdown error", "err", err)
	}
	logger.Info("stopped")
}
