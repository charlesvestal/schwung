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
