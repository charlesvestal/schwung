# Web Download & Wave Edit Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a web-based download tool to schwung-manager that downloads audio from YouTube/SoundCloud/Archive.org URLs as WAV files and opens them in Wave Edit on the device.

**Architecture:** schwung-manager gets new routes for a download page and API. Downloads shell out to yt-dlp + ffmpeg (from webstream's installed binaries). Opening files in Wave Edit uses the existing sampler_cmd pattern: write a JSON file with tool/path info, set a command byte in shadow_control shared memory, shadow_ui.js polls and dispatches.

**Tech Stack:** Go (schwung-manager), C (shadow_ui.c, shadow_constants.h), JavaScript (shadow_ui.js, cratedig.html)

---

### Task 1: Add `open_tool_cmd` field to shadow_control_t

**Files:**
- Modify: `src/host/shadow_constants.h:145` (reserved array)

**Step 1: Replace reserved[0] with open_tool_cmd**

In `src/host/shadow_constants.h`, change the end of `shadow_control_t` from:

```c
    volatile uint8_t suspend_overtake;  /* 1=suspend (skip exit hook), 0=normal exit */
    volatile uint8_t reserved[8];
```

to:

```c
    volatile uint8_t suspend_overtake;  /* 1=suspend (skip exit hook), 0=normal exit */
    volatile uint8_t open_tool_cmd;     /* 0=none, 1=open tool (path in /data/UserData/schwung/open_tool_cmd.json) */
    volatile uint8_t reserved[7];
```

This keeps the struct at exactly 64 bytes (CONTROL_BUFFER_SIZE). The `open_tool_cmd` field is at byte offset 56.

**Step 2: Verify struct size unchanged**

The compile-time check on line 337 ensures correctness:
```c
typedef char shadow_control_size_check[(sizeof(shadow_control_t) == CONTROL_BUFFER_SIZE) ? 1 : -1];
```

Run: `./scripts/build.sh` — should compile without errors.

**Step 3: Commit**
```
feat: add open_tool_cmd field to shadow_control_t
```

---

### Task 2: Add C binding in shadow_ui.c to read/clear open_tool_cmd

**Files:**
- Modify: `src/shadow/shadow_ui.c` (add new JS-exposed function, register it)

**Step 1: Add the C function**

Near the existing `js_shadow_get_ui_flags` function (around line 290), add:

```c
/* shadow_get_open_tool_cmd() -> int (0=none, 1=open_tool; auto-clears) */
static JSValue js_shadow_get_open_tool_cmd(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_NewInt32(ctx, 0);
    uint8_t cmd = shadow_control->open_tool_cmd;
    if (cmd) shadow_control->open_tool_cmd = 0;  /* auto-clear */
    return JS_NewInt32(ctx, cmd);
}
```

**Step 2: Register it in JS global scope**

In the function registration section (around line 2228-2233, near `shadow_get_ui_flags`), add:

```c
JS_SetPropertyStr(ctx, global_obj, "shadow_get_open_tool_cmd",
    JS_NewCFunction(ctx, js_shadow_get_open_tool_cmd, "shadow_get_open_tool_cmd", 0));
```

**Step 3: Commit**
```
feat: expose shadow_get_open_tool_cmd to JS
```

---

### Task 3: Poll open_tool_cmd in shadow_ui.js tick()

**Files:**
- Modify: `src/shadow/shadow_ui.js` (in tick function, around line 13220)

**Step 1: Add polling after the existing ui_flags check**

After the `shadow_get_ui_flags()` block in tick() (around line 13257), add:

```javascript
    /* Check for open-in-tool command from web UI */
    if (typeof shadow_get_open_tool_cmd === "function") {
        const toolCmd = shadow_get_open_tool_cmd();
        if (toolCmd === 1) {
            const cmdJson = host_read_file("/data/UserData/schwung/open_tool_cmd.json");
            if (cmdJson) {
                try {
                    const cmd = JSON.parse(cmdJson);
                    if (cmd.file_path && cmd.tool_id) {
                        debugLog("open_tool_cmd: opening " + cmd.file_path + " in " + cmd.tool_id);
                        host_open_file_in_tool(cmd.file_path, cmd.tool_id);
                    }
                } catch (e) {
                    debugLog("open_tool_cmd: JSON parse error: " + e);
                }
            }
        }
    }
```

This uses the existing `host_read_file()` and `host_open_file_in_tool()` functions. The command byte auto-clears in the C binding, so even if the JSON file lingers on disk it won't re-trigger.

**Step 2: Commit**
```
feat: poll open_tool_cmd in shadow_ui.js tick
```

---

### Task 4: Add open_tool_cmd offset and accessor in shmconfig.go

**Files:**
- Modify: `schwung-manager/shmconfig.go` (add offset constant and accessor method)

**Step 1: Add offset constant**

In the offset constants section (around line 52, after `offSkipbackReqVol`), add:

```go
offOpenToolCmd = 56 // uint8 — 0=none, 1=open tool
```

**Step 2: Add accessor method**

After the existing accessor methods (around line 147), add:

```go
func (s *ShmConfig) SetOpenToolCmd(v uint8) { s.setU8(offOpenToolCmd, v) }
```

**Step 3: Commit**
```
feat: expose open_tool_cmd in shmconfig.go
```

---

### Task 5: Add download page template

**Files:**
- Create: `schwung-manager/templates/download.html`

**Step 1: Create the template**

Create `schwung-manager/templates/download.html`. The template extends `base.html` (same pattern as files.html, modules.html, etc.). It should contain:

- A URL input field (pre-filled from `{{.URL}}` if present)
- A title/filename input field (pre-filled from `{{.Title}}` if present)
- A Download button
- A status area that polls `/api/download/status/{id}`
- Auto-start logic: if `{{.URL}}` is set, auto-submit on page load
- After successful download, auto-trigger open-in-tool via `POST /api/open-in-tool` (only when auto-started from URL param)

The page uses `fetch()` with CSRF token from cookie (matching existing pattern in the codebase — read `csrf_token` cookie, send as `X-CSRF-Token` header).

```html
{{define "download.html"}}
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Download — Schwung</title>
    <link rel="stylesheet" href="/static/style.css">
    <style>
        .download-form { max-width: 600px; margin: 2rem auto; padding: 0 1rem; }
        .download-form input[type="text"] { width: 100%; padding: 0.5rem; margin: 0.25rem 0 1rem 0; box-sizing: border-box; font-size: 1rem; }
        .download-form label { font-weight: bold; display: block; }
        .download-form button { padding: 0.5rem 1.5rem; font-size: 1rem; cursor: pointer; }
        .download-form button:disabled { opacity: 0.5; cursor: not-allowed; }
        #status { margin-top: 1rem; padding: 1rem; border-radius: 4px; display: none; }
        #status.downloading { background: #e8f0fe; display: block; }
        #status.done { background: #e6f4ea; display: block; }
        #status.error { background: #fce8e6; display: block; }
    </style>
</head>
<body>
    {{template "nav.html" .}}
    <div class="download-form">
        <h1>Download Audio</h1>
        <label for="url">URL</label>
        <input type="text" id="url" placeholder="YouTube, SoundCloud, or Archive.org URL" value="{{.URL}}">
        <label for="title">Filename (optional)</label>
        <input type="text" id="title" placeholder="Auto-detected from URL" value="{{.Title}}">
        <button id="dlBtn" onclick="startDownload()">Download</button>
        <div id="status"></div>
    </div>
    <script>
    const autoStart = {{.AutoStart}};
    let jobId = null;
    let pollTimer = null;

    function csrfToken() {
        const m = document.cookie.match(/(?:^|; )csrf_token=([^;]*)/);
        return m ? m[1] : '';
    }

    async function startDownload() {
        const url = document.getElementById('url').value.trim();
        const title = document.getElementById('title').value.trim();
        if (!url) return;
        document.getElementById('dlBtn').disabled = true;
        setStatus('downloading', 'Starting download...');
        try {
            const resp = await fetch('/api/download', {
                method: 'POST',
                headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken()},
                body: JSON.stringify({url, title})
            });
            const data = await resp.json();
            if (!resp.ok) { setStatus('error', data.error || 'Failed'); return; }
            jobId = data.job_id;
            pollStatus();
        } catch (e) {
            setStatus('error', 'Request failed: ' + e.message);
        }
    }

    async function pollStatus() {
        if (!jobId) return;
        try {
            const resp = await fetch('/api/download/status/' + jobId);
            const data = await resp.json();
            if (data.state === 'downloading') {
                setStatus('downloading', 'Downloading...');
                pollTimer = setTimeout(pollStatus, 1000);
            } else if (data.state === 'done') {
                setStatus('done', 'Downloaded: ' + data.path.split('/').pop());
                document.getElementById('dlBtn').disabled = false;
                if (autoStart) { openInTool(data.path); }
            } else if (data.state === 'error') {
                setStatus('error', 'Error: ' + (data.error || 'Unknown'));
                document.getElementById('dlBtn').disabled = false;
            }
        } catch (e) {
            setStatus('error', 'Poll failed: ' + e.message);
        }
    }

    async function openInTool(filePath) {
        try {
            await fetch('/api/open-in-tool', {
                method: 'POST',
                headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken()},
                body: JSON.stringify({file_path: filePath, tool_id: 'waveform-editor'})
            });
            setStatus('done', 'Downloaded and sent to Wave Edit: ' + filePath.split('/').pop());
        } catch (e) { /* ignore — download succeeded even if open fails */ }
    }

    function setStatus(cls, msg) {
        const el = document.getElementById('status');
        el.className = cls;
        el.textContent = msg;
    }

    if (autoStart && document.getElementById('url').value.trim()) {
        startDownload();
    }
    </script>
</body>
</html>
{{end}}
```

**Step 2: Register template in loadTemplates**

In `schwung-manager/main.go`, in the `pages` slice inside `loadTemplates()` (around line 613-622), add:

```go
"templates/download.html",
```

**Step 3: Commit**
```
feat: add download page template
```

---

### Task 6: Add download API and handlers in schwung-manager

**Files:**
- Modify: `schwung-manager/main.go` (add handlers and route registration)

**Step 1: Add download job tracking**

Add these types and fields near the App struct (around line 647):

```go
type downloadJob struct {
    ID    string `json:"job_id"`
    State string `json:"state"` // "downloading", "done", "error"
    Path  string `json:"path,omitempty"`
    Error string `json:"error,omitempty"`
}

// Add to App struct:
// downloadJobs map[string]*downloadJob
// downloadMu   sync.Mutex
```

Add the new fields to the App struct, and initialize `downloadJobs: make(map[string]*downloadJob)` in the App initialization (around line 2580).

**Step 2: Add the download page handler**

```go
func (app *App) handleDownloadPage(w http.ResponseWriter, r *http.Request) {
    urlParam := r.URL.Query().Get("url")
    titleParam := r.URL.Query().Get("title")
    data := map[string]any{
        "Title":     "Download",
        "Active":    "download",
        "URL":       urlParam,
        "Title":     titleParam,
        "AutoStart": urlParam != "",
    }
    app.render(w, r, "download.html", data)
}
```

Note: the `"Title"` key is used twice — the template data key for the filename should be `"FileTitle"` to avoid collision. Update the template to use `{{.FileTitle}}` for the filename input value, and keep `"Title"` for the page title in the nav.

**Step 3: Add the download API handler**

```go
func (app *App) handleAPIDownload(w http.ResponseWriter, r *http.Request) {
    var req struct {
        URL   string `json:"url"`
        Title string `json:"title"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.URL == "" {
        http.Error(w, `{"error":"url required"}`, http.StatusBadRequest)
        return
    }

    jobID := fmt.Sprintf("%d", time.Now().UnixNano())
    job := &downloadJob{ID: jobID, State: "downloading"}
    app.downloadMu.Lock()
    app.downloadJobs[jobID] = job
    app.downloadMu.Unlock()

    go app.runDownload(job, req.URL, req.Title)

    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(map[string]string{"job_id": jobID})
}
```

**Step 4: Add the download execution function**

The binary paths and output directory:
```go
const (
    webstreamBinDir = "/data/UserData/schwung/modules/sound_generators/webstream/bin"
    downloadOutDir  = "/data/UserData/UserLibrary/Samples/Schwung/Webstream"
)
```

```go
func (app *App) runDownload(job *downloadJob, url, title string) {
    // Ensure output dir exists
    os.MkdirAll(downloadOutDir, 0755)

    // Sanitize filename
    if title == "" {
        title = "download"
    }
    safe := sanitizeFilename(title)
    outPath := filepath.Join(downloadOutDir, safe+".wav")

    // Dedup
    if _, err := os.Stat(outPath); err == nil {
        for n := 2; n < 100; n++ {
            candidate := filepath.Join(downloadOutDir, fmt.Sprintf("%s (%d).wav", safe, n))
            if _, err := os.Stat(candidate); os.IsNotExist(err) {
                outPath = candidate
                break
            }
        }
    }

    // Build command: yt-dlp | ffmpeg
    ytdlp := filepath.Join(webstreamBinDir, "yt-dlp")
    ffmpeg := filepath.Join(webstreamBinDir, "ffmpeg")
    cmdStr := fmt.Sprintf(
        `"%s" --no-playlist -f "bestaudio[ext=m4a]/bestaudio" -o - '%s' 2>/dev/null | "%s" -hide_banner -loglevel warning -i pipe:0 -vn -sn -dn -af "aresample=44100" -ac 2 -ar 44100 '%s' -y 2>/dev/null`,
        ytdlp, url, ffmpeg, outPath,
    )

    cmd := exec.Command("sh", "-c", cmdStr)
    output, err := cmd.CombinedOutput()
    if err != nil {
        app.logger.Error("download failed", "url", url, "err", err, "output", string(output))
        job.State = "error"
        job.Error = "Download failed"
        return
    }

    // Verify file exists and is non-trivial (>44 bytes = WAV header)
    info, err := os.Stat(outPath)
    if err != nil || info.Size() <= 44 {
        job.State = "error"
        job.Error = "Output file missing or empty"
        os.Remove(outPath)
        return
    }

    job.State = "done"
    job.Path = outPath
}

