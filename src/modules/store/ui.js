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

/* Categories - host update shown separately at top */
const CATEGORIES = [
    { id: 'sound_generator', name: 'Sound Generators' },
    { id: 'audio_fx', name: 'Audio FX' },
    { id: 'midi_fx', name: 'MIDI FX' },
    { id: 'midi_source', name: 'MIDI Sources' }
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
let currentCategory = null;
let currentModule = null;
let errorMessage = '';
let resultMessage = '';
let shiftHeld = false;
let loadingMessage = 'Fetching catalog...';

/* CC constants */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_JOG_CLICK = MoveMainButton;
const CC_SHIFT = MoveShift;
const CC_BACK = MoveBack;
const CC_UP = MoveUp;
const CC_DOWN = MoveDown;

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
    hostUpdateAvailable = catalog.host.latest_version !== hostVersion;
}

/* Update the host */
function updateHost() {
    if (!catalog || !catalog.host) {
        state = STATE_RESULT;
        resultMessage = 'No host info';
        return;
    }

    state = STATE_UPDATING_HOST;
    loadingMessage = 'Updating host...';

    const tarPath = `${TMP_DIR}/move-anything.tar.gz`;

    /* Download the host tarball */
    const downloadOk = host_http_download(catalog.host.download_url, tarPath);
    if (!downloadOk) {
        state = STATE_RESULT;
        resultMessage = 'Download failed';
        return;
    }

    /* Extract over existing installation */
    const extractOk = host_extract_tar(tarPath, BASE_DIR);
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
    const hasUpdate = installedVersion !== mod.latest_version;
    return { installed: true, hasUpdate };
}

/* Count modules by category */
function getCategoryCount(categoryId) {
    return getModulesForCategory(categoryId).length;
}

/* Fetch catalog from network */
function fetchCatalog() {
    state = STATE_LOADING;
    loadingMessage = 'Fetching catalog...';

    /* Try to download fresh catalog */
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
        checkHostUpdate();
        state = STATE_CATEGORIES;
        console.log(`Loaded catalog with ${catalog.modules ? catalog.modules.length : 0} modules`);
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
    state = STATE_INSTALLING;
    loadingMessage = `Installing ${mod.name}...`;

    const tarPath = `${TMP_DIR}/${mod.id}-module.tar.gz`;

    /* Download the module tarball */
    const downloadOk = host_http_download(mod.download_url, tarPath);
    if (!downloadOk) {
        state = STATE_RESULT;
        resultMessage = 'Download failed';
        return;
    }

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
    loadingMessage = `Removing ${mod.name}...`;

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

/* Get total items in categories view (includes host update if available) */
function getCategoryItemCount() {
    return CATEGORIES.length + (hostUpdateAvailable ? 1 : 0);
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
        case STATE_CATEGORIES:
            /* If host update available, index 0 is "Update Host" */
            if (hostUpdateAvailable) {
                if (selectedCategoryIndex === 0) {
                    selectedActionIndex = 0;
                    state = STATE_HOST_UPDATE;
                    break;
                }
                /* Adjust index for categories */
                currentCategory = CATEGORIES[selectedCategoryIndex - 1];
            } else {
                currentCategory = CATEGORIES[selectedCategoryIndex];
            }
            selectedModuleIndex = 0;
            state = STATE_MODULE_LIST;
            break;

        case STATE_HOST_UPDATE:
            updateHost();
            break;

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
            state = STATE_CATEGORIES;
            break;

        case STATE_MODULE_LIST:
            currentCategory = null;
            state = STATE_CATEGORIES;
            break;

        case STATE_MODULE_DETAIL:
            currentModule = null;
            state = STATE_MODULE_LIST;
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

/* Draw loading screen */
function drawLoading() {
    clear_screen();
    drawMenuHeader('Module Store');
    print(2, 30, loadingMessage, 1);
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
            statusIcon = status.hasUpdate ? '*' : '\u2713';  /* checkmark */
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
    drawMenuHeader(currentModule.name, `v${currentModule.latest_version}`);

    /* Description */
    const desc = currentModule.description || '';
    const truncDesc = desc.length > 20 ? desc.substring(0, 17) + '...' : desc;
    print(2, 16, truncDesc, 1);

    /* Author */
    print(2, 28, `by ${currentModule.author || 'Unknown'}`, 1);

    /* Divider */
    fill_rect(0, 38, 128, 1, 1);

    /* Action buttons */
    const status = getModuleStatus(currentModule);
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

    drawMenuFooter('Back:list');
}

/* Main draw function */
function draw() {
    switch (state) {
        case STATE_LOADING:
        case STATE_INSTALLING:
        case STATE_REMOVING:
        case STATE_UPDATING_HOST:
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
