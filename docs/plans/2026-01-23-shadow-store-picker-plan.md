# Shadow UI Store Picker Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add module store access from Shadow UI's module picker so users can browse and install modules without leaving the chain editor.

**Architecture:** Extract shared store utilities from `store/ui.js` into `shared/store_utils.mjs`. Add new views to Shadow UI for browsing/installing modules filtered by category. Navigation is restricted - back always returns to component select.

**Tech Stack:** QuickJS JavaScript, Move hardware (128x64 1-bit display)

---

### Task 1: Create store_utils.mjs with constants and version functions

**Files:**
- Create: `src/shared/store_utils.mjs`

**Step 1: Create the shared store utilities file with constants and version comparison**

```javascript
/*
 * Shared Store Utilities
 *
 * Common functions for catalog fetching, module installation, and version comparison.
 * Used by both the Module Store UI and Shadow UI store picker.
 */

import * as std from 'std';
import * as os from 'os';

/* Constants */
export const CATALOG_URL = 'https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json';
export const CATALOG_CACHE_PATH = '/data/UserData/move-anything/catalog-cache.json';
export const MODULES_DIR = '/data/UserData/move-anything/modules';
export const BASE_DIR = '/data/UserData/move-anything';
export const TMP_DIR = '/data/UserData/move-anything/tmp';
export const HOST_VERSION_FILE = '/data/UserData/move-anything/host/version.txt';

/* Categories */
export const CATEGORIES = [
    { id: 'sound_generator', name: 'Sound Generators' },
    { id: 'audio_fx', name: 'Audio FX' },
    { id: 'midi_fx', name: 'MIDI FX' },
    { id: 'midi_source', name: 'MIDI Sources' },
    { id: 'utility', name: 'Utilities' }
];

/* Compare semver versions: returns 1 if a > b, -1 if a < b, 0 if equal */
export function compareVersions(a, b) {
    const partsA = a.split('.').map(n => parseInt(n, 10) || 0);
    const partsB = b.split('.').map(n => parseInt(n, 10) || 0);

    for (let i = 0; i < Math.max(partsA.length, partsB.length); i++) {
        const numA = partsA[i] || 0;
        const numB = partsB[i] || 0;
        if (numA > numB) return 1;
        if (numA < numB) return -1;
    }
    return 0;
}

/* Check if version a is newer than version b */
export function isNewerVersion(a, b) {
    return compareVersions(a, b) > 0;
}

/* Get install subdirectory based on component_type */
export function getInstallSubdir(componentType) {
    switch (componentType) {
        case 'sound_generator': return 'sound_generators';
        case 'audio_fx': return 'audio_fx';
        case 'midi_fx': return 'midi_fx';
        case 'utility': return 'utilities';
        default: return 'other';
    }
}
```

**Step 2: Verify file was created correctly**

Run: `head -50 src/shared/store_utils.mjs`
Expected: Shows the constants and version functions

**Step 3: Commit**

```bash
git add src/shared/store_utils.mjs
git commit -m "feat: create store_utils.mjs with constants and version functions"
```

---

### Task 2: Add catalog and release fetching to store_utils.mjs

**Files:**
- Modify: `src/shared/store_utils.mjs`

**Step 1: Add catalog fetching functions**

Append to `src/shared/store_utils.mjs`:

```javascript

/* Fetch release info from release.json in repo */
export function fetchReleaseJson(github_repo) {
    const cacheFile = `${TMP_DIR}/${github_repo.replace('/', '_')}_release.json`;
    const releaseUrl = `https://raw.githubusercontent.com/${github_repo}/main/release.json`;

    const success = host_http_download(releaseUrl, cacheFile);
    if (!success) {
        console.log(`Failed to fetch release.json for ${github_repo}`);
        return null;
    }

    try {
        const jsonStr = std.loadFile(cacheFile);
        if (!jsonStr) return null;

        const release = JSON.parse(jsonStr);

        if (!release.version || !release.download_url) {
            console.log(`Invalid release.json format for ${github_repo}`);
            return null;
        }

        return {
            version: release.version,
            download_url: release.download_url,
            install_path: release.install_path || ''
        };
    } catch (e) {
        console.log(`Failed to parse release.json for ${github_repo}: ${e}`);
        return null;
    }
}

