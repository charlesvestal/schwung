# Schwung Manager Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix 5 open issues in the Schwung Manager web UI and its device-side config sync.

**Architecture:** The web UI (Go server at `:7700`) writes settings to `shadow_config.json` and `features.json`. The device-side shadow UI (QuickJS) reads these files in `syncSettingsFromConfigFile()` during `tick()`. The crash came from C functions (`display_mirror_set`, `set_pages_set`) doing blocking file I/O (fopen) during tick(). Fix: add memory-only C functions for tick()-safe sync, re-enable the periodic sync. Other fixes are CSS/JS/Go template changes.

**Tech Stack:** C (shadow_ui.c), JavaScript (shadow_ui.js), Go (main.go), HTML templates, CSS

---

### Task 1: Fix Web→Device config sync (was SIGABRT)

**Root cause:** `display_mirror_set()` and `set_pages_set()` in `shadow_ui.c` do `fopen()` to persist to `features.json`. Calling these from JS `tick()` causes SIGABRT. Other C functions (`tts_set_enabled`, `overlay_knobs_set_mode`, etc.) only write shared memory and are safe.

**Fix:** Add two new memory-only C functions (`display_mirror_set_shm`, `set_pages_set_shm`) that write to shared memory without file I/O. Use these in `syncSettingsFromConfigFile()`. The web server already persists to `features.json`, so file I/O from the JS side is redundant during sync. Re-enable the periodic sync in tick().

**Files:**
- Modify: `src/shadow/shadow_ui.c` (add 2 new C functions, register them)
- Modify: `src/shadow/shadow_ui.js` (update syncSettingsFromConfigFile, re-enable in tick)

**Step 1: Add memory-only C functions in shadow_ui.c**

After `js_display_mirror_set` (line ~1677), add:

```c
/* display_mirror_set_shm(enabled) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web→device config sync. */
static JSValue js_display_mirror_set_shm(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->display_mirror = enabled ? 1 : 0;
    return JS_UNDEFINED;
}
```

After `js_set_pages_set` (line ~1739), add:

```c
/* set_pages_set_shm(enabled) - Write to shared memory ONLY (no file I/O).
 * Safe to call from tick() for web→device config sync. */
static JSValue js_set_pages_set_shm(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1 || !shadow_control) return JS_UNDEFINED;
    int enabled = 0;
    JS_ToInt32(ctx, &enabled, argv[0]);
    shadow_control->set_pages_enabled = enabled ? 1 : 0;
    return JS_UNDEFINED;
}
```

Register both in the function registration block (near line ~2273):

```c
JS_SetPropertyStr(ctx, global_obj, "display_mirror_set_shm", JS_NewCFunction(ctx, js_display_mirror_set_shm, "display_mirror_set_shm", 1));
JS_SetPropertyStr(ctx, global_obj, "set_pages_set_shm", JS_NewCFunction(ctx, js_set_pages_set_shm, "set_pages_set_shm", 1));
```

**Step 2: Update syncSettingsFromConfigFile() in shadow_ui.js**

Replace the `display_mirror_set` call with `display_mirror_set_shm` and the `set_pages_set` call with `set_pages_set_shm`. These skip file I/O.

Change line ~5431-5433:
```javascript
/* Display mirror */
if (c.display_mirror !== undefined && typeof display_mirror_set_shm === "function") {
    const cur = typeof display_mirror_get === "function" ? !!display_mirror_get() : false;
    if (!!c.display_mirror !== cur) display_mirror_set_shm(c.display_mirror ? 1 : 0);
}
```

Change line ~5502-5504:
```javascript
/* Set pages */
if (c.set_pages_enabled !== undefined && typeof set_pages_set_shm === "function") {
    const cur = typeof set_pages_get === "function" ? !!set_pages_get() : true;
    if (!!c.set_pages_enabled !== cur) set_pages_set_shm(c.set_pages_enabled ? 1 : 0);
}
```

Also fix the `filebrowser_enabled` sync — it calls `host_write_file` / `host_remove_dir` which may also be unsafe from tick. Guard it:
```javascript
/* Filebrowser — only update JS variable, skip flag file I/O from tick */
if (c.filebrowser_enabled !== undefined && c.filebrowser_enabled !== filebrowserEnabled) {
    filebrowserEnabled = c.filebrowser_enabled;
}
```

**Step 3: Re-enable periodic sync in tick()**

Uncomment the sync block (~line 13053-13060) and remove the view guard so it syncs regardless of which view is active:

```javascript
/* Periodic config sync from web UI */
if (++_configSyncTickCounter >= CONFIG_SYNC_INTERVAL) {
    _configSyncTickCounter = 0;
    syncSettingsFromConfigFile();
}
```

**Step 4: Build and test**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung && ./scripts/build.sh
```

Deploy and verify: change a setting in the web UI, confirm it takes effect on the device within ~2 seconds without SIGABRT.

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.c src/shadow/shadow_ui.js
git commit -m "fix: re-enable web→device config sync with tick-safe shared memory writes"
```

---

### Task 2: Fix config page number input debouncing

