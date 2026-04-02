package main

import (
	"context"
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"html/template"
	"io"
	"io/fs"
	"log/slog"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sort"
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
	Path        string   `json:"path"`
	Label       string   `json:"label"`
	Extensions  []string `json:"extensions"`
	Description string   `json:"description"`
	Hint        string   `json:"hint"`
	Optional    bool     `json:"optional"`
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
}

// templateMap maps page template names to their parsed template sets.
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
		"templates/config_edit.html",
		"templates/system.html",
		"templates/install.html",
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
	tmpl       templateMap
	fileSvc    *FileService
	catalogSvc *CatalogService
	basePath   string // e.g. /data/UserData/schwung
	logger     *slog.Logger
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

	data := map[string]any{
		"Title":        "Modules",
		"Modules":      modules,
		"Installed":    installed,
		"HasInstalled": len(installed) > 0,
		"ReleaseMeta":  app.catalogSvc.GetReleaseMeta(),
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

	data := map[string]any{
		"Title":        mod.Name,
		"Module":       mod,
		"Installed":    installed,
		"ModuleDir":    modDir,
		"AssetsDir":    assetsDir,
		"ModuleAssets": moduleAssets,
		"Active":       "modules",
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

func (app *App) handleModuleInstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	mod := app.findCatalogModule(id)
	if mod == nil {
		http.Redirect(w, r, "/modules?flash=Module+not+found:+"+id, http.StatusSeeOther)
		return
	}
	if err := app.installModule(mod); err != nil {
		app.logger.Error("module install failed", "id", id, "err", err)
		http.Redirect(w, r, "/modules/"+id+"?flash=Install+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/modules/"+id+"?flash="+mod.Name+"+installed+successfully", http.StatusSeeOther)
}

func (app *App) handleModuleUninstall(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if err := app.uninstallModule(id); err != nil {
		app.logger.Error("module uninstall failed", "id", id, "err", err)
		http.Redirect(w, r, "/modules/"+id+"?flash=Uninstall+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/modules/"+id+"?flash=Module+uninstalled", http.StatusSeeOther)
}

func (app *App) handleModuleUpdate(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	mod := app.findCatalogModule(id)
	if mod == nil {
		http.Redirect(w, r, "/modules?flash=Module+not+found:+"+id, http.StatusSeeOther)
		return
	}
	if err := app.installModule(mod); err != nil {
		app.logger.Error("module update failed", "id", id, "err", err)
		http.Redirect(w, r, "/modules/"+id+"?flash=Update+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	http.Redirect(w, r, "/modules/"+id+"?flash="+mod.Name+"+updated+successfully", http.StatusSeeOther)
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

func (app *App) handleConfig(w http.ResponseWriter, r *http.Request) {
	configPath := filepath.Join(app.basePath, "features.json")
	content, err := os.ReadFile(configPath)
	if err != nil {
		content = []byte("{}")
	}
	data := map[string]any{
		"Title":   "Configuration",
		"Config":  string(content),
		"Path":    configPath,
		"Flash":   r.URL.Query().Get("flash"),
		"Active":  "config",
	}
	app.render(w, r, "config.html", data)
}

func (app *App) handleConfigEdit(w http.ResponseWriter, r *http.Request) {
	configPath := filepath.Join(app.basePath, "features.json")
	content, err := os.ReadFile(configPath)
	if err != nil {
		content = []byte("{}")
	}
	data := map[string]any{
		"Title":   "Edit Configuration",
		"Config":  string(content),
		"Path":    configPath,
		"Active":  "config",
	}
	app.render(w, r, "config_edit.html", data)
}

func (app *App) handleConfigSave(w http.ResponseWriter, r *http.Request) {
	configPath := filepath.Join(app.basePath, "features.json")
	content := r.FormValue("content")

	// Validate JSON.
	var js json.RawMessage
	if err := json.Unmarshal([]byte(content), &js); err != nil {
		http.Redirect(w, r, "/config/edit?flash=Invalid+JSON:+"+err.Error(), http.StatusSeeOther)
		return
	}

	// Pretty-print.
	var pretty []byte
	pretty, _ = json.MarshalIndent(js, "", "  ")

	if err := os.WriteFile(configPath, pretty, 0644); err != nil {
		http.Redirect(w, r, "/config/edit?flash=Save+failed:+"+err.Error(), http.StatusSeeOther)
		return
	}
	app.logger.Info("config saved", "path", configPath)
	http.Redirect(w, r, "/config?flash=Configuration+saved", http.StatusSeeOther)
}

// -- System --

func (app *App) handleSystem(w http.ResponseWriter, r *http.Request) {
	// Read version.
	verBytes, err := os.ReadFile(filepath.Join(app.basePath, "host", "version.txt"))
	version := "unknown"
	if err == nil {
		version = strings.TrimSpace(string(verBytes))
	}

	// Disk usage via stat (simplified).
	var diskTotal, diskFree uint64
	var stat syscall.Statfs_t
	if err := syscall.Statfs(app.basePath, &stat); err == nil {
		diskTotal = stat.Blocks * uint64(stat.Bsize)
		diskFree = stat.Bavail * uint64(stat.Bsize)
	}

	data := map[string]any{
		"Title":        "System",
		"Version":      version,
		"DiskTotal":    int64(diskTotal),
		"DiskFree":     int64(diskFree),
		"DiskUsed":     int64(diskTotal - diskFree),
		"DiskPercent":  0,
		"Flash":        r.URL.Query().Get("flash"),
		"Active":       "system",
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

func (app *App) handleSystemUpgrade(w http.ResponseWriter, r *http.Request) {
	app.logger.Info("system upgrade requested")
	http.Redirect(w, r, "/system?flash=Upgrade+started", http.StatusSeeOther)
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
	app.logger.Info("install action", "id", id)
	http.Redirect(w, r, "/modules/"+id+"?flash=Installation+started", http.StatusSeeOther)
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

func main() {
	port := flag.Int("port", 7700, "HTTP listen port")
	roots := flag.String("roots", "/data/UserData/", "Comma-separated allowed filesystem roots")
	catalogURL := flag.String("catalog-url",
		"https://raw.githubusercontent.com/charlesvestal/schwung/main/module-catalog.json",
		"URL for the module catalog JSON")
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

	app := &App{
		tmpl:       tmpl,
		fileSvc:    &FileService{AllowedRoots: allowedRoots},
		catalogSvc: NewCatalogService(*catalogURL),
		basePath:   basePath,
		logger:     logger,
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
	mux.HandleFunc("GET /config/edit", app.handleConfigEdit)
	mux.HandleFunc("POST /config/save", app.handleConfigSave)

	// System.
	mux.HandleFunc("GET /system", app.handleSystem)
	mux.HandleFunc("POST /system/check-update", app.handleSystemCheckUpdate)
	mux.HandleFunc("POST /system/upgrade", app.handleSystemUpgrade)
	mux.HandleFunc("GET /system/logs", app.handleSystemLogs)

	// Install.
	mux.HandleFunc("GET /install/{id}", app.handleInstallPage)
	mux.HandleFunc("POST /install/{id}", app.handleInstallAction)

	// Apply middleware.
	var handler http.Handler = mux
	handler = middleware.PathTraversalProtection(allowedRoots)(handler)
	handler = middleware.CSRFProtection(handler)

	addr := fmt.Sprintf(":%d", *port)
	srv := &http.Server{
		Addr:         addr,
		Handler:      handler,
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 60 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	// Graceful shutdown.
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	go func() {
		logger.Info("starting schwung-manager", "addr", addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			logger.Error("server error", "err", err)
			os.Exit(1)
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
