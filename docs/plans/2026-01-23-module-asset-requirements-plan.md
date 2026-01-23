# Module Asset Requirements Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add scrollable descriptions, post-install messages, and missing asset error overlays for modules requiring external files.

**Architecture:** Extend release.json with metadata fields, create scrollable text UI component, add get_error to plugin API, show overlays for post-install and load errors.

**Tech Stack:** JavaScript (QuickJS), C (plugin API), JSON

---

## Task 1: Extend release.json Parsing

**Files:**
- Modify: `src/shared/store_utils.mjs:58-89`

**Step 1: Update fetchReleaseJson to extract new fields**

In `fetchReleaseJson()`, extend the return object to include new fields from release.json:

```javascript
return {
    version: release.version,
    download_url: release.download_url,
    install_path: release.install_path || '',
    /* New fields */
    name: release.name || '',
    description: release.description || '',
    requires: release.requires || '',
    post_install: release.post_install || '',
    repo_url: release.repo_url || ''
};
```

**Step 2: Update loadCatalogFromCache to merge new fields**

In the loop that processes modules (around line 138), add the new fields:

```javascript
if (release) {
    mod.latest_version = release.version;
    mod.download_url = release.download_url;
    mod.install_path = release.install_path;
    /* Merge new fields, preferring release.json over catalog */
    if (release.name) mod.name = release.name;
    if (release.description) mod.description = release.description;
    mod.requires = release.requires;
    mod.post_install = release.post_install;
    mod.repo_url = release.repo_url;
}
```

**Step 3: Verify**

The store should still load and display modules. New fields will be empty until module release.json files are updated.

**Step 4: Commit**

```bash
git add src/shared/store_utils.mjs
git commit -m "feat(store): extend release.json parsing for description, requires, post_install"
```

---

## Task 2: Create Scrollable Text Component

**Files:**
- Create: `src/shared/scrollable_text.mjs`

**Step 1: Create the scrollable text module**