/* Fetch catalog from network, returns { success, catalog, error } */
export function fetchCatalog(onProgress) {
    if (onProgress) onProgress('Loading Catalog', 'Fetching...');

    const success = host_http_download(CATALOG_URL, CATALOG_CACHE_PATH);

    if (success) {
        return loadCatalogFromCache(onProgress);
    } else {
        /* Try cached version */
        if (host_file_exists(CATALOG_CACHE_PATH)) {
            console.log('Using cached catalog (network unavailable)');
            return loadCatalogFromCache(onProgress);
        } else {
            return { success: false, catalog: null, error: 'Could not fetch catalog' };
        }
    }
}

/* Load catalog from cache file, returns { success, catalog, error } */
export function loadCatalogFromCache(onProgress) {
    try {
        const jsonStr = std.loadFile(CATALOG_CACHE_PATH);
        if (!jsonStr) {
            return { success: false, catalog: null, error: 'No catalog available' };
        }

        const catalog = JSON.parse(jsonStr);
        console.log(`Loaded catalog with ${catalog.modules ? catalog.modules.length : 0} modules`);

        /* For catalog v2+, fetch release info from release.json files */
        if (catalog.catalog_version >= 2 && catalog.modules) {
            for (let i = 0; i < catalog.modules.length; i++) {
                const mod = catalog.modules[i];
                if (mod.github_repo) {
                    if (onProgress) onProgress('Loading Catalog', mod.name);

                    const release = fetchReleaseJson(mod.github_repo);
                    if (release) {
                        mod.latest_version = release.version;
                        mod.download_url = release.download_url;
                        mod.install_path = release.install_path;
                    } else {
                        mod.latest_version = '0.0.0';
                        mod.download_url = null;
                    }
                }
            }
        }

        return { success: true, catalog, error: null };
    } catch (e) {
        console.log('Failed to parse catalog: ' + e);
        return { success: false, catalog: null, error: 'Invalid catalog format' };
    }
}

/* Get modules for a specific category */
export function getModulesForCategory(catalog, categoryId) {
    if (!catalog || !catalog.modules) return [];
    return catalog.modules.filter(m => m.component_type === categoryId);
}

/* Get module install status */
export function getModuleStatus(mod, installedModules) {
    const installedVersion = installedModules[mod.id];
    if (!installedVersion) {
        return { installed: false, hasUpdate: false };
    }
    const hasUpdate = isNewerVersion(mod.latest_version, installedVersion);
    return { installed: true, hasUpdate, installedVersion };
}
```

**Step 2: Verify the additions**

Run: `grep -n "export function" src/shared/store_utils.mjs`
Expected: Shows all exported functions including fetchCatalog, loadCatalogFromCache, etc.

**Step 3: Commit**

```bash
git add src/shared/store_utils.mjs
git commit -m "feat: add catalog fetching functions to store_utils"
```

---

### Task 3: Add module installation to store_utils.mjs

**Files:**
- Modify: `src/shared/store_utils.mjs`

**Step 1: Add installation and removal functions**

Append to `src/shared/store_utils.mjs`:

```javascript

/* Get current host version */
export function getHostVersion() {
    try {
        const versionStr = std.loadFile(HOST_VERSION_FILE);
        if (versionStr) {
            return versionStr.trim();
        }
    } catch (e) {
        /* Fall through */
    }
    return '1.0.0';
}

