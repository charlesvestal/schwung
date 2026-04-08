# Asset Validation for Module Detail Page

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add per-file asset validation to the schwung-manager module detail page so users can see which ROM files are present, missing, or invalid (wrong size/CRC), and upload/replace/delete individual files.

**Architecture:** Extend the `ModuleAssets` Go struct with `Files` and `Folders` arrays. Add a validation function that checks each expected file on disk (exists, size, CRC32). Render an asset table on the module detail page with per-file status indicators and upload/delete actions. Start with Mini JV as the first module.

**Tech Stack:** Go (schwung-manager/main.go), HTML templates, CSS, CRC32 (`hash/crc32` stdlib)

---

### Task 1: Extend Go structs for per-file asset definitions

**Files:**
- Modify: `schwung-manager/main.go:75-82` (ModuleAssets struct)

**Step 1: Add AssetFile, AssetFolder structs and extend ModuleAssets**

Add these structs after the existing `ModuleAssets` struct, and add `Files` and `Folders` fields to `ModuleAssets`:

```go
type ModuleAssets struct {
	Path        string        `json:"path"`
	Label       string        `json:"label"`
	Extensions  []string      `json:"extensions"`
	Description string        `json:"description"`
	Hint        string        `json:"hint"`
	HintURL     string        `json:"hint_url,omitempty"`
	HintURLLabel string       `json:"hint_url_label,omitempty"`
	Optional    bool          `json:"optional"`
	AllowFolders bool         `json:"allowFolders,omitempty"`
	Files       []AssetFile   `json:"files,omitempty"`
	Folders     []AssetFolder `json:"folders,omitempty"`
}

type AssetFile struct {
	Filename string `json:"filename"`
	Label    string `json:"label"`
	Size     int64  `json:"size"`
	Required bool   `json:"required"`
	CRC32    string `json:"crc32,omitempty"`
}

type AssetFolder struct {
	Path        string   `json:"path"`
	Label       string   `json:"label"`
	Description string   `json:"description"`
	Extensions  []string `json:"extensions"`
	Required    bool     `json:"required"`
}
```

**Step 2: Add AssetFileStatus and AssetFolderStatus types**

These are computed at request time, not stored in JSON:

```go
type AssetFileStatus struct {
	AssetFile
	Present   bool
	SizeMatch bool
	CRCMatch  *bool  // nil = no CRC specified in spec
	ActualSize int64
	ActualCRC  string
}

type AssetFolderStatus struct {
	AssetFolder
	Exists    bool
	FileCount int
}
```

**Step 3: Commit**

```bash
git add schwung-manager/main.go
git commit -m "feat: add per-file asset definition structs for module validation"
```

---

### Task 2: Add asset validation function

**Files:**
- Modify: `schwung-manager/main.go` (new function, add `hash/crc32` import)

**Step 1: Add the validateAssets function**

Add after the struct definitions. This takes the assets directory path and the `ModuleAssets` spec, and returns slices of status objects:

```go
import (
	"hash/crc32"
	// ... existing imports
)

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
```

**Step 2: Commit**

```bash
git add schwung-manager/main.go
git commit -m "feat: add asset validation with size and CRC32 checking"
```

---

### Task 3: Wire validation into the module detail handler

**Files:**
- Modify: `schwung-manager/main.go:638-699` (handleModuleDetail)

**Step 1: Call validateAssets and pass results to template**

After the existing `moduleAssets` assignment (around line 684), add:

```go
var fileStatuses []AssetFileStatus
var folderStatuses []AssetFolderStatus
if moduleAssets != nil && len(moduleAssets.Files) > 0 && assetsDir != "" {
	fileStatuses, folderStatuses = validateAssets(assetsDir, moduleAssets)
}
```

Add to the `data` map:

```go
"FileStatuses":   fileStatuses,
"FolderStatuses": folderStatuses,
```

**Step 2: Commit**

```bash
git add schwung-manager/main.go
git commit -m "feat: wire asset validation into module detail handler"
```

