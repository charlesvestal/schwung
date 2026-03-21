/*
 * Shadow UI - Tools views (menu, file browser, engine select, confirm,
 * processing, result, stem review).
 *
 * Extracted from shadow_ui.js to allow forks to modify tool
 * presentation without touching core.
 */
import * as os from 'os';
import * as std from 'std';
import { ctx } from './shadow_ui_ctx.mjs';
import {
    SCREEN_WIDTH,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_RULE_Y,
    truncateText
} from '/data/UserData/schwung/shared/chain_ui_views.mjs';
import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter,
    drawMenuList
} from '/data/UserData/schwung/shared/menu_layout.mjs';
import {
    announce
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ---- Helpers ------------------------------------------------------------ */

function formatTime(seconds) {
    seconds = Math.round(seconds);
    if (seconds < 60) return seconds + "s";
    const m = Math.floor(seconds / 60);
    const s = seconds % 60;
    return m + "m " + (s < 10 ? "0" : "") + s + "s";
}

function getToolProcessingRatio() {
    const { toolSelectedEngine } = ctx;
    if (toolSelectedEngine && toolSelectedEngine.processing_ratio) {
        return toolSelectedEngine.processing_ratio;
    }
    return 0.5;
}

/* ---- Scan --------------------------------------------------------------- */

export function scanForToolModules() {
    const TOOLS_DIR = "/data/UserData/schwung/modules/tools";
    const result = [];
    const { debugLog } = ctx;

    debugLog("scanForToolModules starting");

    try {
        const entries = os.readdir(TOOLS_DIR) || [];
        const dirList = entries[0];
        if (!Array.isArray(dirList)) {
            debugLog("scanForToolModules: no entries");
            return result;
        }

        for (const entry of dirList) {
            if (entry === "." || entry === "..") continue;
            const dirPath = `${TOOLS_DIR}/${entry}`;
            const modulePath = `${dirPath}/module.json`;
            try {
                const content = std.loadFile(modulePath);
                if (!content) continue;
                const json = JSON.parse(content);
                if (json.component_type === "tool" && json.tool_config && !json.hidden) {
                    debugLog("FOUND tool: " + json.name);
                    result.push({
                        id: json.id || entry,
                        name: json.name || entry,
                        path: dirPath,
                        tool_config: json.tool_config || null,
                        capabilities: json.capabilities || null
                    });
                }
            } catch (e) {
                /* Skip directories without readable module.json */
            }
        }
    } catch (e) {
        debugLog("scanForToolModules error: " + e);
    }

    result.sort((a, b) => a.name.localeCompare(b.name));
    debugLog("scanForToolModules: found " + result.length + " tools");
    return result;
}

/* ---- Enter -------------------------------------------------------------- */

export function enterToolsMenu() {
    const { setView, VIEWS } = ctx;
    ctx.toolModules = scanForToolModules();
    ctx.toolsMenuIndex = 0;
    setView(VIEWS.TOOLS);
    ctx.needsRedraw = true;
    if (ctx.toolModules.length > 0) {
        announce("Tools, " + ctx.toolModules[0].name);
    } else {
        announce("Tools, no tools installed");
    }
}

/* ---- Draw --------------------------------------------------------------- */

export function drawToolsMenu() {
    const { toolModules, toolsMenuIndex, menuLayoutDefaults } = ctx;

    clear_screen();
    drawHeader("Tools");

    if (toolModules.length === 0) {
        print(4, 28, "No tools installed", 1);
        drawFooter({left: "Back: Exit"});
        return;
    }

    const items = toolModules.map(m => ({
        label: m.name,
        value: ""
    }));
    drawMenuList({
        items,
        selectedIndex: toolsMenuIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label,
        getValue: (item) => item.value
    });

    drawFooter({left: "Back: Exit", right: "Jog: Select"});
}

export function drawToolFileBrowser() {
    const { toolBrowserState, menuLayoutDefaults } = ctx;

    clear_screen();

    if (!toolBrowserState) {
        drawHeader("Browser");
        print(4, 28, "No files found", 1);
        drawFooter({left: "Back: Up"});
        return;
    }

    const dirName = toolBrowserState.currentDir.substring(
        toolBrowserState.currentDir.lastIndexOf("/") + 1);
    drawHeader(dirName);

    if (toolBrowserState.items.length === 0) {
        print(4, 28, "No files found", 1);
        drawFooter({left: "Back: Up"});
        return;
    }

    drawMenuList({
        items: toolBrowserState.items,
        selectedIndex: toolBrowserState.selectedIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label,
        getValue: () => ""
    });

    drawFooter({left: "Back: Up", right: "Jog: Select"});
}

export function drawToolEngineSelect() {
    const { toolAvailableEngines, toolEngineIndex, menuLayoutDefaults } = ctx;

    clear_screen();
    drawHeader("Choose Engine");

    const items = toolAvailableEngines.map(e => ({
        label: e.name,
        value: e.stems ? e.stems.length + " stems" : ""
    }));
    drawMenuList({
        items,
        selectedIndex: toolEngineIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label,
        getValue: (item) => item.value
    });

    drawFooter({left: "Back: Files", right: "Jog: Select"});
}

export function drawToolConfirm() {
    const { toolActiveTool, toolSelectedFile, toolSelectedEngine, toolSelectedSetName } = ctx;
    const isSetPicker = toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.set_picker;

    clear_screen();
    drawHeader(toolActiveTool ? toolActiveTool.name : "Confirm");

    if (isSetPicker) {
        const setName = toolSelectedSetName || "Unknown";
        const displayName = setName.length > 20 ? setName.substring(0, 17) + "..." : setName;
        print(4, 16, "Render this set?", 1);
        print(4, 28, displayName, 1);
    } else {
        const fileName = toolSelectedFile.substring(toolSelectedFile.lastIndexOf("/") + 1);
        const displayName = fileName.length > 20 ? fileName.substring(0, 17) + "..." : fileName;
        print(4, 16, "Process this file?", 1);
        print(4, 28, displayName, 1);

        if (toolSelectedEngine && toolSelectedEngine.name) {
            print(4, 40, "Engine: " + toolSelectedEngine.name, 1);
        }
    }

    drawFooter({right: "Jog: Confirm"});
}

export function drawToolProcessing() {
    const { toolActiveTool, toolSelectedFile, toolProcessStartTime,
            toolFileDurationSec, toolStemsFound, toolExpectedStems, toolSelectedSetName } = ctx;
    const isSetPicker = toolActiveTool && toolActiveTool.tool_config && toolActiveTool.tool_config.set_picker;

    clear_screen();
    drawHeader(toolActiveTool ? toolActiveTool.name : "Processing");

    let displayName;
    if (isSetPicker) {
        const setName = toolSelectedSetName || "Set";
        displayName = setName.length > 20 ? setName.substring(0, 17) + "..." : setName;
    } else {
        const fileName = toolSelectedFile.substring(toolSelectedFile.lastIndexOf("/") + 1);
        displayName = fileName.length > 20 ? fileName.substring(0, 17) + "..." : fileName;
    }

    const elapsedMs = Date.now() - toolProcessStartTime;
    const elapsedSec = Math.floor(elapsedMs / 1000);
    const elapsedStr = formatTime(elapsedSec);

    ctx.toolProcessingDots = (ctx.toolProcessingDots + 1) % 4;
    const dots = ".".repeat(ctx.toolProcessingDots + 1);

    print(4, 14, displayName, 1);
    print(4, 24, (isSetPicker ? "Rendering" : "Processing") + dots, 1);
    print(4, 34, elapsedStr + " elapsed", 1);

    if (!isSetPicker) {
        const estTotalSec = Math.round(toolFileDurationSec * getToolProcessingRatio());
        let remainStr = "";
        if (estTotalSec > 0) {
            const remainSec = Math.max(0, estTotalSec - elapsedSec);
            remainStr = "~" + formatTime(remainSec) + " left";
        }
        if (remainStr) {
            print(76, 34, remainStr, 1);
        }

        const stemProgress = toolExpectedStems > 0
            ? toolStemsFound + "/" + toolExpectedStems + " stems"
            : "";
        if (stemProgress) {
            print(4, 44, stemProgress, 1);
        }
    }

    drawFooter({left: "Back: Cancel"});
}

export function drawToolResult() {
    const { toolResultSuccess, toolResultMessage } = ctx;

    clear_screen();
    drawHeader(toolResultSuccess ? "Complete" : "Error");

    const lines = toolResultMessage.split("\n");
    let y = 20;
    for (const line of lines) {
        print(4, y, line, 1);
        y += 12;
    }

    drawFooter({left: "Back: Tools"});
}

export function drawToolStemReview() {
    const { toolStemFiles, toolStemReviewIndex, toolStemKept,
            menuLayoutDefaults } = ctx;

    clear_screen();
    drawHeader("Stems");

    const keptCount = toolStemKept.filter(k => k).length;
    let actionLabel;
    if (keptCount === 0) {
        actionLabel = ">> Cancel";
    } else if (keptCount === toolStemFiles.length) {
        actionLabel = ">> Save All";
    } else {
        actionLabel = ">> Save " + keptCount + " Stem" + (keptCount > 1 ? "s" : "");
    }
    const items = [{ label: actionLabel, value: "" }];
    for (let i = 0; i < toolStemFiles.length; i++) {
        const name = toolStemFiles[i].replace(/\.wav$/i, "");
        const prefix = toolStemKept[i] ? "[x] " : "[ ] ";
        items.push({ label: prefix + name, value: "" });
    }

    drawMenuList({
        items,
        selectedIndex: toolStemReviewIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label,
        getValue: (item) => item.value
    });

    drawFooter({right: "Select"});
}

export function drawToolSetPicker() {
    const { toolSetList, toolSetPickerIndex, menuLayoutDefaults } = ctx;

    clear_screen();
    drawHeader("Choose Set");

    if (toolSetList.length === 0) {
        print(4, 28, "No sets found", 1);
        drawFooter({left: "Back: Tools"});
        return;
    }

    drawMenuList({
        items: toolSetList,
        selectedIndex: toolSetPickerIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name,
        getValue: () => ""
    });

    drawFooter({left: "Back: Tools", right: "Jog: Select"});
}
