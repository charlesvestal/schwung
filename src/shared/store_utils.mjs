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
    { id: 'utility', name: 'Utilities' },
    { id: 'overtake', name: 'Overtake Modules' }
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
        case 'overtake': return 'overtake';
        default: return 'other';
    }
}

/* Ensure tmp directory exists for downloads */
function ensureTmpDir() {
    if (globalThis.host_ensure_dir) {
        globalThis.host_ensure_dir(TMP_DIR);
    }
}

/* Fetch release info from release.json in repo
 * For multi-module repos, pass module_id to get that specific module's info.
 * Supports two formats:
 *   - Single module: { version, download_url, ... }
 *   - Multi-module:  { modules: { module_id: { version, download_url, ... }, ... } }
 */
export function fetchReleaseJson(github_repo, module_id, branch) {
    ensureTmpDir();
    const cacheFile = `${TMP_DIR}/${github_repo.replace('/', '_')}_release.json`;
    const releaseUrl = `https://raw.githubusercontent.com/${github_repo}/${branch || 'main'}/release.json`;

    const success = globalThis.host_http_download(releaseUrl, cacheFile);
    if (!success) {
        console.log(`Failed to fetch release.json for ${github_repo}`);
        return null;
    }

    try {
        const jsonStr = std.loadFile(cacheFile);
        if (!jsonStr) return null;

        const release = JSON.parse(jsonStr);

        /* Check for multi-module format */
        if (release.modules && module_id && release.modules[module_id]) {
            const modRelease = release.modules[module_id];
            if (!modRelease.version || !modRelease.download_url) {
                console.log(`Invalid module entry for ${module_id} in ${github_repo}`);
                return null;
            }
            return {
                version: modRelease.version,
                download_url: modRelease.download_url,
                install_path: modRelease.install_path || '',
                name: modRelease.name || '',
                description: modRelease.description || '',
                requires: modRelease.requires || '',
                post_install: modRelease.post_install || '',
                repo_url: modRelease.repo_url || ''
            };
        }

        /* Single module format (backwards compatible) */
        if (!release.version || !release.download_url) {
            console.log(`Invalid release.json format for ${github_repo}`);
            return null;
        }

        return {
            version: release.version,
            download_url: release.download_url,
            install_path: release.install_path || '',
            /* New fields for module metadata */
            name: release.name || '',
            description: release.description || '',
            requires: release.requires || '',
            post_install: release.post_install || '',
            repo_url: release.repo_url || ''
        };
    } catch (e) {
        console.log(`Failed to parse release.json for ${github_repo}: ${e}`);
        return null;
    }
}

/* Quick fetch of release.json for core version check.
 * Returns { version, download_url } or null on failure. */
export function fetchReleaseJsonQuick(github_repo) {
    const cacheFile = `${TMP_DIR}/${github_repo.replace('/', '_')}_release_quick.json`;
    const releaseUrl = `https://raw.githubusercontent.com/${github_repo}/main/release.json`;

    const success = globalThis.host_http_download(releaseUrl, cacheFile);
    if (!success) return null;

    try {
        const jsonStr = std.loadFile(cacheFile);
        if (!jsonStr) return null;
        const release = JSON.parse(jsonStr);
        if (!release.version || !release.download_url) return null;
        return { version: release.version, download_url: release.download_url };
    } catch (e) {
        return null;
    }
}

/* Fetch release notes from GitHub Atom feed, returns text or null */
export function fetchReleaseNotes(githubRepo) {
    const cacheFile = `${TMP_DIR}/${githubRepo.replace('/', '_')}_releases.atom`;
    const atomUrl = `https://github.com/${githubRepo}/releases.atom`;
    const success = globalThis.host_http_download(atomUrl, cacheFile);
    if (!success) return null;
    try {
        const raw = std.loadFile(cacheFile);
        if (!raw) return null;
        /* Extract content from first <entry> */
        const match = raw.match(/<entry>[\s\S]*?<content[^>]*>([\s\S]*?)<\/content>/);
        if (!match) return null;
        /* Unescape HTML entities and strip tags */
        let html = match[1]
            .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
            .replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'");
        /* Strip HTML tags, convert <li> to "- " bullets */
        let text = html
            .replace(/<li>/gi, '- ')
            .replace(/<br\s*\/?>/gi, '\n')
            .replace(/<\/p>/gi, '\n')
            .replace(/<[^>]+>/g, '')
            .replace(/\n{3,}/g, '\n\n')
            .trim();
        return text || null;
    } catch (e) {
        return null;
    }
}

/* Fetch catalog from network, returns { success, catalog, error } */
export function fetchCatalog(onProgress) {
    if (onProgress) onProgress('Loading Catalog', 'Fetching...', 0, 0);

    let success = false;
    let networkAvailable = false;
    try {
        const fn = globalThis.host_http_download;
        if (!fn) {
            return { success: false, catalog: null, error: 'host_http_download not available' };
        }
        success = fn(CATALOG_URL, CATALOG_CACHE_PATH);
        networkAvailable = success;
    } catch (e) {
        return { success: false, catalog: null, error: 'Download error: ' + String(e) };
    }

    if (success) {
        return loadCatalogFromCache(onProgress, networkAvailable);
    } else {
        /* Try cached version */
        if (globalThis.host_file_exists(CATALOG_CACHE_PATH)) {
            console.log('Using cached catalog (network unavailable)');
            return loadCatalogFromCache(onProgress, false);
        } else {
            return { success: false, catalog: null, error: 'No internet connection' };
        }
    }
}

/* Load catalog from cache file, returns { success, catalog, error }
 * networkAvailable: if false, skip all release.json fetches (use cached data only) */