/* Install a module, returns { success, error } */
export function installModule(mod, hostVersion) {
    /* Check host version compatibility */
    if (mod.min_host_version && compareVersions(mod.min_host_version, hostVersion) > 0) {
        return { success: false, error: `Requires host v${mod.min_host_version}` };
    }

    /* Check if module has a download URL */
    if (!mod.download_url) {
        return { success: false, error: 'No release available' };
    }

    const tarPath = `${TMP_DIR}/${mod.id}-module.tar.gz`;

    /* Download the module tarball */
    console.log(`Downloading: ${mod.download_url}`);
    const downloadOk = host_http_download(mod.download_url, tarPath);
    if (!downloadOk) {
        console.log('Download failed');
        return { success: false, error: 'Download failed' };
    }

    /* Determine extraction path based on component_type */
    const subdir = getInstallSubdir(mod.component_type);
    const extractDir = subdir ? `${MODULES_DIR}/${subdir}` : MODULES_DIR;

    /* Extract to appropriate directory */
    const extractOk = host_extract_tar(tarPath, extractDir);
    if (!extractOk) {
        return { success: false, error: 'Extract failed' };
    }

    /* Rescan modules */
    host_rescan_modules();

    return { success: true, error: null };
}

/* Remove a module, returns { success, error } */
export function removeModule(mod) {
    /* Determine module path based on component_type */
    const subdir = getInstallSubdir(mod.component_type);
    const modulePath = subdir
        ? `${MODULES_DIR}/${subdir}/${mod.id}`
        : `${MODULES_DIR}/${mod.id}`;

    /* Remove the module directory */
    const removeOk = host_remove_dir(modulePath);
    if (!removeOk) {
        return { success: false, error: 'Remove failed' };
    }

    /* Rescan modules */
    host_rescan_modules();

    return { success: true, error: null };
}