```javascript
/*
 * Scrollable Text Component
 *
 * Displays multi-line text that scrolls with jog wheel, with a fixed
 * action button at the bottom that becomes selected after scrolling
 * past all text.
 */

const SCREEN_WIDTH = 128;
const CHAR_WIDTH = 6;
const LINE_HEIGHT = 10;
const MAX_CHARS_PER_LINE = 20;

/**
 * Word-wrap text into lines
 * @param {string} text - Text to wrap
 * @param {number} maxChars - Max characters per line
 * @returns {string[]} Array of lines
 */
export function wrapText(text, maxChars = MAX_CHARS_PER_LINE) {
    if (!text) return [];

    const words = text.split(/\s+/);
    const lines = [];
    let currentLine = '';

    for (const word of words) {
        if (currentLine.length === 0) {
            currentLine = word;
        } else if (currentLine.length + 1 + word.length <= maxChars) {
            currentLine += ' ' + word;
        } else {
            lines.push(currentLine);
            currentLine = word;
        }
    }
    if (currentLine) {
        lines.push(currentLine);
    }

    return lines;
}

/**
 * Create scrollable text state
 * @param {Object} options
 * @param {string[]} options.lines - Pre-wrapped text lines
 * @param {string} options.actionLabel - Label for action button (e.g., "Install")
 * @param {number} options.visibleLines - Number of visible lines (default 4)
 * @returns {Object} State object
 */
export function createScrollableText({ lines, actionLabel, visibleLines = 4 }) {
    return {
        lines: lines || [],
        actionLabel: actionLabel || 'OK',
        visibleLines,
        scrollOffset: 0,
        actionSelected: false
    };
}

/**
 * Handle jog input for scrollable text
 * @param {Object} state - Scrollable text state
 * @param {number} delta - Jog delta (-1 or 1)
 * @returns {boolean} true if state changed
 */
export function handleScrollableTextJog(state, delta) {
    const maxScroll = Math.max(0, state.lines.length - state.visibleLines);

    if (delta > 0) {
        /* Scroll down */
        if (state.actionSelected) {
            return false; /* Already at bottom */
        }
        if (state.scrollOffset >= maxScroll) {
            /* At end of text, select action */
            state.actionSelected = true;
            return true;
        }
        state.scrollOffset++;
        return true;
    } else if (delta < 0) {
        /* Scroll up */
        if (state.actionSelected) {
            state.actionSelected = false;
            return true;
        }
        if (state.scrollOffset > 0) {
            state.scrollOffset--;
            return true;
        }
    }
    return false;
}

/**
 * Check if action is selected
 * @param {Object} state - Scrollable text state
 * @returns {boolean}
 */
export function isActionSelected(state) {
    return state.actionSelected;
}

/**
 * Draw scrollable text area
 * @param {Object} options
 * @param {Object} options.state - Scrollable text state
 * @param {number} options.topY - Top Y position of text area
 * @param {number} options.bottomY - Bottom Y position (above action button)
 * @param {number} options.actionY - Y position of action button
 */
export function drawScrollableText({ state, topY, bottomY, actionY }) {
    const { lines, scrollOffset, actionSelected, actionLabel, visibleLines } = state;

    /* Draw visible text lines */
    const endIdx = Math.min(scrollOffset + visibleLines, lines.length);
    for (let i = scrollOffset; i < endIdx; i++) {
        const y = topY + (i - scrollOffset) * LINE_HEIGHT;
        print(4, y, lines[i], 1);
    }

    /* Draw scroll indicators */
    const indicatorX = 122;
    if (scrollOffset > 0) {
        /* Up arrow */
        drawArrowUp(indicatorX, topY);
    }
    if (scrollOffset < lines.length - visibleLines && !actionSelected) {
        /* Down arrow */
        drawArrowDown(indicatorX, bottomY - 4);
    }

    /* Draw action button */
    const buttonText = `[${actionLabel}]`;
    const buttonWidth = buttonText.length * CHAR_WIDTH + 8;
    const buttonX = (SCREEN_WIDTH - buttonWidth) / 2;

    if (actionSelected) {
        fill_rect(buttonX - 2, actionY - 2, buttonWidth + 4, 14, 1);
        print(buttonX + 4, actionY, buttonText, 0);
    } else {
        print(buttonX + 4, actionY, buttonText, 1);
    }
}

/* Arrow drawing helpers */
function drawArrowUp(x, y) {
    set_pixel(x + 2, y, 1);
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    for (let i = 0; i < 5; i++) set_pixel(x + i, y + 2, 1);
}

function drawArrowDown(x, y) {
    for (let i = 0; i < 5; i++) set_pixel(x + i, y, 1);
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    set_pixel(x + 2, y + 2, 1);
}
```

**Step 2: Commit**

```bash
git add src/shared/scrollable_text.mjs
git commit -m "feat(ui): add scrollable text component for module descriptions"
```

---

## Task 3: Add Post-Install Overlay Helper

**Files:**
- Modify: `src/shared/menu_layout.mjs`

**Step 1: Add drawPostInstallOverlay function**

Add after the existing `drawStatusOverlay` function (around line 317):

```javascript
/**
 * Draw a post-install message overlay
 * @param {string} title - Title (e.g., "Install Complete")
 * @param {string[]} messageLines - Pre-wrapped message lines
 */
export function drawPostInstallOverlay(title, messageLines) {
    const lineCount = Math.min(messageLines.length, 4);
    const boxHeight = 24 + lineCount * 10;
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - boxHeight) / 2;

    /* Background and double border */
    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 0);
    drawRect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 1);
    drawRect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, boxHeight - 2, 1);

    /* Center title */
    const titleW = title.length * 6;
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 6, title, 1);

    /* Message lines */
    for (let i = 0; i < lineCount; i++) {
        const line = messageLines[i];
        const lineW = line.length * 6;
        print(Math.floor((SCREEN_WIDTH - lineW) / 2), boxY + 18 + i * 10, line, 1);
    }
}
```

**Step 2: Export the function**

The function is already exported via the `export` keyword.

