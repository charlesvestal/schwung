/*
 * Module Store UI
 *
 * Browse, install, update, and remove external modules.
 */

import * as std from 'std';

import {
    MidiCC,
    MoveMainKnob, MoveMainButton,
    MoveShift, MoveBack,
    MoveUp, MoveDown
} from '../../shared/constants.mjs';

import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';
import { decodeDelta } from '../../shared/input_filter.mjs';
import {
    drawMenuHeader,
    drawMenuList,
    drawMenuFooter,
    drawStatusOverlay,
    menuLayoutDefaults
} from '../../shared/menu_layout.mjs';

/* Constants */
const CATALOG_URL = 'https://raw.githubusercontent.com/charlesvestal/move-anything/main/module-catalog.json';
const CATALOG_CACHE_PATH = '/data/UserData/move-anything/catalog-cache.json';
const MODULES_DIR = '/data/UserData/move-anything/modules';
const BASE_DIR = '/data/UserData/move-anything';
const TMP_DIR = '/data/UserData/move-anything/tmp';
const HOST_VERSION_FILE = '/data/UserData/move-anything/host/version.txt';
/* UI States */
const STATE_LOADING = 'loading';
const STATE_ERROR = 'error';
const STATE_CATEGORIES = 'categories';
const STATE_MODULE_LIST = 'modules';
const STATE_MODULE_DETAIL = 'detail';
const STATE_INSTALLING = 'installing';
const STATE_REMOVING = 'removing';
const STATE_RESULT = 'result';
const STATE_HOST_UPDATE = 'host_update';
const STATE_UPDATING_HOST = 'updating_host';
const STATE_UPDATE_ALL = 'update_all';
const STATE_UPDATING_ALL = 'updating_all';

/* Categories - host update shown separately at top */
const CATEGORIES = [
    { id: 'sound_generator', name: 'Sound Generators' },
    { id: 'audio_fx', name: 'Audio FX' },
    { id: 'midi_fx', name: 'MIDI FX' },
    { id: 'midi_source', name: 'MIDI Sources' },
    { id: 'utility', name: 'Utilities' }
];

/* State */
let state = STATE_LOADING;
let catalog = null;
let installedModules = {};
let hostVersion = '0.0.0';
let hostUpdateAvailable = false;
let selectedCategoryIndex = 0;
let selectedModuleIndex = 0;
let selectedActionIndex = 0;
let selectedUpdateIndex = 0;
let currentCategory = null;
let currentModule = null;
let cameFromUpdateAll = false;
let errorMessage = '';
let resultMessage = '';
let shiftHeld = false;
let loadingTitle = 'Module Store';
let loadingMessage = 'Loading...';

/* CC constants */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_SHIFT = MoveShift;
const CC_BACK = MoveBack;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;