/* Scan installed modules, returns { moduleId: version } map */
export function scanInstalledModules() {
    const installedModules = {};
    const modules = host_list_modules();
    for (const mod of modules) {
        installedModules[mod.id] = mod.version;
    }
    return installedModules;
}
```

**Step 2: Verify all exports**

Run: `grep -c "export function\|export const" src/shared/store_utils.mjs`
Expected: Should show ~15+ exports

**Step 3: Commit**

```bash
git add src/shared/store_utils.mjs
git commit -m "feat: add module install/remove functions to store_utils"
```

---

### Task 4: Refactor store/ui.js to use shared utilities

**Files:**
- Modify: `src/modules/store/ui.js`

**Step 1: Add import statement at the top of the file**

After the existing imports (around line 25), add:

```javascript
import {
    CATALOG_URL, CATALOG_CACHE_PATH, MODULES_DIR, BASE_DIR, TMP_DIR, HOST_VERSION_FILE,
    CATEGORIES,
    compareVersions, isNewerVersion, getInstallSubdir,
    fetchReleaseJson, fetchCatalog, loadCatalogFromCache, getModulesForCategory,
    getModuleStatus, getHostVersion, installModule, removeModule, scanInstalledModules
} from '/data/UserData/move-anything/shared/store_utils.mjs';
```

**Step 2: Remove the local constant definitions**

Delete lines defining these constants (approximately lines 27-66):
- `CATALOG_URL`
- `CATALOG_CACHE_PATH`
- `MODULES_DIR`
- `BASE_DIR`
- `TMP_DIR`
- `HOST_VERSION_FILE`
- `CATEGORIES`
- `getInstallSubdir` function

**Step 3: Remove local utility functions that are now imported**

Delete these function definitions:
- `compareVersions` (lines ~95-107)
- `isNewerVersion` (lines ~109-112)
- `fetchReleaseJson` (lines ~114-147)
- `getModulesForCategory` (lines ~268-272)
- `getModuleStatus` (lines ~274-282)
- `scanInstalledModules` (lines ~259-266)
- `getHostVersion` (lines ~195-208)

**Step 4: Update fetchCatalog to use shared version**

Replace the local `fetchCatalog` function with a wrapper that uses the shared one:

```javascript
/* Fetch catalog from network */
function fetchCatalogLocal() {
    state = STATE_LOADING;

    const onProgress = (title, message) => {
        loadingTitle = title;
        loadingMessage = message;
        draw();
        host_flush_display();
    };

    onProgress('Loading Catalog', 'Fetching...');
    os.sleep(50);

    const result = fetchCatalog(onProgress);

    if (result.success) {
        catalog = result.catalog;

        /* Fetch host release info separately */
        if (catalog.host && catalog.host.github_repo) {
            onProgress('Loading Catalog', 'Checking host...');
            const hostRelease = fetchReleaseJson(catalog.host.github_repo);
            if (hostRelease) {
                catalog.host.latest_version = hostRelease.version;
                catalog.host.download_url = hostRelease.download_url;
            }
        }

        checkHostUpdate();
        state = STATE_CATEGORIES;
    } else {
        state = STATE_ERROR;
        errorMessage = result.error;
    }
}
```

**Step 5: Update installModule calls to use shared version**

Replace the local `installModule` function body to wrap the shared one:

```javascript
/* Install a module */
function installModuleLocal(mod) {
    state = STATE_INSTALLING;
    loadingTitle = 'Downloading';
    loadingMessage = `${mod.name} v${mod.latest_version}`;
    draw();
    host_flush_display();

    const result = installModule(mod, hostVersion);

    if (result.success) {
        /* Rescan and update local state */
        installedModules = scanInstalledModules();
        state = STATE_RESULT;
        resultMessage = `Installed ${mod.name}`;
    } else {
        state = STATE_RESULT;
        resultMessage = result.error;
    }
}
```

**Step 6: Update removeModule calls similarly**

```javascript
/* Remove a module */
function removeModuleLocal(mod) {
    state = STATE_REMOVING;
    loadingTitle = 'Removing';
    loadingMessage = mod.name;
    draw();
    host_flush_display();

    const result = removeModule(mod);

    if (result.success) {
        installedModules = scanInstalledModules();
        state = STATE_RESULT;
        resultMessage = `Removed ${mod.name}`;
    } else {
        state = STATE_RESULT;
        resultMessage = result.error;
    }
}
```

**Step 7: Update function calls throughout the file**

Replace calls to:
- `fetchCatalog()` → `fetchCatalogLocal()`
- `installModule(mod)` → `installModuleLocal(mod)`
- `removeModule(mod)` → `removeModuleLocal(mod)`
- `getModulesForCategory(categoryId)` → `getModulesForCategory(catalog, categoryId)`
- `getModuleStatus(mod)` → `getModuleStatus(mod, installedModules)`

**Step 8: Update init function**

```javascript
globalThis.init = function() {
    console.log('Module Store starting...');

    state = STATE_LOADING;
    loadingTitle = 'Module Store';
    loadingMessage = 'Loading...';
    draw();
    host_flush_display();
    os.sleep(50);

    hostVersion = getHostVersion();
    installedModules = scanInstalledModules();
    fetchCatalogLocal();
};
```

**Step 9: Build and verify**

Run: `cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything && ./scripts/build.sh`
Expected: Build succeeds without errors

**Step 10: Commit**

```bash
git add src/modules/store/ui.js src/shared/store_utils.mjs
git commit -m "refactor: store/ui.js uses shared store_utils"
```

---

### Task 5: Add store picker state variables to Shadow UI

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add import for store utilities**

Near the top of the file, after other imports, add:

```javascript
import {
    fetchCatalog, getModulesForCategory, getModuleStatus,
    installModule, removeModule, scanInstalledModules, getHostVersion,
    CATEGORIES
} from '/data/UserData/move-anything/shared/store_utils.mjs';
```

**Step 2: Add new view constants**

Find the `VIEWS` object (around line 85-95) and add new views:

```javascript
    STORE_PICKER_LIST: "storepickerlist",
    STORE_PICKER_DETAIL: "storepickerdetail",
    STORE_PICKER_LOADING: "storepickerloading",
    STORE_PICKER_RESULT: "storepickerresult",
```

**Step 3: Add store picker state variables**

Find the state variables section (around line 250-300) and add:

```javascript
/* Store picker state */
let storeCatalog = null;
let storeInstalledModules = {};
let storeHostVersion = '1.0.0';
let storePickerCategory = null;        /* Category ID being browsed */
let storePickerModules = [];           /* Modules available for download */
let storePickerSelectedIndex = 0;      /* Selection in list */
let storePickerCurrentModule = null;   /* Module being viewed in detail */
let storePickerActionIndex = 0;        /* Selected action in detail view */
let storePickerMessage = '';           /* Result/error message */
let storePickerLoadingTitle = '';
let storePickerLoadingMessage = '';
```

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker state variables to shadow_ui"
```