**Step 3: Commit**

```bash
git add src/shared/menu_layout.mjs
git commit -m "feat(ui): add drawPostInstallOverlay helper"
```

---

## Task 4: Update Module Store UI - Scrollable Detail

**Files:**
- Modify: `src/modules/store/ui.js`

**Step 1: Add imports for scrollable text**

At the top of the file, add:

```javascript
import {
    wrapText,
    createScrollableText,
    handleScrollableTextJog,
    isActionSelected,
    drawScrollableText
} from '../../shared/scrollable_text.mjs';

import { drawPostInstallOverlay } from '../../shared/menu_layout.mjs';
```

**Step 2: Add state variables**

Add near other state variables (around line 66):

```javascript
let detailScrollState = null;
let showingPostInstall = false;
let postInstallLines = [];
```

**Step 3: Add new state constant**

```javascript
const STATE_POST_INSTALL = 'post_install';
```

**Step 4: Update drawModuleDetail function**

Replace the existing `drawModuleDetail` function (lines 716-778) with the new scrollable version:

```javascript
/* Draw module detail screen with scrollable description */
function drawModuleDetail() {
    clear_screen();
    const status = getModuleStatus(currentModule);

    /* Header with name and version */
    let versionStr;
    let title = currentModule.name;
    if (status.installed && status.hasUpdate) {
        const installedVer = installedModules[currentModule.id] || '?';
        versionStr = `${installedVer}->${currentModule.latest_version}`;
        if (title.length > 8) title = title.substring(0, 7) + '~';
    } else {
        versionStr = `v${currentModule.latest_version}`;
        if (title.length > 14) title = title.substring(0, 13) + '~';
    }
    drawMenuHeader(title, versionStr);

    /* Initialize scroll state if needed */
    if (!detailScrollState || detailScrollState.moduleId !== currentModule.id) {
        const descLines = wrapText(currentModule.description || 'No description available.', 20);

        /* Add requires line if present */
        if (currentModule.requires) {
            descLines.push('');
            descLines.push('Requires:');
            const reqLines = wrapText(currentModule.requires, 18);
            descLines.push(...reqLines);
        }

        /* Determine action label */
        let actionLabel;
        if (status.installed) {
            actionLabel = status.hasUpdate ? 'Update' : 'Reinstall';
        } else {
            actionLabel = 'Install';
        }

        detailScrollState = createScrollableText({
            lines: descLines,
            actionLabel,
            visibleLines: 4
        });
        detailScrollState.moduleId = currentModule.id;
    }

    /* Draw scrollable content */
    drawScrollableText({
        state: detailScrollState,
        topY: 16,
        bottomY: 48,
        actionY: 52
    });
}
```

**Step 5: Update handleJogWheel for STATE_MODULE_DETAIL**

In the `handleJogWheel` function, update the STATE_MODULE_DETAIL case:

```javascript
case STATE_MODULE_DETAIL: {
    if (detailScrollState) {
        handleScrollableTextJog(detailScrollState, delta);
    }
    break;
}
```

**Step 6: Update handleJogClick for STATE_MODULE_DETAIL**

In the `handleJogClick` function, update the STATE_MODULE_DETAIL case:

```javascript
case STATE_MODULE_DETAIL: {
    if (!detailScrollState || !isActionSelected(detailScrollState)) {
        break; /* Can't click until action is selected */
    }
    const status = getModuleStatus(currentModule);
    installModule(currentModule);
    break;
}
```

**Step 7: Update installModule to show post-install overlay**

Find the `installModule` function and update it to show post-install message:

```javascript
function installModule(mod) {
    state = STATE_LOADING;
    loadingTitle = 'Installing';
    loadingMessage = `${mod.name} v${mod.latest_version}`;
    drawScreen();

    const result = sharedInstallModule(mod, hostVersion);

    if (result.success) {
        scanModules();
        /* Check for post_install message */
        if (mod.post_install) {
            postInstallLines = wrapText(mod.post_install, 18);
            showingPostInstall = true;
            state = STATE_POST_INSTALL;
        } else {
            resultMessage = `Installed ${mod.name}`;
            state = STATE_RESULT;
        }
    } else {
        errorMessage = result.error || 'Install failed';
        state = STATE_ERROR;
    }
}
```

