/*
 * Reusable filepath parameter browser helpers for Move Anything Shadow UI.
 *
 * This file is designed to be copied into move-anything/shared and imported
 * from shadow_ui.js with minimal glue code.
 */

const DEFAULT_ROOT = '/data/UserData';

function normalizePath(path) {
    if (!path || typeof path !== 'string') return '/';

    const isAbsolute = path.startsWith('/');
    const parts = path.split('/');
    const stack = [];

    for (const part of parts) {
        if (!part || part === '.') continue;
        if (part === '..') {
            if (stack.length > 0) stack.pop();
            continue;
        }
        stack.push(part);
    }

    if (isAbsolute) {
        return stack.length > 0 ? `/${stack.join('/')}` : '/';
    }
    return stack.join('/');
}

function dirname(path) {
    const normalized = normalizePath(path);
    if (normalized === '/' || normalized === '') return '/';
    const idx = normalized.lastIndexOf('/');
    if (idx <= 0) return '/';
    return normalized.slice(0, idx);
}

function joinPath(base, name) {
    if (!name) return normalizePath(base);
    if (name.startsWith('/')) return normalizePath(name);
    const cleanBase = normalizePath(base);
    if (cleanBase === '/') return normalizePath(`/${name}`);
    return normalizePath(`${cleanBase}/${name}`);
}

function basename(path) {
    const normalized = normalizePath(path);
    if (normalized === '/' || normalized === '') return normalized;
    const idx = normalized.lastIndexOf('/');
    return idx >= 0 ? normalized.slice(idx + 1) : normalized;
}

function isWithinRoot(path, root) {
    const p = normalizePath(path);
    const r = normalizePath(root);
    if (r === '/') return true;
    return p === r || p.startsWith(`${r}/`);
}

function parseFilter(filter) {
    if (!filter) return [];

    const values = Array.isArray(filter) ? filter : [filter];
    return values
        .map((v) => (typeof v === 'string' ? v.trim().toLowerCase() : ''))
        .filter((v) => v.length > 0);
}

function hasAllowedExtension(name, extensions) {
    if (extensions.length === 0) return true;
    const lower = String(name || '').toLowerCase();
    return extensions.some((ext) => lower.endsWith(ext));
}

function isLikelySelectedFile(path, extensions) {
    const name = basename(path);
    if (!name || name === '/' || name === '.' || name === '..') return false;
    if (extensions.length > 0) return hasAllowedExtension(name, extensions);
    return name.includes('.');
}

function isDirectoryStat(rawStat) {
    if (Array.isArray(rawStat)) {
        const statObj = rawStat[0];
        const err = rawStat[1];
        if (err && err !== 0) return false;
        return isDirectoryStat(statObj);
    }

    if (!rawStat || typeof rawStat !== 'object') return false;

    if (typeof rawStat.isDirectory === 'function') {
        return !!rawStat.isDirectory();
    }

    if (typeof rawStat.mode === 'number') {
        return (rawStat.mode & 0o170000) === 0o040000;
    }

    if (rawStat.type === 'directory') return true;
    if (rawStat.type === 'dir') return true;

    return false;
}

function defaultFsAdapter(osModule) {
    return {
        readdir(path) {
            const out = osModule.readdir(path) || [];
            if (Array.isArray(out[0])) return out[0];
            if (Array.isArray(out)) return out;
            return [];
        },
        stat(path) {
            return osModule.stat(path);
        }
    };
}

export function buildFilepathBrowserState(paramMeta, currentValue) {
    const meta = paramMeta || {};
    const root = normalizePath(meta.root || DEFAULT_ROOT);
    const filter = parseFilter(meta.filter);

    /* Priority: current value -> optional start_path -> root */
    const rawCandidate = currentValue && currentValue.length > 0
        ? currentValue
        : (meta.start_path || root);
    const startCandidate = normalizePath(rawCandidate);

    let currentDir = root;
    let selectedPath = '';

    if (isWithinRoot(startCandidate, root)) {
        if (isLikelySelectedFile(startCandidate, filter)) {
            currentDir = dirname(startCandidate);
            selectedPath = startCandidate;
        } else {
            currentDir = startCandidate;
        }
    }

    return {
        title: meta.name || meta.key || 'File',
        key: meta.key || '',
        root,
        currentDir,
        selectedIndex: 0,
        filter,
        selectedPath,
        items: [],
        error: ''
    };
}

export function refreshFilepathBrowser(state, fsLike) {
    if (!state) return;
    const fs = fsLike || defaultFsAdapter(globalThis.os);

    const currentDir = isWithinRoot(state.currentDir, state.root)
        ? state.currentDir
        : state.root;

    const dirs = [];
    const files = [];

    state.items = [];
    state.error = '';

    if (currentDir !== state.root) {
        const parent = dirname(currentDir);
        if (isWithinRoot(parent, state.root)) {
            state.items.push({
                kind: 'up',
                label: '..',
                path: parent
            });
        }
    }

    try {
        const names = fs.readdir(currentDir) || [];
        for (const name of names) {
            if (!name || name === '.' || name === '..') continue;
            if (name.startsWith('.')) continue;  /* Skip dotfiles */
            const fullPath = joinPath(currentDir, name);
            const stat = fs.stat(fullPath);
            const isDir = isDirectoryStat(stat);

            if (isDir) {
                dirs.push({
                    kind: 'dir',
                    label: `[${name}]`,
                    path: fullPath
                });
                continue;
            }

            if (hasAllowedExtension(name, state.filter)) {
                files.push({
                    kind: 'file',
                    label: name,
                    path: fullPath
                });
            }
        }
    } catch (e) {
        state.error = 'Unable to read folder';
    }

    dirs.sort((a, b) => a.label.localeCompare(b.label));
    files.sort((a, b) => a.label.localeCompare(b.label));
    state.items.push(...dirs, ...files);

    if (state.selectedPath) {
        const idx = state.items.findIndex((item) => item.path === state.selectedPath);
        if (idx >= 0) {
            state.selectedIndex = idx;
            state.selectedPath = '';
        } else if (currentDir !== state.root) {
            state.currentDir = state.root;
            state.selectedIndex = 0;
            state.selectedPath = '';
            refreshFilepathBrowser(state, fsLike);
            return;
        } else {
            state.selectedIndex = 0;
            state.selectedPath = '';
        }
    }

    if (state.selectedIndex >= state.items.length) {
        state.selectedIndex = Math.max(0, state.items.length - 1);
    }

    state.currentDir = currentDir;
}

export function moveFilepathBrowserSelection(state, delta) {
    if (!state || !state.items || state.items.length === 0) return;
    state.selectedIndex = Math.max(
        0,
        Math.min(state.items.length - 1, state.selectedIndex + delta)
    );
}

export function activateFilepathBrowserItem(state) {
    if (!state || !state.items || state.items.length === 0) {
        return { action: 'noop' };
    }

    const item = state.items[state.selectedIndex];
    if (!item) return { action: 'noop' };

    if (item.kind === 'up' || item.kind === 'dir') {
        state.currentDir = item.path;
        state.selectedIndex = 0;
        state.selectedPath = '';
        return { action: 'open', path: item.path };
    }

    if (item.kind === 'file') {
        return {
            action: 'select',
            key: state.key,
            value: item.path,
            filename: basename(item.path)
        };
    }

    return { action: 'noop' };
}