---

### Task 6: Add "[Get more...]" option to module scanner

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Modify scanModulesForType to add "[Get more...]" option**

Find the `scanModulesForType` function (around line 1642) and modify the end of the function, just before `return result;`:

```javascript
    /* Add option to get more modules from store */
    result.push({ id: "__get_more__", name: "[Get more...]" });

    return result;
```

**Step 2: Add helper to map component key to catalog category**

Add this function near `scanModulesForType`:

```javascript
/* Map component key to catalog category ID */
function componentKeyToCategoryId(componentKey) {
    switch (componentKey) {
        case 'synth': return 'sound_generator';
        case 'fx1':
        case 'fx2': return 'audio_fx';
        case 'midiFx': return 'midi_fx';
        default: return null;
    }
}
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add [Get more...] option to module scanner"
```

---

### Task 7: Add store picker entry and catalog fetching

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add function to enter store picker**

Add this function after the `componentKeyToCategoryId` function:

```javascript
/* Enter the store picker for a specific component type */
function enterStorePicker(componentKey) {
    const categoryId = componentKeyToCategoryId(componentKey);
    if (!categoryId) return;

    storePickerCategory = categoryId;
    storePickerSelectedIndex = 0;
    storePickerCurrentModule = null;
    storePickerActionIndex = 0;

    /* Check if we need to fetch catalog */
    if (!storeCatalog) {
        view = VIEWS.STORE_PICKER_LOADING;
        storePickerLoadingTitle = 'Module Store';
        storePickerLoadingMessage = 'Loading catalog...';
        needsRedraw = true;

        /* Fetch will happen in tick via fetchStoreCatalog */
        fetchStoreCatalogAsync();
    } else {
        /* Catalog already loaded, go to list */
        storePickerModules = getModulesForCategory(storeCatalog, categoryId);
        view = VIEWS.STORE_PICKER_LIST;
        needsRedraw = true;
    }
}

/* Async catalog fetch (called from tick when in loading state) */
let storeFetchPending = false;

function fetchStoreCatalogAsync() {
    if (storeFetchPending) return;
    storeFetchPending = true;

    /* This will block but we're already showing loading screen */
    const onProgress = (title, message) => {
        storePickerLoadingTitle = title;
        storePickerLoadingMessage = message;
        needsRedraw = true;
    };

    storeHostVersion = getHostVersion();
    storeInstalledModules = scanInstalledModules();

    const result = fetchCatalog(onProgress);
    storeFetchPending = false;

    if (result.success) {
        storeCatalog = result.catalog;
        storePickerModules = getModulesForCategory(storeCatalog, storePickerCategory);
        view = VIEWS.STORE_PICKER_LIST;
    } else {
        storePickerMessage = result.error || 'Failed to load catalog';
        view = VIEWS.STORE_PICKER_RESULT;
    }
    needsRedraw = true;
}
```

**Step 2: Modify applyComponentSelection to handle "[Get more...]"**

Find the `applyComponentSelection` function (around line 1731) and add at the beginning, after getting `selected`:

```javascript
function applyComponentSelection() {
    const comp = CHAIN_COMPONENTS[selectedChainComponent];
    const selected = availableModules[selectedModuleIndex];

    /* Check if user selected "[Get more...]" */
    if (selected && selected.id === "__get_more__") {
        enterStorePicker(comp.key);
        return;
    }

    /* Rest of existing function... */
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker entry and catalog fetching"
```

---

### Task 8: Add store picker list view drawing

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add drawing function for store picker list**

Add this function in the drawing section (near other draw functions):