**Step 8: Add STATE_POST_INSTALL handling**

Add case in `handleJogClick`:

```javascript
case STATE_POST_INSTALL:
    showingPostInstall = false;
    resultMessage = `Installed ${currentModule.name}`;
    state = STATE_RESULT;
    break;
```

Add case in `handleBack`:

```javascript
case STATE_POST_INSTALL:
    showingPostInstall = false;
    resultMessage = `Installed ${currentModule.name}`;
    state = STATE_RESULT;
    break;
```

**Step 9: Add drawing for STATE_POST_INSTALL**

In `drawScreen()`, add:

```javascript
case STATE_POST_INSTALL:
    drawPostInstallOverlay('Install Complete', postInstallLines);
    break;
```

**Step 10: Reset scroll state on back**

In the STATE_MODULE_DETAIL case of `handleBack`:

```javascript
case STATE_MODULE_DETAIL:
    currentModule = null;
    detailScrollState = null;
    /* ... rest of existing code ... */
```

**Step 11: Commit**

```bash
git add src/modules/store/ui.js
git commit -m "feat(store): scrollable module detail with post-install message"
```

---

## Task 5: Update Shadow UI Store Picker

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add imports**

Near the top where other imports are:

```javascript
import {
    wrapText,
    createScrollableText,
    handleScrollableTextJog,
    isActionSelected,
    drawScrollableText
} from '../shared/scrollable_text.mjs';

import { drawPostInstallOverlay } from '../shared/menu_layout.mjs';
```

**Step 2: Add state variables**

Near other store picker state (around line 315):

```javascript
let storeDetailScrollState = null;
let storeShowingPostInstall = false;
let storePostInstallLines = [];
```

**Step 3: Add new view constant**

Add to the VIEWS object:

```javascript
STORE_PICKER_POST_INSTALL: "storepickerpostinstall",
```

**Step 4: Update drawStorePickerDetail**

Replace the `drawStorePickerDetail` function with scrollable version:

```javascript
/* Draw store picker module detail with scrollable description */
function drawStorePickerDetail() {
    clear_screen();

    const mod = storePickerCurrentModule;
    if (!mod) return;

    const status = getModuleStatus(mod, storeInstalledModules);

    /* Header */
    let title = mod.name;
    let versionStr = `v${mod.latest_version}`;
    if (status.installed && status.hasUpdate) {
        versionStr = `${status.installedVersion}->${mod.latest_version}`;
        if (title.length > 8) title = title.substring(0, 7) + '~';
    } else {
        if (title.length > 12) title = title.substring(0, 11) + '~';
    }
    drawHeader(title, versionStr);

    /* Initialize scroll state if needed */
    if (!storeDetailScrollState || storeDetailScrollState.moduleId !== mod.id) {
        const descLines = wrapText(mod.description || 'No description available.', 20);

        if (mod.requires) {
            descLines.push('');
            descLines.push('Requires:');
            const reqLines = wrapText(mod.requires, 18);
            descLines.push(...reqLines);
        }

        let actionLabel;
        if (status.installed) {
            actionLabel = status.hasUpdate ? 'Update' : 'Reinstall';
        } else {
            actionLabel = 'Install';
        }

        storeDetailScrollState = createScrollableText({
            lines: descLines,
            actionLabel,
            visibleLines: 4
        });
        storeDetailScrollState.moduleId = mod.id;
    }

    drawScrollableText({
        state: storeDetailScrollState,
        topY: 16,
        bottomY: 48,
        actionY: 52
    });
}
```

**Step 5: Update handleStorePickerDetailJog**

```javascript
function handleStorePickerDetailJog(delta) {
    if (storeDetailScrollState) {
        handleScrollableTextJog(storeDetailScrollState, delta);
        needsRedraw = true;
    }
}
```