---

### Task 4: Add asset upload-to-slot and delete endpoints

**Files:**
- Modify: `schwung-manager/main.go` (new handlers + route registration around line 2232)

**Step 1: Add handleModuleAssetUploadSlot handler**

This accepts a specific target filename, saving the uploaded file with that exact name (not the uploaded file's original name):

```go
func (app *App) handleModuleAssetUploadSlot(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	targetFilename := r.PathValue("filename")
	modDir := app.findModuleDir(id)
	if modDir == "" {
		http.NotFound(w, r)
		return
	}

	// Read module.json to find assets dir
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

	// Ensure assets directory exists
	os.MkdirAll(assetsDir, 0755)

	file, _, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "missing file", http.StatusBadRequest)
		return
	}
	defer file.Close()

	target := filepath.Join(assetsDir, targetFilename)
	// Validate target is within assets dir
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
```

**Step 2: Add handleModuleAssetDelete handler**

```go
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
```

**Step 3: Register routes**

Add near line 2233:

```go
mux.HandleFunc("POST /modules/{id}/assets/upload/{filename}", app.handleModuleAssetUploadSlot)
mux.HandleFunc("POST /modules/{id}/assets/delete/{filename}", app.handleModuleAssetDelete)
```

**Step 4: Commit**

```bash
git add schwung-manager/main.go
git commit -m "feat: add per-file asset upload and delete endpoints"
```

---

### Task 5: Update module detail template with asset table

**Files:**
- Modify: `schwung-manager/templates/module_detail.html:37-82`

**Step 1: Replace the assets section in the template**

Replace the block from line 37 (`{{if .ModuleAssets}}`) through line 82 (end of assets `dd`) with the new asset table. When `FileStatuses` is populated, show the per-file table; otherwise fall back to the existing generic display.

```html
{{if .FileStatuses}}
<dt>{{.ModuleAssets.Label}}</dt>
<dd>
    <p class="asset-description">{{.ModuleAssets.Description}}</p>
    {{if .ModuleAssets.Hint}}<p class="text-muted asset-hint">{{.ModuleAssets.Hint}}</p>{{end}}
    <table class="asset-table" role="table">
        <thead>
            <tr>
                <th></th>
                <th>File</th>
                <th>Expected</th>
                <th>Status</th>
                <th>Actions</th>
            </tr>
        </thead>
        <tbody>
            {{range .FileStatuses}}
            <tr class="asset-row">
                <td class="asset-req">{{if .Required}}<span title="Required">●</span>{{else}}<span class="text-muted" title="Optional">○</span>{{end}}</td>
                <td>
                    <strong>{{.Label}}</strong><br>
                    <code class="asset-filename">{{.Filename}}</code>
                </td>
                <td class="text-muted">{{if .Size}}{{humanSize .Size}}{{else}}—{{end}}</td>
                <td>
                    {{if not .Present}}
                        {{if .Required}}<span class="asset-status asset-missing" title="Missing — required">✗ Missing</span>
                        {{else}}<span class="asset-status asset-optional" title="Not uploaded">— Not uploaded</span>{{end}}
                    {{else if not .SizeMatch}}
                        <span class="asset-status asset-warning" title="Wrong size: got {{humanSize .ActualSize}}">⚠ Wrong size</span>
                    {{else if and .CRCMatch (not (derefBool .CRCMatch))}}
                        <span class="asset-status asset-warning" title="CRC mismatch: got {{.ActualCRC}}">⚠ CRC mismatch</span>
                    {{else}}
                        <span class="asset-status asset-valid" title="Valid">✓ Valid</span>
                    {{end}}
                </td>
                <td class="asset-actions">
                    {{if .Present}}
                    <form method="POST" action="/modules/{{$.Module.ID}}/assets/upload/{{.Filename}}" enctype="multipart/form-data" class="inline-form asset-upload-form">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}">
                        <label class="btn btn-secondary btn-small asset-upload-btn">
                            Replace <input type="file" name="file" hidden>
                        </label>
                    </form>
                    <form method="POST" action="/modules/{{$.Module.ID}}/assets/delete/{{.Filename}}" class="inline-form" data-confirm="Delete {{.Filename}}?" data-confirm-title="Delete Asset" data-confirm-label="Delete" data-confirm-danger="true">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}">
                        <button type="submit" class="btn btn-danger btn-small">Delete</button>
                    </form>
                    {{else}}
                    <form method="POST" action="/modules/{{$.Module.ID}}/assets/upload/{{.Filename}}" enctype="multipart/form-data" class="inline-form asset-upload-form">
                        <input type="hidden" name="csrf_token" value="{{$.CSRFToken}}">
                        <label class="btn btn-primary btn-small asset-upload-btn">
                            Upload <input type="file" name="file" hidden>
                        </label>
                    </form>
                    {{end}}
                </td>
            </tr>
            {{end}}
        </tbody>
    </table>

    {{range .FolderStatuses}}
    <div class="asset-folder-row">
        <strong>{{.Label}}</strong>{{if .Description}} — <span class="text-muted">{{.Description}}</span>{{end}}
        {{if not .Required}}<span class="text-muted">(optional)</span>{{end}}
        <br>
        {{if .Exists}}
            <span class="text-muted">{{.FileCount}} file{{if ne .FileCount 1}}s{{end}} installed</span>
            <a href="/files?path={{$.AssetsDir}}/{{.Path}}" class="btn btn-secondary btn-small">Manage</a>
        {{else}}
            <span class="text-muted">Not created</span>
        {{end}}
    </div>
    {{end}}
</dd>

{{else if .ModuleAssets}}
<dt>{{.ModuleAssets.Label}}</dt>
<dd class="requires-note">
    {{.ModuleAssets.Description}}
    {{if .ModuleAssets.Hint}}<br><small class="text-muted">{{.ModuleAssets.Hint}}</small>{{end}}
</dd>
{{else if .Module.Requires}}
<dt>Requires</dt>
<dd class="requires-note">{{.Module.Requires}}</dd>
{{end}}

{{if isInstalled .Module.ID .Installed}}
{{if .AssetsDir}}
{{if not .FileStatuses}}
<dt>Assets</dt>
<dd>
    <a href="/files?path={{.AssetsDir}}" class="btn btn-secondary btn-small">Manage {{if .ModuleAssets}}{{.ModuleAssets.Label}}{{else}}Assets{{end}}</a>
</dd>
{{end}}
{{end}}
{{end}}
```

**Step 2: Add template helper functions**

In `funcMap` (around line 418 in main.go), add:

```go
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
        return true // treat nil as "no check needed" = truthy
    }
    return *b
},
```

**Step 3: Add auto-submit JS for file upload inputs**

At the bottom of the template's `<script>` block, add:

```javascript
document.querySelectorAll('.asset-upload-form input[type="file"]').forEach(function(input) {
    input.addEventListener('change', function() {
        if (this.files.length > 0) {
            var overlay = document.getElementById('action-overlay');
            var text = document.getElementById('action-overlay-text');
            text.textContent = 'Uploading...';
            overlay.hidden = false;
            this.closest('form').submit();
        }
    });
});
```

**Step 4: Commit**

```bash
git add schwung-manager/templates/module_detail.html schwung-manager/main.go
git commit -m "feat: render per-file asset table with status indicators on module detail page"
```

---

### Task 6: Add CSS for asset table

**Files:**
- Modify: `schwung-manager/static/style.css`

**Step 1: Add asset table styles**

Add after the `.requires-note` block (around line 1008):

```css
/* Asset validation table */
.asset-description {
    margin: 0 0 var(--space-xs) 0;
}
.asset-hint {
    font-size: 0.85rem;
    margin: 0 0 var(--space-md) 0;
}
.asset-table {
    width: 100%;
    border-collapse: collapse;
    font-size: 0.9rem;
    margin-top: var(--space-sm);
}
.asset-table th {
    text-align: left;
    font-weight: 600;
    color: var(--text-muted);
    font-size: 0.8rem;
    text-transform: uppercase;
    letter-spacing: 0.03em;
    padding: var(--space-xs) var(--space-sm);
    border-bottom: 1px solid var(--border);
}
.asset-table td {
    padding: var(--space-sm);
    border-bottom: 1px solid var(--border);
    vertical-align: middle;
}
.asset-table tbody tr:last-child td {
    border-bottom: none;
}
.asset-req {
    width: 1.5rem;
    text-align: center;
}
.asset-filename {
    font-size: 0.8rem;
}
.asset-status {
    font-weight: 600;
    white-space: nowrap;
}
.asset-missing {
    color: var(--color-danger);
}
.asset-warning {
    color: var(--color-warning);
}
.asset-valid {
    color: var(--color-success);
}
.asset-optional {
    color: var(--text-muted);
}
.asset-actions {
    white-space: nowrap;
    display: flex;
    gap: var(--space-xs);
    align-items: center;
}
.asset-upload-btn {
    cursor: pointer;
}
.asset-upload-btn input[type="file"] {
    display: none;
}
.asset-folder-row {
    margin-top: var(--space-md);
    padding: var(--space-sm) 0;
    border-top: 1px solid var(--border);
}
.asset-folder-row .btn {
    margin-left: var(--space-sm);
}
```

**Step 2: Commit**

```bash
git add schwung-manager/static/style.css
git commit -m "feat: add asset table CSS styles"
```

---

### Task 7: Update Mini JV module.json with per-file asset definitions

**Files:**
- Modify: `/Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-jv880/src/module.json`

**Step 1: Add files and folders arrays to the assets object**

Update the existing `assets` block to include:

```json
"assets": {
    "path": "roms",
    "label": "ROMs",
    "extensions": [".bin", ".rom"],
    "allowFolders": true,
    "description": "Requires JV-880 v1.0.0 ROM files",
    "hint": "The plugin requires the v1.0.0 ROMs. Version 1.0.1 will NOT work.",
    "hint_url": "https://github.com/charlesvestal/schwung-jv880",
    "hint_url_label": "Read more",
    "files": [
        {
            "filename": "jv880_rom1.bin",
            "label": "CPU ROM",
            "size": 32768,
            "required": true
        },
        {
            "filename": "jv880_rom2.bin",
            "label": "Patch ROM",
            "size": 262144,
            "required": true
        },
        {
            "filename": "jv880_waverom1.bin",
            "label": "Waveform ROM 1",
            "size": 2097152,
            "required": true
        },
        {
            "filename": "jv880_waverom2.bin",
            "label": "Waveform ROM 2",
            "size": 2097152,
            "required": true
        },
        {
            "filename": "jv880_nvram.bin",
            "label": "NVRAM (user patches)",
            "size": 32768,
            "required": false
        }
    ],
    "folders": [
        {
            "path": "expansions",
            "label": "Expansion ROMs",
            "description": "SR-JV80 expansion cards (8MB or 2MB .bin files)",
            "extensions": [".bin", ".BIN"],
            "required": false
        }
    ]
}
```

Note: CRC32 values omitted for now since the JV-880 ROMs don't have universally known CRCs (different dumps may vary). Size checking is sufficient for validation.

**Step 2: Commit (in the schwung-jv880 repo)**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung-jv880
git add src/module.json
git commit -m "feat: add per-file asset definitions for ROM validation"
```

---

### Task 8: Build, deploy, and verify

**Step 1: Build the manager**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh
```

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Verify in browser**

Open the Mini JV module detail page in schwung-manager. Confirm:
- Asset table renders with all 5 ROM files
- Status shows correctly for present/missing files
- Upload works: pick a file, auto-submits, page refreshes with updated status
- Delete works: confirm dialog, file removed, status updates
- Expansion ROMs folder shows file count and "Manage" link
- Modules without `files` array still show the old generic display