```javascript
/* Draw store picker module list */
function drawStorePickerList() {
    clear_screen();

    /* Find category name */
    const cat = CATEGORIES.find(c => c.id === storePickerCategory);
    const catName = cat ? cat.name : 'Modules';

    /* Header */
    fill_rect(0, 0, 128, 11, 1);
    print(2, 1, catName, 0);
    print(100, 1, `(${storePickerModules.length})`, 0);

    if (storePickerModules.length === 0) {
        print(2, 28, "No modules available", 1);
        print(2, 54, "Back: return", 1);
        return;
    }

    /* List area */
    const listTop = 13;
    const listBottom = 52;
    const lineHeight = 10;
    const visibleCount = Math.floor((listBottom - listTop) / lineHeight);

    /* Calculate scroll offset */
    let scrollOffset = 0;
    if (storePickerSelectedIndex >= visibleCount) {
        scrollOffset = storePickerSelectedIndex - visibleCount + 1;
    }

    /* Draw visible items */
    for (let i = 0; i < visibleCount && (i + scrollOffset) < storePickerModules.length; i++) {
        const idx = i + scrollOffset;
        const mod = storePickerModules[idx];
        const y = listTop + (i * lineHeight);
        const isSelected = (idx === storePickerSelectedIndex);

        /* Get status */
        const status = getModuleStatus(mod, storeInstalledModules);
        let statusIcon = '';
        if (status.installed) {
            statusIcon = status.hasUpdate ? '^' : '*';
        }

        if (isSelected) {
            fill_rect(0, y, 128, lineHeight, 1);
            print(2, y + 1, mod.name, 0);
            if (statusIcon) print(120, y + 1, statusIcon, 0);
        } else {
            print(2, y + 1, mod.name, 1);
            if (statusIcon) print(120, y + 1, statusIcon, 1);
        }
    }

    /* Footer */
    fill_rect(0, 54, 128, 10, 1);
    print(2, 55, "Back:return  Jog:browse", 0);
}
```

**Step 2: Add drawing function for store picker loading**

```javascript
/* Draw store picker loading screen */
function drawStorePickerLoading() {
    clear_screen();

    /* Centered loading display */
    const title = storePickerLoadingTitle || 'Loading';
    const msg = storePickerLoadingMessage || '...';

    /* Title */
    fill_rect(0, 20, 128, 14, 1);
    const titleX = Math.max(2, (128 - title.length * 6) / 2);
    print(titleX, 22, title, 0);

    /* Message */
    const msgX = Math.max(2, (128 - msg.length * 6) / 2);
    print(msgX, 40, msg, 1);
}
```

**Step 3: Add drawing function for store picker result**

```javascript
/* Draw store picker result screen */
function drawStorePickerResult() {
    clear_screen();

    /* Header */
    fill_rect(0, 0, 128, 11, 1);
    print(2, 1, "Module Store", 0);

    /* Message */
    const msg = storePickerMessage || 'Done';
    print(2, 28, msg, 1);

    /* Footer */
    fill_rect(0, 54, 128, 10, 1);
    print(2, 55, "Press to continue", 0);
}
```

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker list and loading views"
```

---

### Task 9: Add store picker detail view drawing

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add drawing function for store picker detail**

```javascript
/* Draw store picker module detail */
function drawStorePickerDetail() {
    clear_screen();

    const mod = storePickerCurrentModule;
    if (!mod) return;

    const status = getModuleStatus(mod, storeInstalledModules);

    /* Header with name and version */
    fill_rect(0, 0, 128, 11, 1);
    let title = mod.name;
    if (title.length > 14) title = title.substring(0, 13) + '~';
    print(2, 1, title, 0);
    print(90, 1, `v${mod.latest_version}`, 0);

    /* Description */
    const desc = mod.description || '';
    const truncDesc = desc.length > 21 ? desc.substring(0, 18) + '...' : desc;
    print(2, 14, truncDesc, 1);

    /* Author */
    print(2, 26, `by ${mod.author || 'Unknown'}`, 1);

    /* Status line */
    if (status.installed) {
        if (status.hasUpdate) {
            print(2, 38, `Installed: v${status.installedVersion}`, 1);
        } else {
            print(2, 38, "Installed", 1);
        }
    }

    /* Action buttons */
    let action1, action2;
    if (status.installed) {
        action1 = status.hasUpdate ? 'Update' : 'Reinstall';
        action2 = 'Remove';
    } else {
        action1 = 'Install';
        action2 = null;
    }

    const buttonY = 48;

    if (storePickerActionIndex === 0) {
        fill_rect(2, buttonY, 58, 12, 1);
        print(4, buttonY + 2, `[${action1}]`, 0);
    } else {
        print(4, buttonY + 2, `[${action1}]`, 1);
    }

    if (action2) {
        if (storePickerActionIndex === 1) {
            fill_rect(66, buttonY, 58, 12, 1);
            print(68, buttonY + 2, `[${action2}]`, 0);
        } else {
            print(68, buttonY + 2, `[${action2}]`, 1);
        }
    }
}
```

**Step 2: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker detail view"
```