**Step 6: Update handleStorePickerDetailSelect**

At the beginning, check if action is selected:

```javascript
function handleStorePickerDetailSelect() {
    if (!storeDetailScrollState || !isActionSelected(storeDetailScrollState)) {
        return; /* Can't select until scrolled to action */
    }
    /* ... rest of existing function ... */
}
```

**Step 7: Add post-install handling after successful install**

In `handleStorePickerDetailSelect`, after successful install:

```javascript
if (result.success) {
    storeInstalledModules = scanInstalledModules();
    if (mod.post_install) {
        storePostInstallLines = wrapText(mod.post_install, 18);
        storeShowingPostInstall = true;
        view = VIEWS.STORE_PICKER_POST_INSTALL;
    } else {
        storePickerMessage = `Installed ${mod.name}`;
        view = VIEWS.STORE_PICKER_RESULT;
    }
}
```

**Step 8: Add handlers for STORE_PICKER_POST_INSTALL**

In jog click handler:

```javascript
case VIEWS.STORE_PICKER_POST_INSTALL:
    storeShowingPostInstall = false;
    storePickerMessage = `Installed ${storePickerCurrentModule.name}`;
    view = VIEWS.STORE_PICKER_RESULT;
    needsRedraw = true;
    break;
```

In back handler:

```javascript
case VIEWS.STORE_PICKER_POST_INSTALL:
    storeShowingPostInstall = false;
    storePickerMessage = `Installed ${storePickerCurrentModule.name}`;
    view = VIEWS.STORE_PICKER_RESULT;
    needsRedraw = true;
    break;
```

**Step 9: Add draw case**

```javascript
case VIEWS.STORE_PICKER_POST_INSTALL:
    drawPostInstallOverlay('Install Complete', storePostInstallLines);
    break;
```

**Step 10: Reset scroll state on back from detail**

In the STORE_PICKER_DETAIL case of back handler:

```javascript
case VIEWS.STORE_PICKER_DETAIL:
    view = VIEWS.STORE_PICKER_LIST;
    storePickerCurrentModule = null;
    storeDetailScrollState = null;
    break;
```

**Step 11: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): scrollable store detail with post-install message"
```

---

## Task 6: Extend Plugin API with get_error

**Files:**
- Modify: `src/host/plugin_api_v1.h`

**Step 1: Add get_error to plugin_api_v1**

In `plugin_api_v1_t` struct, add after `render_block`:

```c
    /* Get error message after on_load failure (optional)
     * buf: output buffer for error message
     * buf_len: size of output buffer
     * Returns: length written, or 0 if no error
     */
    int (*get_error)(char *buf, int buf_len);
```

**Step 2: Add get_error to plugin_api_v2**

In `plugin_api_v2_t` struct, add after `render_block`:

```c
    /* Get error message after create_instance failure (optional) */
    int (*get_error)(void *instance, char *buf, int buf_len);
```

**Step 3: Commit**

```bash
git add src/host/plugin_api_v1.h
git commit -m "feat(api): add get_error function to plugin API v1 and v2"
```

---

## Task 7: Update Module Manager for Error Handling

**Files:**
- Modify: `src/host/module_manager.h`
- Modify: `src/host/module_manager.c`

**Step 1: Add error buffer to module_manager struct**

In `module_manager.h`, add to `module_manager_t`:

```c
    /* Last load error message */
    char last_error[256];