export function loadCatalogFromCache(onProgress, networkAvailable) {
    try {
        const jsonStr = std.loadFile(CATALOG_CACHE_PATH);
        if (!jsonStr) {
            return { success: false, catalog: null, error: 'No catalog available' };
        }

        const catalog = JSON.parse(jsonStr);
        const moduleCount = catalog.modules ? catalog.modules.length : 0;
        console.log(`Loaded catalog with ${moduleCount} modules (network: ${networkAvailable ? 'yes' : 'no'})`);

        /* For catalog v2+, fetch release info — but skip if network is down */
        if (catalog.catalog_version >= 2 && catalog.modules && networkAvailable) {
            let consecutiveFailures = 0;
            for (let i = 0; i < catalog.modules.length; i++) {
                const mod = catalog.modules[i];
                if (mod.github_repo) {
                    if (consecutiveFailures >= 2) {
                        /* Network likely down — skip remaining fetches */
                        mod.latest_version = mod.latest_version || '?';
                        mod.download_url = mod.download_url || null;
                        continue;
                    }
                    if (onProgress) onProgress('Loading Catalog', mod.name, i + 1, moduleCount);

                    /* Pass module id for multi-module repo support */
                    const release = fetchReleaseJson(mod.github_repo, mod.id, mod.default_branch);
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
                        consecutiveFailures = 0;
                    } else {
                        mod.latest_version = mod.latest_version || '?';
                        if (mod.github_repo && mod.asset_name) {
                            mod.download_url = `https://github.com/${mod.github_repo}/releases/latest/download/${mod.asset_name}`;
                        } else {
                            mod.download_url = null;
                        }
                        consecutiveFailures++;
                        if (consecutiveFailures >= 2) {
                            console.log('Two consecutive release fetch failures, skipping remaining modules');
                        }
                    }
                }
            }
        } else if (catalog.catalog_version >= 2 && catalog.modules && !networkAvailable) {
            /* Network down — construct download URLs from catalog fields as fallback */
            for (let i = 0; i < catalog.modules.length; i++) {
                const mod = catalog.modules[i];
                mod.latest_version = mod.latest_version || '?';
                if (!mod.download_url && mod.github_repo && mod.asset_name) {
                    mod.download_url = `https://github.com/${mod.github_repo}/releases/latest/download/${mod.asset_name}`;
                }
            }
        }

        /* Also fetch host release.json — only if network available */
        if (catalog.catalog_version >= 2 && catalog.host && catalog.host.github_repo && networkAvailable) {
            if (onProgress) onProgress('Loading Catalog', 'Core', moduleCount, moduleCount);
            const hostRelease = fetchReleaseJson(catalog.host.github_repo);
            if (hostRelease) {
                catalog.host.latest_version = hostRelease.version;
                catalog.host.download_url = hostRelease.download_url;
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

/* Validate a module ID contains no path traversal */
function isValidModuleId(id) {
    if (!id || typeof id !== 'string') return false;
    if (id.includes('..') || id.includes('/') || id.includes('\\')) return false;
    return true;
}

/* Install a module, returns { success, error } */
export function installModule(mod, hostVersion, onProgress) {
    if (!isValidModuleId(mod.id)) {
        return { success: false, error: 'Invalid module ID' };
    }
    console.log(`Installing module: ${mod.id}`);

    /* Check host version compatibility */
    if (mod.min_host_version && compareVersions(mod.min_host_version, hostVersion) > 0) {
        return { success: false, error: `Requires host v${mod.min_host_version}` };
    }

    /* Check if module has a download URL */
    if (!mod.download_url) {
        return { success: false, error: 'No release available' };
    }

    ensureTmpDir();
    const tarPath = `${TMP_DIR}/${mod.id}-module.tar.gz`;

    /* Download the module tarball */
    if (onProgress) onProgress('Downloading', mod.name);
    const downloadOk = globalThis.host_http_download(mod.download_url, tarPath);
    if (!downloadOk) {
        return { success: false, error: 'Download failed' };
    }

    /* Determine extraction path based on component_type */
    const subdir = getInstallSubdir(mod.component_type);
    const extractDir = subdir ? `${MODULES_DIR}/${subdir}` : MODULES_DIR;

    /* Ensure extraction directory exists */
    if (globalThis.host_ensure_dir) {
        globalThis.host_ensure_dir(extractDir);
    }

    /* Extract to appropriate directory */
    if (onProgress) onProgress('Extracting', mod.name);
    const extractOk = globalThis.host_extract_tar(tarPath, extractDir);
    if (!extractOk) {
        return { success: false, error: 'Extract failed' };
    }

    /* Rescan modules */
    globalThis.host_rescan_modules();

    console.log(`Installed module: ${mod.id}`);
    return { success: true, error: null };
}

/* Remove a module, returns { success, error } */
export function removeModule(mod) {
    if (!isValidModuleId(mod.id)) {
        return { success: false, error: 'Invalid module ID' };
    }
    /* Determine module path based on component_type */
    const subdir = getInstallSubdir(mod.component_type);
    const modulePath = subdir
        ? `${MODULES_DIR}/${subdir}/${mod.id}`
        : `${MODULES_DIR}/${mod.id}`;

    /* Remove the module directory */
    const removeOk = globalThis.host_remove_dir(modulePath);
    if (!removeOk) {
        return { success: false, error: 'Remove failed' };
    }

    /* Rescan modules */
    globalThis.host_rescan_modules();

    return { success: true, error: null };
}

/* Scan installed modules, returns { moduleId: version } map */
export function scanInstalledModules() {
    const installedModules = {};
    const modules = globalThis.host_list_modules();
    for (const mod of modules) {
        installedModules[mod.id] = mod.version;
    }
    return installedModules;
}