---

### Task 10: Add store picker to main draw switch

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Find the main draw function's switch statement**

Locate the function that handles drawing based on current view (search for `switch (view)` in the draw section). Add cases for the store picker views:

```javascript
        case VIEWS.STORE_PICKER_LIST:
            drawStorePickerList();
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            drawStorePickerDetail();
            break;
        case VIEWS.STORE_PICKER_LOADING:
            drawStorePickerLoading();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            drawStorePickerResult();
            break;
```

**Step 2: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker views to draw switch"
```

---

### Task 11: Add store picker navigation handlers

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add jog wheel handler for store picker**

Find where jog wheel input is handled and add cases for store picker views:

```javascript
/* Handle jog wheel in store picker list */
function handleStorePickerListJog(delta) {
    storePickerSelectedIndex += delta;
    if (storePickerSelectedIndex < 0) storePickerSelectedIndex = 0;
    if (storePickerSelectedIndex >= storePickerModules.length) {
        storePickerSelectedIndex = storePickerModules.length - 1;
    }
    needsRedraw = true;
}

/* Handle jog wheel in store picker detail */
function handleStorePickerDetailJog(delta) {
    const status = getModuleStatus(storePickerCurrentModule, storeInstalledModules);
    const maxAction = status.installed ? 1 : 0;

    storePickerActionIndex += delta;
    if (storePickerActionIndex < 0) storePickerActionIndex = 0;
    if (storePickerActionIndex > maxAction) storePickerActionIndex = maxAction;
    needsRedraw = true;
}
```

**Step 2: Add select handler for store picker**

```javascript
/* Handle selection in store picker list */
function handleStorePickerListSelect() {
    if (storePickerModules.length === 0) return;

    storePickerCurrentModule = storePickerModules[storePickerSelectedIndex];
    storePickerActionIndex = 0;
    view = VIEWS.STORE_PICKER_DETAIL;
    needsRedraw = true;
}

/* Handle selection in store picker detail */
function handleStorePickerDetailSelect() {
    const mod = storePickerCurrentModule;
    if (!mod) return;

    const status = getModuleStatus(mod, storeInstalledModules);

    view = VIEWS.STORE_PICKER_LOADING;
    needsRedraw = true;

    if (storePickerActionIndex === 0) {
        /* Install/Update/Reinstall */
        storePickerLoadingTitle = 'Installing';
        storePickerLoadingMessage = mod.name;

        const result = installModule(mod, storeHostVersion);

        if (result.success) {
            storeInstalledModules = scanInstalledModules();
            storePickerMessage = `Installed ${mod.name}`;
        } else {
            storePickerMessage = result.error || 'Install failed';
        }
    } else {
        /* Remove */
        storePickerLoadingTitle = 'Removing';
        storePickerLoadingMessage = mod.name;

        const result = removeModule(mod);

        if (result.success) {
            storeInstalledModules = scanInstalledModules();
            storePickerMessage = `Removed ${mod.name}`;
        } else {
            storePickerMessage = result.error || 'Remove failed';
        }
    }

    view = VIEWS.STORE_PICKER_RESULT;
    needsRedraw = true;
}

