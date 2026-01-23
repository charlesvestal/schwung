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