```

**Step 2: Add mm_get_last_error function declaration**

In `module_manager.h`:

```c
/* Get last load error message */
const char* mm_get_last_error(module_manager_t *mm);
```

**Step 3: Update mm_load_module in module_manager.c**

At the start of `mm_load_module`, clear the error:

```c
mm->last_error[0] = '\0';
```

When v2 create_instance fails (around line 330), try to get error:

```c
if (!mm->plugin_instance) {
    printf("mm: v2 create_instance failed, trying v1\n");
    /* Try to get error message */
    if (mm->plugin_v2->get_error) {
        mm->plugin_v2->get_error(NULL, mm->last_error, sizeof(mm->last_error));
    }
    mm->plugin_v2 = NULL;
}
```

When v1 on_load fails (around line 371), try to get error:

```c
if (ret != 0) {
    printf("mm: plugin on_load failed with %d\n", ret);
    /* Try to get error message */
    if (mm->plugin->get_error) {
        mm->plugin->get_error(mm->last_error, sizeof(mm->last_error));
    }
    if (mm->last_error[0] == '\0') {
        snprintf(mm->last_error, sizeof(mm->last_error), "Module failed to load");
    }
    dlclose(mm->dsp_handle);
    mm->dsp_handle = NULL;
    mm->plugin = NULL;
    return -1;
}
```

**Step 4: Implement mm_get_last_error**

```c
const char* mm_get_last_error(module_manager_t *mm) {
    return mm->last_error;
}
```

**Step 5: Commit**

```bash
git add src/host/module_manager.h src/host/module_manager.c
git commit -m "feat(host): capture plugin error messages on load failure"
```

---

## Task 8: Expose Error to JavaScript

**Files:**
- Modify: `src/move_anything.c`

**Step 1: Find host_load_module JS binding**

Search for `host_load_module` implementation and update it to store error.

**Step 2: Add host_get_last_error JS function**

Add a new JS function `host_get_last_error()` that returns the error message:

```c
static JSValue js_host_get_last_error(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
    const char *error = mm_get_last_error(&module_manager);
    if (error && error[0]) {
        return JS_NewString(ctx, error);
    }
    return JS_NULL;
}
```

Register the function:

```c
JS_SetPropertyStr(ctx, global, "host_get_last_error",
    JS_NewCFunction(ctx, js_host_get_last_error, "host_get_last_error", 0));