**Problem:** Number inputs (TTS speed/pitch/volume/debounce) fire `onchange` on every keystroke in some browsers, spamming the server. Should wait for the user to finish typing.

**Fix:** Change number inputs to fire on `blur` instead of `change`, and add a debounced input handler for live feedback.

**Files:**
- Modify: `schwung-manager/templates/config.html`

**Step 1: Replace onchange with debounced approach**

In config.html, update the number input template (line ~38-40) to remove `onchange` and use a class instead:

```html
{{else if or (eq .Type "int") (eq .Type "float")}}
<label for="{{.Key}}">{{.Label}}</label>
<div class="number-input-wrap">
    <input type="number" id="{{.Key}}" data-setting-key="{{.Key}}"
           value="{{settingValue .Key $.Values}}" min="{{.Min}}" max="{{.Max}}" step="{{.Step}}">
</div>
{{end}}
```

Then update the `<script>` block to add blur-based saving for number inputs:

```javascript
/* Number inputs: save on blur or Enter key, not on every keystroke */
document.querySelectorAll('input[type="number"][data-setting-key]').forEach(function(el) {
    el.addEventListener('blur', function() {
        setSetting(el.dataset.settingKey, el.value);
    });
    el.addEventListener('keydown', function(e) {
        if (e.key === 'Enter') {
            el.blur();
        }
    });
});
```

**Step 2: Build and test**

```bash
cd /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/schwung-manager
cp -r . /tmp/schwung-manager-build/ && cd /tmp/schwung-manager-build/ && GOOS=linux GOARCH=arm64 go build -a -o schwung-manager . && cp schwung-manager /Volumes/ExtFS/charlesvestal/github/schwung-parent/schwung/schwung-manager/
```

Actually — the Go binary embeds templates, so a full `./scripts/build.sh` from the main repo is needed. Or just test locally with `go run .` on macOS.

**Step 3: Commit**

```bash
git add schwung-manager/templates/config.html
git commit -m "fix: debounce number inputs on config page to fire on blur, not every keystroke"
```

---

### Task 3: Align settings sections between shadow_ui.js and settings-schema.json (issue #5)

**Problem:** Both files already have `auto_update_check` and `filebrowser_enabled` in a "Services" section, so they're actually in sync. The continuation prompt may be outdated. Verify and fix any remaining differences.

**Current state (both files):**
- `settings-schema.json`: Services section has `filebrowser_enabled`, `auto_update_check`
- `shadow_ui.js` `GLOBAL_SETTINGS_SECTIONS`: Services section has `filebrowser_enabled`, `auto_update_check`

These match. The only diff is shadow_ui.js has two extra sections (`updates`, `help`) which are action-only and don't need schema entries.

**Step 1: Verify alignment**

Compare the two schemas side by side. If they match (they appear to), no code changes needed. Add a comment in both files referencing each other to prevent future drift.

**Step 2: If differences found, fix them**

Update the divergent file to match. The canonical source is `settings-schema.json`.

**Step 3: Commit (if changes)**

```bash
git add src/shared/settings-schema.json src/shadow/shadow_ui.js
git commit -m "fix: align settings sections between schema and shadow UI"
```

---

### Task 4: Add loading spinner during module install/uninstall/update (issue #3)

**Problem:** When you click Install/Uninstall/Update on a module detail page, the form submits and the page hangs with no feedback until the redirect completes (can take 30+ seconds for large modules).

**Fix:** Intercept form submission with JS, show an overlay spinner, disable the button. Use existing htmx-indicator pattern.

**Files:**
- Modify: `schwung-manager/templates/module_detail.html`
- Modify: `schwung-manager/static/style.css`

**Step 1: Add loading overlay to module_detail.html**

Add a hidden overlay div and JS to show it on form submit:

```html
<div id="action-overlay" class="action-overlay" hidden>
    <div class="action-overlay-content">
        <div class="spinner"></div>
        <p id="action-overlay-text">Installing...</p>
    </div>
</div>

<script>
document.querySelectorAll('.detail-actions form').forEach(function(form) {
    form.addEventListener('submit', function(e) {
        var btn = form.querySelector('button[type="submit"]');
        var overlay = document.getElementById('action-overlay');
        var text = document.getElementById('action-overlay-text');
        if (btn.classList.contains('btn-danger')) {
            text.textContent = 'Uninstalling...';
        } else if (btn.textContent.trim().startsWith('Update')) {
            text.textContent = 'Updating...';
        } else {
            text.textContent = 'Installing...';
        }
        btn.disabled = true;
        overlay.hidden = false;
    });
});
</script>
```

**Step 2: Add CSS for the overlay and spinner**

In `static/style.css`:

```css
/* ---- Action Overlay ---- */
.action-overlay {
    position: fixed;
    inset: 0;
    background: rgba(0, 0, 0, 0.5);
    display: flex;
    align-items: center;
    justify-content: center;
    z-index: 1000;
}

.action-overlay-content {
    background: var(--surface);
    border-radius: 12px;
    padding: 2rem;
    text-align: center;
    box-shadow: 0 4px 24px rgba(0, 0, 0, 0.3);
}

.spinner {
    width: 32px;
    height: 32px;
    border: 3px solid var(--border);
    border-top-color: var(--primary);
    border-radius: 50%;
    margin: 0 auto 1rem;
    animation: spin 0.8s linear infinite;
}

@keyframes spin {
    to { transform: rotate(360deg); }
}
```

**Step 3: Build and test**

Click Install on a module, verify overlay appears. When redirect happens, overlay naturally goes away.

**Step 4: Commit**

```bash
git add schwung-manager/templates/module_detail.html schwung-manager/static/style.css
git commit -m "feat: add loading spinner overlay during module install/uninstall/update"
```

---

### Task 5: Implement system upgrade handler (issue #4)

**Problem:** `handleSystemUpgrade` is a stub that just redirects with "Upgrade started" but doesn't do anything.

**Fix:** Implement the actual upgrade flow: download the latest `schwung.tar.gz` from the catalog, extract it, and run `install.sh local`. Since this restarts the service (including the web server), show a "rebooting" page and use JS to poll for the server coming back up.

**Files:**
- Modify: `schwung-manager/main.go` (implement handleSystemUpgrade)
- Modify: `schwung-manager/templates/system.html` (add upgrade progress UI)

**Step 1: Implement handleSystemUpgrade in main.go**

Replace the stub with:
1. Fetch catalog to get download URL
2. Download the tarball to `/data/UserData/schwung-upgrade.tar.gz`
3. Extract to a temp dir
4. Run the install script in the background
5. Respond with a "restarting" page

```go
func (app *App) handleSystemUpgrade(w http.ResponseWriter, r *http.Request) {
    cat, err := app.catalogSvc.Fetch()
    if err != nil {
        http.Redirect(w, r, "/system?flash=Failed+to+fetch+catalog:+"+err.Error(), http.StatusSeeOther)
        return
    }

    downloadURL := cat.Host.DownloadURL
    if downloadURL == "" {
        http.Redirect(w, r, "/system?flash=No+download+URL+in+catalog", http.StatusSeeOther)
        return
    }

    // Download in background, then run install.
    go func() {
        client := &http.Client{Timeout: 300 * time.Second}
        resp, err := client.Get(downloadURL)
        if err != nil {
            app.logger.Error("upgrade download failed", "err", err)
            return
        }
        defer resp.Body.Close()

        tarPath := filepath.Join(app.basePath, "schwung-upgrade.tar.gz")
        f, err := os.Create(tarPath)
        if err != nil {
            app.logger.Error("upgrade create file failed", "err", err)
            return
        }
        if _, err := io.Copy(f, resp.Body); err != nil {
            f.Close()
            app.logger.Error("upgrade download copy failed", "err", err)
            return
        }
        f.Close()

        // Extract and run install
        cmd := exec.Command("sh", "-c",
            fmt.Sprintf("cd %s && tar xzf schwung-upgrade.tar.gz && ./scripts/install.sh local --skip-modules --skip-confirmation",
                app.basePath))
        cmd.Dir = app.basePath
        output, err := cmd.CombinedOutput()
        if err != nil {
            app.logger.Error("upgrade install failed", "err", err, "output", string(output))
        }
    }()

    // Show a restarting page.
    w.Header().Set("Content-Type", "text/html")
    w.Write([]byte(`<!DOCTYPE html><html><head><title>Upgrading...</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>body{font-family:system-ui;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;background:#1a1a2e;color:#eee}
.box{text-align:center;padding:2rem}.spinner{width:48px;height:48px;border:4px solid #333;border-top-color:#6c63ff;border-radius:50%;margin:0 auto 1rem;animation:s .8s linear infinite}
@keyframes s{to{transform:rotate(360deg)}}</style></head>
<body><div class="box"><div class="spinner"></div><h2>Upgrading Schwung...</h2><p>The service will restart. This page will reload automatically.</p>
<script>setInterval(function(){fetch('/system',{cache:'no-store'}).then(function(r){if(r.ok)location.href='/system?flash=Upgrade+complete'}).catch(function(){})},3000)</script>
</div></body></html>`))
}
```

**Step 2: Add version check before upgrade**

Before downloading, compare `cat.Host.LatestVersion` with the installed version. If already up to date, redirect with a flash message.

**Step 3: Build and test**

Full `./scripts/build.sh`, deploy, test the upgrade button.

**Step 4: Commit**

```bash
git add schwung-manager/main.go
git commit -m "feat: implement system upgrade handler with download, extract, and auto-restart"
```

---

## Summary

| Task | Issue | Files Changed |
|------|-------|---------------|
| 1 | Web→Device config sync crash | `shadow_ui.c`, `shadow_ui.js` |
| 2 | Number input debouncing | `config.html` |
| 3 | Settings section alignment | `shadow_ui.js` (verify only, may be no-op) |
| 4 | Module install loading spinner | `module_detail.html`, `style.css` |
| 5 | System upgrade stub | `main.go`, `system.html` |
