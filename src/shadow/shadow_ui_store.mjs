/*
 * Shadow UI - Module Store views (categories, list, detail, loading, result).
 *
 * Extracted from shadow_ui.js to allow forks to modify store
 * presentation without touching core.
 */
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/move-anything/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/move-anything/shared/menu_layout.mjs';
import {
    announce
} from '/data/UserData/move-anything/shared/screen_reader.mjs';

/* ---- Helpers ------------------------------------------------------------ */

function buildReleaseNoteLines(notesText) {
    const { wrapText } = ctx;
    const lines = [];
    const noteLines = notesText.split('\n');
    for (const line of noteLines) {
        if (line.trim() === '') {
            lines.push('');
        } else {
            const cleaned = line.trim()
                .replace(/^#+\s*/, '')
                .replace(/\*\*/g, '')
                .replace(/\*/g, '');
            const wrapped = wrapText(cleaned, 20);
            lines.push(...wrapped);
        }
    }
    return lines;
}

/* ---- Draw --------------------------------------------------------------- */

export function drawStorePickerCategories() {
    const { storeCategoryItems, storeCategoryIndex, menuLayoutDefaults } = ctx;

    clear_screen();
    drawHeader('Module Store');

    if (storeCategoryItems.length === 0) {
        print(2, 28, "No modules available", 1);
        drawFooter('Back: return');
        return;
    }

    drawMenuList({
        items: storeCategoryItems,
        selectedIndex: storeCategoryIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.label,
        getValue: (item) => item.value || ''
    });

    drawFooter({left: "Back: return", right: "Jog: browse"});
}

export function drawStorePickerList() {
    const { storePickerCategory, storePickerModules, storePickerSelectedIndex,
            storeInstalledModules, getModuleStatus, CATEGORIES,
            menuLayoutDefaults } = ctx;

    clear_screen();

    const cat = CATEGORIES.find(c => c.id === storePickerCategory);
    const catName = cat ? cat.name : 'Modules';
    drawHeader('Store: ' + catName);

    if (storePickerModules.length === 0) {
        print(2, 28, "No modules available", 1);
        drawFooter('Back: return');
        return;
    }

    const items = storePickerModules.map(mod => {
        let statusIcon = '';
        if (mod._isHostUpdate) {
            statusIcon = '^';
        } else {
            const status = getModuleStatus(mod, storeInstalledModules);
            if (status.installed) {
                statusIcon = status.hasUpdate ? '^' : '*';
            }
        }
        return { ...mod, statusIcon };
    });

    drawMenuList({
        items,
        selectedIndex: storePickerSelectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        valueAlignRight: true,
        getLabel: (item) => item.name,
        getValue: (item) => item.statusIcon
    });

    drawFooter({left: "Back: return", right: "Jog: browse"});
}

export function drawStorePickerLoading() {
    const { storePickerLoadingTitle, storePickerLoadingMessage,
            drawStatusOverlay } = ctx;

    clear_screen();
    const title = storePickerLoadingTitle || 'Module Store';
    const msg = storePickerLoadingMessage || 'Loading...';
    drawStatusOverlay(title, msg);
}

export function drawStorePickerResult() {
    const { storePickerResultTitle, storePickerMessage } = ctx;

    clear_screen();
    drawHeader(storePickerResultTitle || 'Module Store');

    const msg = storePickerMessage || 'Done';
    print(2, 28, msg, 1);

    drawFooter('Press to continue');
}

export function drawStorePickerDetail() {
    const { storePickerCurrentModule, storeInstalledModules, storeHostVersion,
            storeDetailScrollState, getModuleStatus,
            fetchReleaseNotes, createScrollableText, drawScrollableText,
            wrapText } = ctx;

    clear_screen();

    const mod = storePickerCurrentModule;
    if (!mod) return;

    if (mod._isHostUpdate) {
        const title = 'Core Update';
        const versionStr = `${storeHostVersion}->${mod.latest_version}`;
        drawHeader(title, versionStr);

        if (!storeDetailScrollState || storeDetailScrollState.moduleId !== mod.id) {
            const notes = fetchReleaseNotes('charlesvestal/move-anything');
            const descLines = [];
            descLines.push(`${storeHostVersion} -> ${mod.latest_version}`);
            descLines.push('');
            if (notes) {
                descLines.push(...buildReleaseNoteLines(notes));
            } else {
                descLines.push('Update Move Anything');
                descLines.push('core framework.');
                descLines.push('');
                descLines.push('Restart required');
                descLines.push('after update.');
            }
            ctx.storeDetailScrollState = createScrollableText({
                lines: descLines,
                actionLabel: 'Update',
                visibleLines: 3,
                onActionSelected: (label) => announce(label)
            });
            ctx.storeDetailScrollState.moduleId = mod.id;
        }

        drawScrollableText({
            state: ctx.storeDetailScrollState,
            topY: 16,
            bottomY: 40,
            actionY: 52
        });
        return;
    }

    const status = getModuleStatus(mod, storeInstalledModules);

    let title = mod.name;
    let versionStr = `v${mod.latest_version}`;
    if (status.installed && status.hasUpdate) {
        versionStr = `${status.installedVersion}->${mod.latest_version}`;
        if (title.length > 8) title = title.substring(0, 7) + '~';
    } else {
        if (title.length > 12) title = title.substring(0, 11) + '~';
    }
    drawHeader(title, versionStr);

    if (!storeDetailScrollState || storeDetailScrollState.moduleId !== mod.id) {
        const descLines = wrapText(mod.description || 'No description available.', 20);

        descLines.push('');
        descLines.push(`by ${mod.author || 'Unknown'}`);

        if (mod.requires) {
            descLines.push('');
            descLines.push('Requires:');
            const reqLines = wrapText(mod.requires, 18);
            descLines.push(...reqLines);
        }

        if (mod.github_repo) {
            const notes = fetchReleaseNotes(mod.github_repo);
            if (notes) {
                descLines.push('');
                descLines.push('What\'s New:');
                descLines.push(...buildReleaseNoteLines(notes));
            }
        }

        let actionLabel;
        if (status.installed) {
            actionLabel = status.hasUpdate ? 'Update' : 'Reinstall';
        } else {
            actionLabel = 'Install';
        }

        ctx.storeDetailScrollState = createScrollableText({
            lines: descLines,
            actionLabel,
            visibleLines: 3,
            onActionSelected: (label) => announce(label)
        });
        ctx.storeDetailScrollState.moduleId = mod.id;
    }

    drawScrollableText({
        state: ctx.storeDetailScrollState,
        topY: 16,
        bottomY: 40,
        actionY: 52
    });
}