```

**Step 3: Commit**

```bash
git add src/move_anything.c
git commit -m "feat(host): expose module load error to JavaScript"
```

---

## Task 9: Show Error Overlay on Module Load Failure

**Files:**
- Modify: `src/host/menu_ui.js`
- Modify: `src/host/menu_main.mjs`

**Step 1: Import overlay helper**

In the relevant menu file, import:

```javascript
import { drawPostInstallOverlay } from '../shared/menu_layout.mjs';
import { wrapText } from '../shared/scrollable_text.mjs';
```

**Step 2: Add error state**

```javascript
let showingLoadError = false;
let loadErrorLines = [];
```

**Step 3: Check for error after module load**

After calling `host_load_module`, check result and show error:

```javascript
const result = host_load_module(moduleIndex);
if (result !== 0) {
    const error = host_get_last_error() || 'Module failed to load';
    loadErrorLines = wrapText(error, 18);
    showingLoadError = true;
    /* Stay on menu, show overlay */
}
```

**Step 4: Handle error dismissal**

On jog click or back when showing error:

```javascript
if (showingLoadError) {
    showingLoadError = false;
    return;
}
```

**Step 5: Draw error overlay**

In draw function:

```javascript
if (showingLoadError) {
    drawPostInstallOverlay('Load Error', loadErrorLines);
}
```

**Step 6: Commit**

```bash
git add src/host/menu_ui.js src/host/menu_main.mjs
git commit -m "feat(ui): show error overlay when module fails to load"
```

---

## Task 10: Update DX7 Module

**Files (external repo):**
- Modify: `move-anything-dx7/release.json`
- Modify: `move-anything-dx7/src/dsp/plugin.c`

**Step 1: Update release.json**

```json
{
  "version": "0.2.1",
  "download_url": "https://github.com/charlesvestal/move-anything-dx7/releases/download/v0.2.1/dx7-module.tar.gz",
  "repo_url": "https://github.com/charlesvestal/move-anything-dx7",
  "name": "DX7 Synth",
  "description": "Yamaha DX7 FM synthesizer emulation with full 6-operator FM synthesis. Loads standard .syx patch banks from the original DX7 or compatible editors.",
  "requires": ".syx patch files (DX7 voice banks)",
  "post_install": "Place .syx files in modules/sound_generators/dx7/patches/"
}
```

**Step 2: Add error state to plugin**

```c
static char load_error[256] = "";
```

**Step 3: Update on_load to set error**

When no patches found:

```c
if (patch_count == 0) {
    snprintf(load_error, sizeof(load_error),
        "No .syx patches found.\nAdd files to patches/ folder.");
    return -1;
}
load_error[0] = '\0';
return 0;
```

**Step 4: Implement get_error**

```c
static int dx7_get_error(char *buf, int buf_len) {
    if (load_error[0] && buf && buf_len > 0) {
        strncpy(buf, load_error, buf_len - 1);
        buf[buf_len - 1] = '\0';
        return strlen(buf);
    }
    return 0;
}
```

**Step 5: Add to plugin API struct**

```c
static plugin_api_v1_t plugin_api = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .on_load = dx7_on_load,
    .on_unload = dx7_on_unload,
    .on_midi = dx7_on_midi,
    .set_param = dx7_set_param,
    .get_param = dx7_get_param,
    .render_block = dx7_render_block,
    .get_error = dx7_get_error
};
```

**Step 6: Commit and tag**

```bash
git add release.json src/dsp/plugin.c
git commit -m "feat: add asset requirement messaging"
git tag v0.2.2
git push --tags
```

---

## Task 11: Update SF2 Module

**Files (external repo):**
- Modify: `move-anything-sf2/release.json`
- Modify: `move-anything-sf2/src/dsp/plugin.c`

Same pattern as Task 10, with SF2-specific messages:

**release.json:**
```json
{
  "description": "SoundFont synthesizer using TinySoundFont. Load .sf2 files for realistic instrument sounds including pianos, strings, brass, and more.",
  "requires": ".sf2 soundfont files",
  "post_install": "Place .sf2 files in modules/sound_generators/sf2/soundfonts/"
}
```

**Error message:**
```c
"No .sf2 files found.\nAdd soundfonts to soundfonts/ folder."
```

---

## Task 12: Update JV-880 Module

**Files (external repo):**
- Modify: `move-anything-jv880/release.json`
- Modify: `move-anything-jv880/src/dsp/plugin.cpp`

Same pattern as Task 10, with JV-880-specific messages:

**release.json:**
```json
{
  "description": "Roland JV-880 rompler emulation. Requires original ROM files dumped from JV-880/JV-1080 hardware.",
  "requires": "ROM files from original JV-880 hardware",
  "post_install": "Place ROM files (jv880_waverom*.bin, jv880_nvram.bin) in modules/sound_generators/jv880/"
}
```

**Error message:**
```c
"ROM files not found.\nSee README for required files."
```

---

## Verification

After completing all tasks:

1. **Store displays descriptions**: Open Module Store, navigate to a module, see scrollable description
2. **Requires line shown**: Modules with `requires` show it after description
3. **Post-install message**: After installing DX7/SF2/JV880, see post-install overlay
4. **Load error overlay**: Load DX7 without patches, see error overlay
5. **Shadow UI works**: Same behavior in Shadow UI store picker

---

## Files Modified Summary

**Main repo:**
- `src/shared/store_utils.mjs` - Extended release.json parsing
- `src/shared/scrollable_text.mjs` - New scrollable text component
- `src/shared/menu_layout.mjs` - Post-install overlay helper
- `src/modules/store/ui.js` - Scrollable detail, post-install
- `src/shadow/shadow_ui.js` - Same for shadow store
- `src/host/plugin_api_v1.h` - get_error function
- `src/host/module_manager.h` - Error buffer and getter
- `src/host/module_manager.c` - Error capture on load failure
- `src/move_anything.c` - JS binding for error
- `src/host/menu_ui.js` - Error overlay display

**External repos:**
- `move-anything-dx7/release.json`, `src/dsp/plugin.c`
- `move-anything-sf2/release.json`, `src/dsp/plugin.c`
- `move-anything-jv880/release.json`, `src/dsp/plugin.cpp`