func sanitizeFilename(in string) string {
    var out strings.Builder
    for _, c := range in {
        switch c {
        case '/', '\\', ':', '*', '?', '"', '<', '>', '|':
            out.WriteRune('_')
        default:
            out.WriteRune(c)
        }
    }
    s := strings.TrimRight(out.String(), " .")
    if s == "" {
        s = "download"
    }
    return s
}
```

**Step 5: Add the status polling handler**

```go
func (app *App) handleAPIDownloadStatus(w http.ResponseWriter, r *http.Request) {
    id := r.PathValue("id")
    app.downloadMu.Lock()
    job, ok := app.downloadJobs[id]
    app.downloadMu.Unlock()

    w.Header().Set("Content-Type", "application/json")
    if !ok {
        json.NewEncoder(w).Encode(map[string]string{"state": "error", "error": "unknown job"})
        return
    }
    json.NewEncoder(w).Encode(job)
}
```

**Step 6: Add the open-in-tool handler**

```go
func (app *App) handleAPIOpenInTool(w http.ResponseWriter, r *http.Request) {
    var req struct {
        FilePath string `json:"file_path"`
        ToolID   string `json:"tool_id"`
    }
    if err := json.NewDecoder(r.Body).Decode(&req); err != nil || req.FilePath == "" || req.ToolID == "" {
        http.Error(w, `{"error":"file_path and tool_id required"}`, http.StatusBadRequest)
        return
    }

    // Write the command JSON file
    cmdJSON, _ := json.Marshal(map[string]string{
        "file_path": req.FilePath,
        "tool_id":   req.ToolID,
    })
    cmdPath := filepath.Join(app.basePath, "open_tool_cmd.json")
    if err := os.WriteFile(cmdPath, cmdJSON, 0644); err != nil {
        http.Error(w, `{"error":"failed to write command"}`, http.StatusInternalServerError)
        return
    }

    // Set the command byte in shared memory
    if app.shm != nil {
        app.shm.SetOpenToolCmd(1)
    }

    w.Header().Set("Content-Type", "application/json")
    w.Write([]byte(`{"ok":true}`))
}
```

**Step 7: Register all routes**

In the route registration section (around line 2595-2661), add:

```go
mux.HandleFunc("GET /download", app.handleDownloadPage)
mux.HandleFunc("POST /api/download", app.handleAPIDownload)
mux.HandleFunc("GET /api/download/status/{id}", app.handleAPIDownloadStatus)
mux.HandleFunc("POST /api/open-in-tool", app.handleAPIOpenInTool)
```

**Step 8: Build and test**

Run: `cd schwung-manager && go build -o schwung-manager .`
Expected: Compiles without errors.

**Step 9: Commit**
```
feat: add download API and open-in-tool endpoint to schwung-manager
```

---

### Task 7: Add "Send to Move" button in Crate Dig

**Files:**
- Modify: `../schwung-webstream/docs/cratedig.html` (in playResult function, around line 1334)

**Step 1: Add the button**

In the `playResult` function, after the WhoSampled button creation (around line 1334) and before the Favorite button (around line 1336), add:

```javascript
const sendBtn = document.createElement('a');
const sendUrl = 'http://schwung.local/download?url=' + encodeURIComponent(r.url || 'https://www.youtube.com/watch?v=' + r.videoId) + '&title=' + encodeURIComponent(r.title || '');
sendBtn.href = sendUrl;
sendBtn.target = '_blank';
sendBtn.className = 'tag tag-btn';
sendBtn.innerHTML = '<span class="tag-label">Move</span>Send';
meta.appendChild(sendBtn);
```

This creates a link that opens the schwung-manager download page with the URL and title pre-filled. Since `autoStart` will be true (URL param is present), the download starts immediately.

**Step 2: Commit**
```
feat: add Send to Move button in Crate Dig
```

---

### Task 8: Build host, deploy, and test end-to-end

**Step 1: Build the host**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung
./scripts/build.sh
```

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Manual test checklist**

1. Navigate to `http://schwung.local/download` — page loads with empty form
2. Paste a YouTube URL, click Download — status shows progress, then "Downloaded"
3. Navigate to `http://schwung.local/download?url=https://www.youtube.com/watch?v=dQw4w9WgXcQ&title=Test` — auto-starts download, auto-opens in Wave Edit on device
4. Check file exists at `/data/UserData/UserLibrary/Samples/Schwung/Webstream/Test.wav`
5. Download same URL again — creates `Test (2).wav` (dedup works)
6. Open Crate Dig, find a track, click "Send to Move" — opens download page, auto-downloads and opens in Wave Edit

**Step 4: Commit any fixes, then final commit**
```
docs: update plans with implementation status
```