/* Handle selection in store picker result */
function handleStorePickerResultSelect() {
    /* Return to list */
    view = VIEWS.STORE_PICKER_LIST;
    storePickerCurrentModule = null;
    needsRedraw = true;
}
```

**Step 3: Add back handler for store picker**

```javascript
/* Handle back in store picker */
function handleStorePickerBack() {
    switch (view) {
        case VIEWS.STORE_PICKER_LIST:
            /* Return to component select */
            view = VIEWS.COMPONENT_SELECT;
            storePickerCategory = null;
            storePickerModules = [];
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            /* Return to list */
            view = VIEWS.STORE_PICKER_LIST;
            storePickerCurrentModule = null;
            break;
        case VIEWS.STORE_PICKER_RESULT:
            /* Return to list */
            view = VIEWS.STORE_PICKER_LIST;
            storePickerCurrentModule = null;
            break;
        case VIEWS.STORE_PICKER_LOADING:
            /* Can't cancel during loading */
            break;
    }
    needsRedraw = true;
}
```

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add store picker navigation handlers"
```

---

### Task 12: Wire up store picker input handling

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Find the jog wheel handling switch and add store picker cases**

Locate where jog wheel delta is handled (search for jog handling, likely in a CC handler). Add:

```javascript
        case VIEWS.STORE_PICKER_LIST:
            handleStorePickerListJog(delta);
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            handleStorePickerDetailJog(delta);
            break;
```

**Step 2: Find the select/click handling switch and add store picker cases**

```javascript
        case VIEWS.STORE_PICKER_LIST:
            handleStorePickerListSelect();
            break;
        case VIEWS.STORE_PICKER_DETAIL:
            handleStorePickerDetailSelect();
            break;
        case VIEWS.STORE_PICKER_RESULT:
            handleStorePickerResultSelect();
            break;
```

**Step 3: Find the back button handling switch and add store picker cases**

```javascript
        case VIEWS.STORE_PICKER_LIST:
        case VIEWS.STORE_PICKER_DETAIL:
        case VIEWS.STORE_PICKER_RESULT:
            handleStorePickerBack();
            break;
```

**Step 4: Build and verify**

Run: `cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything && ./scripts/build.sh`
Expected: Build succeeds

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: wire up store picker input handling"
```

---

### Task 13: Test and verify on device

**Files:**
- None (manual testing)

**Step 1: Deploy to device**

Run: `cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything && ./scripts/install.sh local`

**Step 2: Manual test checklist**

1. Load Signal Chain module
2. Enter Shadow UI mode
3. Navigate to a chain slot (e.g., synth)
4. Enter component select
5. Scroll to bottom - verify "[Get more...]" appears
6. Select "[Get more...]" - verify loading screen appears
7. Verify module list appears with correct category
8. Browse list - verify scrolling works
9. Select a module - verify detail view shows
10. If not installed: verify Install button works
11. If installed: verify Update/Remove buttons work
12. Press Back from detail - verify returns to list
13. Press Back from list - verify returns to component select
14. Verify newly installed module appears in component select list

**Step 3: Commit any fixes discovered during testing**

```bash
git add -A
git commit -m "fix: adjustments from device testing"
```

---

### Task 14: Final cleanup and commit

**Files:**
- All modified files

**Step 1: Review all changes**

Run: `git diff --stat HEAD~10`

**Step 2: Verify Module Store still works standalone**

1. Load Module Store from main menu
2. Browse categories
3. Install/remove a module
4. Verify no regressions

**Step 3: Create final commit if needed**

```bash
git add -A
git commit -m "feat: shadow UI store picker complete"
```

**Step 4: Summary**

The implementation adds:
- `src/shared/store_utils.mjs` - shared catalog/install utilities
- Modified `src/modules/store/ui.js` - uses shared utilities
- Modified `src/shadow/shadow_ui.js` - store picker views and navigation

Users can now access the module store from within Shadow UI's module picker, browse modules filtered by category, and install without leaving the chain editor.