/* Compare semver versions: returns 1 if a > b, -1 if a < b, 0 if equal */
function compareVersions(a, b) {
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
function isNewerVersion(a, b) {
    return compareVersions(a, b) > 0;
}

/* Fetch release info from release.json in repo (high rate limits via raw.githubusercontent.com) */
function fetchReleaseJson(github_repo) {
    const cacheFile = `${TMP_DIR}/${github_repo.replace('/', '_')}_release.json`;
    const releaseUrl = `https://raw.githubusercontent.com/${github_repo}/main/release.json`;

    /* Download release.json */
    const success = host_http_download(releaseUrl, cacheFile);
    if (!success) {
        console.log(`Failed to fetch release.json for ${github_repo}`);
        return null;
    }

    try {
        const jsonStr = std.loadFile(cacheFile);
        if (!jsonStr) return null;

        const release = JSON.parse(jsonStr);

        /* release.json format: { "version": "x.y.z", "download_url": "..." } */
        if (!release.version || !release.download_url) {
            console.log(`Invalid release.json format for ${github_repo}`);
            return null;
        }

        return { version: release.version, download_url: release.download_url };
    } catch (e) {
        console.log(`Failed to parse release.json for ${github_repo}: ${e}`);
        return null;
    }
}

/* Fetch release info for all modules in catalog */
function fetchAllReleaseInfo() {
    if (!catalog) return;

    /* Fetch host release info */
    if (catalog.host && catalog.host.github_repo) {
        loadingTitle = 'Loading Catalog';
        loadingMessage = 'Checking host...';
        draw();
        host_flush_display();

        const hostRelease = fetchReleaseJson(catalog.host.github_repo);
        if (hostRelease) {
            catalog.host.latest_version = hostRelease.version;
            catalog.host.download_url = hostRelease.download_url;
            console.log(`Host latest: ${hostRelease.version}`);
        }
    }

    /* Fetch module release info */
    if (catalog.modules) {
        for (let i = 0; i < catalog.modules.length; i++) {
            const mod = catalog.modules[i];
            if (mod.github_repo) {
                loadingTitle = 'Loading Catalog';
                loadingMessage = mod.name;
                draw();
                host_flush_display();

                const release = fetchReleaseJson(mod.github_repo);
                if (release) {
                    mod.latest_version = release.version;
                    mod.download_url = release.download_url;
                    console.log(`${mod.id} latest: ${release.version}`);
                } else {
                    /* Fallback: module has no release.json yet */
                    mod.latest_version = '0.0.0';
                    mod.download_url = null;
                }
            }
        }
    }
}

/* Get current host version */
function getHostVersion() {
    try {
        const versionStr = std.loadFile(HOST_VERSION_FILE);
        if (versionStr) {
            hostVersion = versionStr.trim();
            return;
        }
    } catch (e) {
        /* Fall through */
    }
    /* Default if no version file */
    hostVersion = '1.0.0';
}

/* Check if host update is available */
function checkHostUpdate() {
    if (!catalog || !catalog.host) {
        hostUpdateAvailable = false;
        return;
    }
    hostUpdateAvailable = isNewerVersion(catalog.host.latest_version, hostVersion);
}

/* Update the host */
function updateHost() {
    if (!catalog || !catalog.host) {
        state = STATE_RESULT;
        resultMessage = 'No host info';
        return;
    }

    if (!catalog.host.download_url) {
        state = STATE_RESULT;
        resultMessage = 'No release available';
        return;
    }

    state = STATE_UPDATING_HOST;
    loadingTitle = 'Updating Host';
    loadingMessage = `v${catalog.host.latest_version}`;

    const tarPath = `${TMP_DIR}/move-anything.tar.gz`;

    /* Download the host tarball */
    const downloadOk = host_http_download(catalog.host.download_url, tarPath);
    if (!downloadOk) {
        state = STATE_RESULT;
        resultMessage = 'Download failed';
        return;
    }

    /* Extract over existing installation - strip move-anything/ prefix from tarball */
    const extractOk = host_extract_tar_strip(tarPath, BASE_DIR, 1);
    if (!extractOk) {
        state = STATE_RESULT;
        resultMessage = 'Extract failed';
        return;
    }

    state = STATE_RESULT;
    resultMessage = 'Updated! Restart to apply';
}

/* Scan installed modules */
function scanInstalledModules() {
    installedModules = {};
    const modules = host_list_modules();
    for (const mod of modules) {
        installedModules[mod.id] = mod.version;
    }
}

/* Get modules for current category */
function getModulesForCategory(categoryId) {
    if (!catalog || !catalog.modules) return [];
    return catalog.modules.filter(m => m.component_type === categoryId);
}

/* Get module install status */
function getModuleStatus(mod) {
    const installedVersion = installedModules[mod.id];
    if (!installedVersion) {
        return { installed: false, hasUpdate: false };
    }
    const hasUpdate = isNewerVersion(mod.latest_version, installedVersion);
    return { installed: true, hasUpdate };
}

/* Get all modules that have updates available */
function getModulesWithUpdates() {
    if (!catalog || !catalog.modules) return [];
    return catalog.modules.filter(mod => {
        const status = getModuleStatus(mod);
        return status.installed && status.hasUpdate;
    });
}

/* Update all modules that have updates */
function updateAllModules() {
    const modulesToUpdate = getModulesWithUpdates();
    if (modulesToUpdate.length === 0) {
        state = STATE_RESULT;
        resultMessage = 'No updates available';
        return;
    }

    state = STATE_UPDATING_ALL;
    let successCount = 0;
    let failCount = 0;

    for (let i = 0; i < modulesToUpdate.length; i++) {
        const mod = modulesToUpdate[i];
        loadingTitle = `Updating ${i + 1}/${modulesToUpdate.length}`;
        loadingMessage = mod.name;
        draw();
        host_flush_display();

        /* Check if module has a download URL */
        if (!mod.download_url) {
            failCount++;
            continue;
        }

        const tarPath = `${TMP_DIR}/${mod.id}-module.tar.gz`;

        /* Download the module tarball */
        const downloadOk = host_http_download(mod.download_url, tarPath);
        if (!downloadOk) {
            failCount++;
            continue;
        }

        /* Extract to modules directory */
        const extractOk = host_extract_tar(tarPath, MODULES_DIR);
        if (!extractOk) {
            failCount++;
            continue;
        }

        successCount++;
    }

    /* Rescan modules */
    host_rescan_modules();
    scanInstalledModules();

    state = STATE_RESULT;
    if (failCount === 0) {
        resultMessage = `Updated ${successCount} module${successCount !== 1 ? 's' : ''}`;
    } else {
        resultMessage = `Updated ${successCount}, failed ${failCount}`;
    }
}

/* Count modules by category */
function getCategoryCount(categoryId) {
    return getModulesForCategory(categoryId).length;
}

/* Fetch catalog from network */
function fetchCatalog() {
    state = STATE_LOADING;
    loadingTitle = 'Loading Catalog';
    loadingMessage = 'Fetching...';

    /* Force display update before blocking download */
    draw();
    host_flush_display();

    /* Try to download fresh catalog (GitHub CDN caches ~5 min) */
    const success = host_http_download(CATALOG_URL, CATALOG_CACHE_PATH);

    if (success) {
        loadCatalogFromCache();
    } else {
        /* Try to use cached version */
        if (host_file_exists(CATALOG_CACHE_PATH)) {
            loadCatalogFromCache();
            console.log('Using cached catalog (network unavailable)');
        } else {
            state = STATE_ERROR;
            errorMessage = 'Could not fetch catalog';
        }
    }
}

/* Load catalog from cache file */
function loadCatalogFromCache() {
    try {
        const jsonStr = std.loadFile(CATALOG_CACHE_PATH);
        if (!jsonStr) {
            console.log('No cached catalog found');
            state = STATE_ERROR;
            errorMessage = 'No catalog available';
            return;
        }

        catalog = JSON.parse(jsonStr);
        console.log(`Loaded catalog with ${catalog.modules ? catalog.modules.length : 0} modules`);

        /* For catalog v2+, fetch release info from release.json files */
        if (catalog.catalog_version >= 2) {
            fetchAllReleaseInfo();
        }

        checkHostUpdate();
        state = STATE_CATEGORIES;
        if (hostUpdateAvailable) {
            console.log(`Host update available: ${hostVersion} -> ${catalog.host.latest_version}`);
        }
    } catch (e) {
        console.log('Failed to parse catalog: ' + e);
        state = STATE_ERROR;
        errorMessage = 'Invalid catalog format';
    }
}

/* Install a module */
function installModule(mod) {
    /* Check host version compatibility */
    if (mod.min_host_version && compareVersions(mod.min_host_version, hostVersion) > 0) {
        state = STATE_RESULT;
        resultMessage = `Requires host v${mod.min_host_version}`;
        return;
    }

    /* Check if module has a download URL */
    if (!mod.download_url) {
        state = STATE_RESULT;
        resultMessage = 'No release available';
        return;
    }

    state = STATE_INSTALLING;
    loadingTitle = 'Downloading';
    loadingMessage = `${mod.name} v${mod.latest_version}`;

    /* Force display update before blocking download */
    draw();
    host_flush_display();

    const tarPath = `${TMP_DIR}/${mod.id}-module.tar.gz`;

    /* Download the module tarball */
    console.log(`Downloading: ${mod.download_url}`);
    const downloadOk = host_http_download(mod.download_url, tarPath);
    if (!downloadOk) {
        state = STATE_RESULT;
        resultMessage = 'Download failed (404?)';
        console.log('Download failed');
        return;
    }

    loadingTitle = 'Installing';
    loadingMessage = `${mod.name} v${mod.latest_version}`;
    draw();
    host_flush_display();

    /* Extract to modules directory */
    const extractOk = host_extract_tar(tarPath, MODULES_DIR);
    if (!extractOk) {
        state = STATE_RESULT;
        resultMessage = 'Extract failed';
        return;
    }

    /* Rescan modules */
    host_rescan_modules();
    scanInstalledModules();

    state = STATE_RESULT;
    resultMessage = `Installed ${mod.name}`;
}

/* Remove a module */
function removeModule(mod) {
    state = STATE_REMOVING;
    loadingTitle = 'Removing';
    loadingMessage = mod.name;

    /* Force display update before blocking operation */
    draw();
    host_flush_display();

    const modulePath = `${MODULES_DIR}/${mod.id}`;

    /* Remove the module directory */
    const removeOk = host_remove_dir(modulePath);
    if (!removeOk) {
        state = STATE_RESULT;
        resultMessage = 'Remove failed';
        return;
    }

    /* Rescan modules */
    host_rescan_modules();
    scanInstalledModules();

    state = STATE_RESULT;
    resultMessage = `Removed ${mod.name}`;
}

/* Get total items in categories view (includes host update and update all if available) */
function getCategoryItemCount() {
    let count = CATEGORIES.length;
    if (hostUpdateAvailable) count++;
    if (getModulesWithUpdates().length > 0) count++;
    return count;
}

/* Handle navigation */
function handleJogWheel(delta) {
    switch (state) {
        case STATE_CATEGORIES: {
            const maxIndex = getCategoryItemCount() - 1;
            selectedCategoryIndex += delta;
            if (selectedCategoryIndex < 0) selectedCategoryIndex = 0;
            if (selectedCategoryIndex > maxIndex) selectedCategoryIndex = maxIndex;
            break;
        }

        case STATE_HOST_UPDATE:
            selectedActionIndex += delta;
            if (selectedActionIndex < 0) selectedActionIndex = 0;
            if (selectedActionIndex > 0) selectedActionIndex = 0;  /* Only one action */
            break;

        case STATE_UPDATE_ALL: {
            /* List includes all modules with updates + "Update All" at the end */
            const updateCount = getModulesWithUpdates().length;
            const maxIndex = updateCount;  /* modules + Update All button */
            selectedUpdateIndex += delta;
            if (selectedUpdateIndex < 0) selectedUpdateIndex = 0;
            if (selectedUpdateIndex > maxIndex) selectedUpdateIndex = maxIndex;
            break;
        }

        case STATE_MODULE_LIST: {
            const modules = getModulesForCategory(currentCategory.id);
            selectedModuleIndex += delta;
            if (selectedModuleIndex < 0) selectedModuleIndex = 0;
            if (selectedModuleIndex >= modules.length) {
                selectedModuleIndex = modules.length - 1;
            }
            break;
        }

        case STATE_MODULE_DETAIL:
            selectedActionIndex += delta;
            if (selectedActionIndex < 0) selectedActionIndex = 0;
            if (selectedActionIndex > 1) selectedActionIndex = 1;
            break;
    }
}

/* Handle selection */
function handleSelect() {
    switch (state) {
        case STATE_CATEGORIES: {
            /* Calculate which item is selected based on what's visible */
            let adjustedIndex = selectedCategoryIndex;
            const updatesAvailable = getModulesWithUpdates().length > 0;

            /* Index 0: Update Host (if available) */
            if (hostUpdateAvailable) {
                if (adjustedIndex === 0) {
                    selectedActionIndex = 0;
                    state = STATE_HOST_UPDATE;
                    break;
                }
                adjustedIndex--;
            }

            /* Next: Update All (if updates available) */
            if (updatesAvailable) {
                if (adjustedIndex === 0) {
                    selectedUpdateIndex = 0;
                    state = STATE_UPDATE_ALL;
                    break;
                }
                adjustedIndex--;
            }

            /* Remaining: Categories */
            currentCategory = CATEGORIES[adjustedIndex];
            selectedModuleIndex = 0;
            state = STATE_MODULE_LIST;
            break;
        }

        case STATE_HOST_UPDATE:
            updateHost();
            break;

        case STATE_UPDATE_ALL: {
            const modulesToUpdate = getModulesWithUpdates();
            if (selectedUpdateIndex < modulesToUpdate.length) {
                /* Selected a specific module - show its detail */
                currentModule = modulesToUpdate[selectedUpdateIndex];
                selectedActionIndex = 0;
                cameFromUpdateAll = true;
                state = STATE_MODULE_DETAIL;
            } else {
                /* Selected "Update All" */
                updateAllModules();
            }
            break;
        }

        case STATE_MODULE_LIST: {
            const modules = getModulesForCategory(currentCategory.id);
            if (modules.length > 0) {
                currentModule = modules[selectedModuleIndex];
                selectedActionIndex = 0;
                state = STATE_MODULE_DETAIL;
            }
            break;
        }

        case STATE_MODULE_DETAIL: {
            const status = getModuleStatus(currentModule);
            if (status.installed) {
                if (selectedActionIndex === 0) {
                    installModule(currentModule);  /* Reinstall */
                } else {
                    removeModule(currentModule);
                }
            } else {
                installModule(currentModule);
            }
            break;
        }

        case STATE_ERROR:
        case STATE_RESULT:
            if (currentModule) {
                state = STATE_MODULE_LIST;
            } else {
                state = STATE_CATEGORIES;
            }
            break;
    }
}

/* Handle back */
function handleBack() {
    switch (state) {
        case STATE_CATEGORIES:
            host_return_to_menu();
            break;

        case STATE_HOST_UPDATE:
        case STATE_UPDATE_ALL:
            state = STATE_CATEGORIES;
            break;

        case STATE_MODULE_LIST:
            currentCategory = null;
            state = STATE_CATEGORIES;
            break;

        case STATE_MODULE_DETAIL:
            currentModule = null;
            if (cameFromUpdateAll) {
                cameFromUpdateAll = false;
                state = STATE_UPDATE_ALL;
            } else {
                state = STATE_MODULE_LIST;
            }
            break;

        case STATE_ERROR:
        case STATE_RESULT:
            if (currentModule) {
                state = STATE_MODULE_DETAIL;
            } else if (currentCategory) {
                state = STATE_MODULE_LIST;
            } else {
                state = STATE_CATEGORIES;
            }
            break;
    }
}

/* Draw loading overlay */
function drawLoading() {
    clear_screen();
    drawStatusOverlay(loadingTitle, loadingMessage);
}

/* Draw error screen */
function drawError() {
    clear_screen();
    drawMenuHeader('Module Store', 'Error');
    print(2, 25, errorMessage, 1);
    drawMenuFooter('Press to continue');
}

/* Draw result screen */
function drawResult() {
    clear_screen();
    drawMenuHeader('Module Store');
    print(2, 25, resultMessage, 1);
    drawMenuFooter('Press to continue');
}

/* Draw categories screen */
function drawCategories() {
    clear_screen();
    drawMenuHeader('Module Store');

    /* Build items list - host update first if available */
    let items = [];
    if (hostUpdateAvailable) {
        items.push({
            id: '_host_update',
            name: 'Update Host',
            value: `${hostVersion} -> ${catalog.host.latest_version}`
        });
    }

    /* Update All option if modules have updates */
    const modulesWithUpdates = getModulesWithUpdates();
    if (modulesWithUpdates.length > 0) {
        items.push({
            id: '_update_all',
            name: 'Update All',
            value: `(${modulesWithUpdates.length})`
        });
    }

    /* Add categories */
    for (const cat of CATEGORIES) {
        items.push({
            ...cat,
            value: `(${getCategoryCount(cat.id)})`
        });
    }

    drawMenuList({
        items,
        selectedIndex: selectedCategoryIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name,
        getValue: (item) => item.value
    });

    drawMenuFooter('Back:exit  Jog:browse');
}

/* Draw host update confirmation screen */
function drawHostUpdate() {
    clear_screen();
    drawMenuHeader('Update Host');

    print(2, 16, `Current: v${hostVersion}`, 1);
    print(2, 28, `New: v${catalog.host.latest_version}`, 1);

    /* Divider */
    fill_rect(0, 38, 128, 1, 1);

    /* Update button */
    const y = 44;
    fill_rect(2, y - 1, 70, 12, 1);
    print(4, y, '[Update Now]', 0);

    drawMenuFooter('Back:cancel');
}

/* Draw update all screen - scrollable list of modules + Update All action */
function drawUpdateAll() {
    clear_screen();
    const modulesToUpdate = getModulesWithUpdates();
    drawMenuHeader('Updates Available', `(${modulesToUpdate.length})`);

    /* Build items list - modules then "Update All" */
    let items = [];
    for (const mod of modulesToUpdate) {
        items.push({
            id: mod.id,
            name: mod.name
        });
    }
    /* Add "Update All" as last item */
    items.push({
        id: '_update_all',
        name: '>> Update All <<'
    });

    drawMenuList({
        items,
        selectedIndex: selectedUpdateIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name,
        getValue: (item) => item.value
    });

    drawMenuFooter('Back:cancel  Jog:select');
}

/* Draw module list screen */
function drawModuleList() {
    clear_screen();
    drawMenuHeader(currentCategory.name);

    const modules = getModulesForCategory(currentCategory.id);

    if (modules.length === 0) {
        print(2, 30, 'No modules available', 1);
        drawMenuFooter('Back:categories');
        return;
    }

    const items = modules.map(mod => {
        const status = getModuleStatus(mod);
        let statusIcon = '';
        if (status.installed) {
            statusIcon = status.hasUpdate ? '^' : '*';  /* ^ = update available, * = installed */
        }
        return { ...mod, statusIcon };
    });

    drawMenuList({
        items,
        selectedIndex: selectedModuleIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name,
        getValue: (item) => item.statusIcon
    });

    drawMenuFooter('Back:categories');
}

/* Draw module detail screen */
function drawModuleDetail() {
    clear_screen();
    const status = getModuleStatus(currentModule);

    /* Show version transition if update available, otherwise just new version */
    let versionStr;
    let title = currentModule.name;
    if (status.installed && status.hasUpdate) {
        const installedVer = installedModules[currentModule.id] || '?';
        versionStr = `${installedVer}->${currentModule.latest_version}`;
        /* Truncate title to fit with version transition (max ~8 chars) */
        if (title.length > 8) {
            title = title.substring(0, 7) + '~';
        }
    } else {
        versionStr = `v${currentModule.latest_version}`;
        /* More room without transition */
        if (title.length > 14) {
            title = title.substring(0, 13) + '~';
        }
    }
    drawMenuHeader(title, versionStr);

    /* Description */
    const desc = currentModule.description || '';
    const truncDesc = desc.length > 20 ? desc.substring(0, 17) + '...' : desc;
    print(2, 16, truncDesc, 1);

    /* Author */
    print(2, 28, `by ${currentModule.author || 'Unknown'}`, 1);

    /* Divider */
    fill_rect(0, 38, 128, 1, 1);

    /* Action buttons */
    let action1, action2;

    if (status.installed) {
        action1 = status.hasUpdate ? 'Update' : 'Reinstall';
        action2 = 'Remove';
    } else {
        action1 = 'Install';
        action2 = null;
    }

    const y = 44;
    if (selectedActionIndex === 0) {
        fill_rect(2, y - 1, 60, 12, 1);
        print(4, y, `[${action1}]`, 0);
    } else {
        print(4, y, `[${action1}]`, 1);
    }

    if (action2) {
        if (selectedActionIndex === 1) {
            fill_rect(66, y - 1, 58, 12, 1);
            print(68, y, `[${action2}]`, 0);
        } else {
            print(68, y, `[${action2}]`, 1);
        }
    }
}

/* Main draw function */
function draw() {
    switch (state) {
        case STATE_LOADING:
        case STATE_INSTALLING:
        case STATE_REMOVING:
        case STATE_UPDATING_HOST:
        case STATE_UPDATING_ALL:
            drawLoading();
            break;
        case STATE_ERROR:
            drawError();
            break;
        case STATE_RESULT:
            drawResult();
            break;
        case STATE_CATEGORIES:
            drawCategories();
            break;
        case STATE_HOST_UPDATE:
            drawHostUpdate();
            break;
        case STATE_UPDATE_ALL:
            drawUpdateAll();
            break;
        case STATE_MODULE_LIST:
            drawModuleList();
            break;
        case STATE_MODULE_DETAIL:
            drawModuleDetail();
            break;
    }
}

/* Handle CC input */
function handleCC(cc, value) {
    /* Track shift */
    if (cc === CC_SHIFT) {
        shiftHeld = (value > 0);
        return;
    }

    /* Back button */
    if (cc === CC_BACK && value > 0) {
        handleBack();
        return;
    }

    /* Only handle on press (value > 0) or for jog wheel */
    if (cc === CC_JOG_WHEEL) {
        const delta = decodeDelta(value);
        handleJogWheel(delta);
        return;
    }

    if (value === 0) return;

    /* Jog click or up/down arrows for selection */
    if (cc === CC_JOG_CLICK) {
        handleSelect();
        return;
    }

    /* Arrow keys for navigation */
    if (cc === CC_UP) {
        handleJogWheel(-1);
        return;
    }
    if (cc === CC_DOWN) {
        handleJogWheel(1);
        return;
    }
}

/* MIDI handlers */
globalThis.onMidiMessageExternal = function(data) {
    /* Ignore external MIDI */
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const isCC = (data[0] & 0xF0) === 0xB0;
    if (isCC) {
        handleCC(data[1], data[2]);
    }
};

/* Init */
globalThis.init = function() {
    console.log('Module Store starting...');

    /* Show loading immediately */
    state = STATE_LOADING;
    loadingTitle = 'Module Store';
    loadingMessage = 'Loading...';
    draw();
    host_flush_display();

    /* Get current host version */
    getHostVersion();

    /* Scan what's installed */
    scanInstalledModules();

    /* Fetch catalog */
    fetchCatalog();
};

/* Tick */
globalThis.tick = function() {
    draw();
};
