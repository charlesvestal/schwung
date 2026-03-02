/*
 * File Browser Tool
 *
 * Browse files and folders on the device. Two roots:
 *   - User Library (/data/UserData/UserLibrary)
 *   - System Files (/data/UserData/move-anything) (with warning)
 *
 * Supports: Delete, Rename, Duplicate, Copy, Move, New Folder.
 *
 * Controls:
 *   Jog wheel     - Scroll list
 *   Jog click     - Open dir / file actions
 *   Shift+Click   - Actions menu (works on dirs too)
 *   Back          - Navigate up / exit
 *
 * 128x64 1-bit monochrome display, QuickJS runtime.
 */

import * as os from 'os';
import * as std from 'std';

import {
    MidiCC, MidiNoteOn,
    MoveMainKnob, MoveMainButton, MoveBack
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    decodeDelta
} from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    buildFilepathBrowserState,
    refreshFilepathBrowser,
    moveFilepathBrowserSelection,
    activateFilepathBrowserItem
} from '/data/UserData/move-anything/shared/filepath_browser.mjs';

import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry
} from '/data/UserData/move-anything/shared/text_entry.mjs';

/* ============ Constants ============ */

var VIEW_ROOT_PICKER = 1;
var VIEW_WARNING = 2;
var VIEW_BROWSER = 3;
var VIEW_ACTIONS = 4;
var VIEW_CONFIRM = 5;
var VIEW_DEST_BROWSER = 6;

var SCREEN_W = 128;
var SCREEN_H = 64;
var MAX_VISIBLE = 4;

var ROOT_USER_LIBRARY = "/data/UserData/UserLibrary";
var ROOT_SYSTEM = "/data/UserData/move-anything";

/* ============ State ============ */

var currentView = VIEW_ROOT_PICKER;
var rootPickerIndex = 0;
var rootLabel = "";
var rootPath = "";

/* Browser state (shared filepath_browser) */
var browserState = null;

/* Actions menu */
var actionsIndex = 0;
var actionsItems = [];
var selectedItemPath = "";
var selectedItemKind = "";  /* 'file' or 'dir' */

/* Confirm dialog */
var confirmAction = "";     /* 'delete' */
var confirmPath = "";

/* Destination browser (for copy/move/duplicate) */
var destBrowserState = null;
var pendingOperation = "";  /* 'copy', 'move', or 'duplicate' */
var pendingSourcePath = "";
var lastDestDir = "";       /* Remember last destination across operations */

/* Status message */
var statusMessage = "";
var statusTicks = 0;

/* Shift state - read from shim's shared memory (CC 49 not forwarded in tool mode) */
function isShiftHeld() {
    if (typeof shadow_get_shift_held === "function") {
        return shadow_get_shift_held() !== 0;
    }
    return false;
}

/* ============ Filesystem Adapter ============ */

var FILEPATH_BROWSER_FS = {
    readdir: function(path) {
        var out = os.readdir(path) || [];
        if (Array.isArray(out[0])) return out[0];
        if (Array.isArray(out)) return out;
        return [];
    },
    stat: function(path) {
        return os.stat(path);
    }
};

/* ============ Helpers ============ */

function basename(path) {
    var idx = path.lastIndexOf("/");
    return idx >= 0 ? path.slice(idx + 1) : path;
}

function dirname(path) {
    var idx = path.lastIndexOf("/");
    if (idx <= 0) return "/";
    return path.slice(0, idx);
}

function extension(path) {
    var name = basename(path);
    var idx = name.lastIndexOf(".");
    if (idx <= 0) return "";
    return name.slice(idx);
}

function nameWithoutExt(path) {
    var name = basename(path);
    var idx = name.lastIndexOf(".");
    if (idx <= 0) return name;
    return name.slice(0, idx);
}

function isDirectory(path) {
    try {
        var st = os.stat(path);
        if (!st) return false;
        var obj = Array.isArray(st) ? st[0] : st;
        if (!obj || typeof obj !== 'object') return false;
        if (typeof obj.mode === 'number') {
            return (obj.mode & 0o170000) === 0o040000;
        }
        return false;
    } catch (e) {
        return false;
    }
}

function fileExists(path) {
    try {
        var st = os.stat(path);
        if (!st) return false;
        var obj = Array.isArray(st) ? st[0] : st;
        return (obj && typeof obj === 'object');
    } catch (e) {
        return false;
    }
}

function showStatus(msg) {
    statusMessage = msg;
    statusTicks = 120;  /* ~2 seconds at 60fps */
}

function setView(v) {
    currentView = v;
}

/* Truncate a string for display, max chars */
function truncLabel(str, max) {
    if (!max) max = 21;
    if (str.length > max) return str.slice(0, max - 3) + "...";
    return str;
}

/* ============ Copy Implementation ============ */

function copyFile(src, dest) {
    var f = std.open(src, "rb");
    if (!f) return false;
    var out = std.open(dest, "wb");
    if (!out) { f.close(); return false; }
    var buf = new ArrayBuffer(4096);
    var n;
    while ((n = f.read(buf, 0, 4096)) > 0) {
        out.write(buf, 0, n);
    }
    f.close();
    out.close();
    return true;
}

/* Generate a unique name by appending (2), (3), etc. */
function uniquePath(dir, base, ext) {
    var candidate = dir + "/" + base + ext;
    if (!fileExists(candidate)) return candidate;
    for (var i = 2; i < 100; i++) {
        candidate = dir + "/" + base + " (" + i + ")" + ext;
        if (!fileExists(candidate)) return candidate;
    }
    return candidate;
}

/* ============ File Operations ============ */

function doDelete(path) {
    try {
        var ret = os.remove(path);
        if (ret < 0) {
            showStatus("Error: not empty");
            setView(VIEW_BROWSER);
            return;
        }
        showStatus("Deleted");
    } catch (e) {
        showStatus("Error: " + e);
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

function doRename(oldPath, newName) {
    if (!newName || newName.length === 0) return;
    var dir = dirname(oldPath);
    var newPath = dir + "/" + newName;
    try {
        os.rename(oldPath, newPath);
        showStatus("Renamed");
    } catch (e) {
        showStatus("Error: " + e);
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

function doNewFolder(parentDir, name) {
    if (!name || name.length === 0) return;
    var newPath = parentDir + "/" + name;
    var ret;
    try {
        ret = os.mkdir(newPath, 0o755);
    } catch (e) {
        showStatus("Error: " + e);
        refreshBrowser();
        setView(VIEW_BROWSER);
        return;
    }
    if (ret < 0) {
        showStatus("Error: mkdir failed");
    } else {
        showStatus("Folder created");
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

function doDuplicate(src) {
    var dir = dirname(src);
    var base = nameWithoutExt(src);
    var ext = extension(src);
    var dest = uniquePath(dir, base, ext);
    try {
        if (copyFile(src, dest)) {
            showStatus("Duplicated");
        } else {
            showStatus("Error: copy failed");
        }
    } catch (e) {
        showStatus("Error: " + e);
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

function doCopy(src, destDir) {
    var base = nameWithoutExt(src);
    var ext = extension(src);
    var dest = uniquePath(destDir, base, ext);
    try {
        if (copyFile(src, dest)) {
            lastDestDir = destDir;
            showStatus("Copied");
        } else {
            showStatus("Error: copy failed");
        }
    } catch (e) {
        showStatus("Error: " + e);
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

function doMove(src, destDir) {
    var name = basename(src);
    var dest = destDir + "/" + name;
    try {
        os.rename(src, dest);
        lastDestDir = destDir;
        showStatus("Moved");
    } catch (e) {
        showStatus("Error: " + e);
    }
    refreshBrowser();
    setView(VIEW_BROWSER);
}

/* ============ Browser Helpers ============ */

function openBrowser(root, label) {
    rootPath = root;
    rootLabel = label;
    browserState = buildFilepathBrowserState(
        { root: root, filter: "", name: label },
        ""
    );
    refreshFilepathBrowser(browserState, FILEPATH_BROWSER_FS);
    setView(VIEW_BROWSER);
}

function refreshBrowser() {
    if (browserState) {
        refreshFilepathBrowser(browserState, FILEPATH_BROWSER_FS);
    }
}

function currentBrowserDir() {
    return browserState ? browserState.currentDir : "/data/UserData";
}

function openDestBrowser() {
    /* Default to last used dir, or current browsing dir */
    var startDir = lastDestDir || currentBrowserDir();
    destBrowserState = buildFilepathBrowserState(
        { root: "/data/UserData", filter: "__dirs_only__", name: "Choose Dest" },
        startDir
    );
    /* Override currentDir since buildFilepathBrowserState may not navigate there */
    if (startDir.indexOf("/data/UserData") === 0) {
        destBrowserState.currentDir = startDir;
    }
    refreshDestBrowser();
    setView(VIEW_DEST_BROWSER);
}

function refreshDestBrowser() {
    if (!destBrowserState) return;
    /* Custom refresh that only shows directories */
    var fs = FILEPATH_BROWSER_FS;
    var state = destBrowserState;
    var currentDir = state.currentDir;

    state.items = [];
    state.error = "";

    /* Add "select this folder" option */
    state.items.push({ kind: "select_here", label: "> Use This Folder", path: currentDir });

    /* Add parent directory navigation */
    if (currentDir !== state.root) {
        var parent = dirname(currentDir);
        if (parent.indexOf(state.root) === 0 || parent === state.root) {
            state.items.push({ kind: "up", label: "..", path: parent });
        }
    }

    try {
        var names = fs.readdir(currentDir) || [];
        var dirs = [];
        for (var i = 0; i < names.length; i++) {
            var name = names[i];
            if (!name || name === "." || name === "..") continue;
            if (name.startsWith(".")) continue;
            var fullPath = currentDir + "/" + name;
            if (isDirectory(fullPath)) {
                dirs.push({ kind: "dir", label: "[" + name + "]", path: fullPath });
            }
        }
        dirs.sort(function(a, b) { return a.label.localeCompare(b.label); });
        for (var j = 0; j < dirs.length; j++) {
            state.items.push(dirs[j]);
        }
    } catch (e) {
        state.error = "Unable to read folder";
    }

    if (state.selectedIndex >= state.items.length) {
        state.selectedIndex = Math.max(0, state.items.length - 1);
    }
    state.currentDir = currentDir;
}

/* ============ Actions Menu ============ */

function openActionsMenu(path, kind) {
    selectedItemPath = path;
    selectedItemKind = kind;
    actionsIndex = 0;

    if (kind === "dir") {
        actionsItems = ["New Folder", "Rename", "Delete", "Copy to...", "Move to..."];
    } else {
        actionsItems = ["Duplicate", "Rename", "Delete", "Copy to...", "Move to..."];
    }
    setView(VIEW_ACTIONS);
}

function executeAction(action) {
    switch (action) {
        case "Delete":
            confirmAction = "delete";
            confirmPath = selectedItemPath;
            setView(VIEW_CONFIRM);
            break;
        case "Rename":
            openTextEntry({
                title: "Rename",
                initialText: basename(selectedItemPath),
                onConfirm: function(text) {
                    doRename(selectedItemPath, text);
                },
                onCancel: function() {
                    setView(VIEW_ACTIONS);
                }
            });
            break;
        case "New Folder":
            openTextEntry({
                title: "New Folder",
                initialText: "",
                onConfirm: function(text) {
                    /* Create inside the current browsed directory */
                    doNewFolder(currentBrowserDir(), text);
                },
                onCancel: function() {
                    setView(VIEW_ACTIONS);
                }
            });
            break;
        case "Duplicate":
            doDuplicate(selectedItemPath);
            break;
        case "Copy to...":
            pendingOperation = "copy";
            pendingSourcePath = selectedItemPath;
            openDestBrowser();
            break;
        case "Move to...":
            pendingOperation = "move";
            pendingSourcePath = selectedItemPath;
            openDestBrowser();
            break;
    }
}

/* ============ Drawing ============ */

function drawHeader(title) {
    print(2, 2, title, 1);
    fill_rect(0, 12, SCREEN_W, 1, 1);
}

function drawFooter(text) {
    fill_rect(0, 55, SCREEN_W, 1, 1);
    if (typeof text === "string") {
        print(2, 57, text, 1);
    } else if (text && typeof text === "object") {
        if (text.left) print(2, 57, text.left, 1);
        if (text.right) {
            var w = text_width(text.right);
            print(SCREEN_W - w - 2, 57, text.right, 1);
        }
    }
}

function drawList(items, selectedIdx, topY) {
    var scrollOffset = 0;
    if (selectedIdx >= MAX_VISIBLE) {
        scrollOffset = selectedIdx - MAX_VISIBLE + 1;
    }

    for (var i = 0; i < MAX_VISIBLE; i++) {
        var idx = scrollOffset + i;
        if (idx >= items.length) break;
        var y = topY + i * 9;
        var label = items[idx];
        if (typeof label === "object") label = label.label || "";
        label = truncLabel(label);
        if (idx === selectedIdx) {
            fill_rect(0, y - 1, SCREEN_W, 9, 1);
            print(4, y, label, 0);  /* inverted */
        } else {
            print(4, y, label, 1);
        }
    }

    /* Scroll indicator */
    if (items.length > MAX_VISIBLE) {
        var trackH = MAX_VISIBLE * 9;
        var barH = Math.max(4, Math.floor((MAX_VISIBLE / items.length) * trackH));
        var maxScroll = items.length - MAX_VISIBLE;
        var barY = topY + (maxScroll > 0 ? Math.floor((scrollOffset / maxScroll) * (trackH - barH)) : 0);
        fill_rect(SCREEN_W - 2, barY, 2, barH, 1);
    }
}

/* Draw item count indicator in header area */
function drawItemCount(count, selectedIdx) {
    if (count > MAX_VISIBLE) {
        var countStr = (selectedIdx + 1) + "/" + count;
        var w = text_width(countStr);
        print(SCREEN_W - w - 2, 2, countStr, 1);
    }
}

function drawRootPicker() {
    clear_screen();
    drawHeader("File Browser");

    var items = ["User Library", "System Files"];
    drawList(items, rootPickerIndex, 15);

    drawFooter("Push: Open");
}

function drawWarning() {
    clear_screen();

    /* Centered warning box */
    fill_rect(4, 8, 120, 40, 1);
    fill_rect(6, 10, 116, 36, 0);

    print(12, 14, "WARNING", 1);
    print(12, 26, "Changing system", 1);
    print(12, 36, "files may break ME", 1);

    drawFooter("Push: Continue");
}

function drawBrowser() {
    clear_screen();

    /* Header with current dir */
    var dirName = basename(browserState.currentDir) || rootLabel;
    drawHeader(truncLabel(dirName, 16));

    if (browserState.error) {
        print(4, 24, browserState.error, 1);
        drawFooter("");
        return;
    }

    if (browserState.items.length === 0) {
        print(4, 24, "(empty folder)", 1);
        drawFooter("Shift+Click: Actions");
        return;
    }

    /* Item count in header */
    drawItemCount(browserState.items.length, browserState.selectedIndex);

    /* Draw items */
    drawList(browserState.items, browserState.selectedIndex, 15);

    /* Footer — shift+click works on everything */
    drawFooter("Shift+Click: Actions");
}

function drawActions() {
    clear_screen();

    var name = basename(selectedItemPath);
    drawHeader(truncLabel(name, 20));

    drawList(actionsItems, actionsIndex, 15);

    drawFooter("Push: Select");
}

function drawConfirm() {
    clear_screen();

    drawHeader("Confirm Delete");

    var name = basename(confirmPath);
    print(4, 18, "Delete this " + (isDirectory(confirmPath) ? "folder" : "file") + "?", 1);
    print(4, 30, truncLabel(name, 20), 1);

    drawFooter({ left: "Back: No", right: "Push: Delete" });
}

function drawDestBrowser() {
    clear_screen();

    var dirName = basename(destBrowserState.currentDir) || "Choose Dest";
    drawHeader("To: " + truncLabel(dirName, 14));

    if (destBrowserState.items.length === 0) {
        print(4, 24, "(no subfolders)", 1);
    } else {
        drawList(destBrowserState.items, destBrowserState.selectedIndex, 15);
    }

    drawFooter("Push: Select");
}

function drawStatusOverlay() {
    if (statusTicks > 0) {
        var w = text_width(statusMessage) + 8;
        var x = Math.floor((SCREEN_W - w) / 2);
        fill_rect(x, 44, w, 12, 1);
        fill_rect(x + 1, 45, w - 2, 10, 0);
        print(x + 4, 47, statusMessage, 1);
    }
}

/* ============ MIDI Handling ============ */

function handleCC(cc, val) {
    var isDown = val > 0;

    /* Jog wheel - navigation */
    if (cc === MoveMainKnob && val > 0) {
        var delta = decodeDelta(val);
        switch (currentView) {
            case VIEW_ROOT_PICKER:
                rootPickerIndex = Math.max(0, Math.min(1, rootPickerIndex + delta));
                break;
            case VIEW_BROWSER:
                if (browserState) {
                    moveFilepathBrowserSelection(browserState, delta);
                }
                break;
            case VIEW_ACTIONS:
                actionsIndex = Math.max(0, Math.min(actionsItems.length - 1, actionsIndex + delta));
                break;
            case VIEW_DEST_BROWSER:
                if (destBrowserState && destBrowserState.items.length > 0) {
                    destBrowserState.selectedIndex = Math.max(0,
                        Math.min(destBrowserState.items.length - 1,
                            destBrowserState.selectedIndex + delta));
                }
                break;
        }
        return;
    }

    /* Jog click - select */
    if (cc === MoveMainButton && isDown) {
        switch (currentView) {
            case VIEW_ROOT_PICKER:
                if (rootPickerIndex === 0) {
                    openBrowser(ROOT_USER_LIBRARY, "User Library");
                } else {
                    setView(VIEW_WARNING);
                }
                break;
            case VIEW_WARNING:
                openBrowser(ROOT_SYSTEM, "System Files");
                break;
            case VIEW_BROWSER:
                handleBrowserSelect();
                break;
            case VIEW_ACTIONS:
                if (actionsIndex >= 0 && actionsIndex < actionsItems.length) {
                    executeAction(actionsItems[actionsIndex]);
                }
                break;
            case VIEW_CONFIRM:
                if (confirmAction === "delete") {
                    doDelete(confirmPath);
                }
                break;
            case VIEW_DEST_BROWSER:
                handleDestBrowserSelect();
                break;
        }
        return;
    }

    /* Back button */
    if (cc === MoveBack && isDown) {
        switch (currentView) {
            case VIEW_ROOT_PICKER:
                exitBrowser();
                break;
            case VIEW_WARNING:
                setView(VIEW_ROOT_PICKER);
                break;
            case VIEW_BROWSER:
                handleBrowserBack();
                break;
            case VIEW_ACTIONS:
                setView(VIEW_BROWSER);
                break;
            case VIEW_CONFIRM:
                setView(VIEW_ACTIONS);
                break;
            case VIEW_DEST_BROWSER:
                setView(VIEW_BROWSER);
                break;
        }
        return;
    }
}

function handleBrowserSelect() {
    if (!browserState || browserState.items.length === 0) return;

    /* Shift+click on any item opens actions menu */
    if (isShiftHeld()) {
        var sel = browserState.items[browserState.selectedIndex];
        if (sel && (sel.kind === "dir" || sel.kind === "file")) {
            openActionsMenu(sel.path, sel.kind);
        }
        return;
    }

    var result = activateFilepathBrowserItem(browserState);
    if (result.action === "open") {
        refreshFilepathBrowser(browserState, FILEPATH_BROWSER_FS);
    } else if (result.action === "select") {
        /* File selected — open actions menu */
        openActionsMenu(result.value, "file");
    }
}

function handleBrowserBack() {
    if (!browserState) {
        setView(VIEW_ROOT_PICKER);
        return;
    }

    /* If at root, go back to root picker */
    if (browserState.currentDir === browserState.root) {
        setView(VIEW_ROOT_PICKER);
        return;
    }

    /* Navigate up */
    browserState.currentDir = dirname(browserState.currentDir);
    browserState.selectedIndex = 0;
    refreshFilepathBrowser(browserState, FILEPATH_BROWSER_FS);
}

function handleDestBrowserSelect() {
    if (!destBrowserState) return;

    var sel = destBrowserState.items[destBrowserState.selectedIndex];
    if (!sel) {
        finishDestOperation(destBrowserState.currentDir);
        return;
    }

    if (sel.kind === "select_here") {
        finishDestOperation(sel.path);
    } else if (sel.kind === "up" || sel.kind === "dir") {
        destBrowserState.currentDir = sel.path;
        destBrowserState.selectedIndex = 0;
        refreshDestBrowser();
    }
}

function finishDestOperation(destDir) {
    if (pendingOperation === "copy") {
        doCopy(pendingSourcePath, destDir);
    } else if (pendingOperation === "move") {
        doMove(pendingSourcePath, destDir);
    }
    pendingOperation = "";
    pendingSourcePath = "";
}

function exitBrowser() {
    if (typeof host_exit_module === "function") {
        host_exit_module();
    }
}

/* ============ Exported Entry Points ============ */

globalThis.init = function() {
    currentView = VIEW_ROOT_PICKER;
    rootPickerIndex = 0;
    statusMessage = "";
    statusTicks = 0;
};

globalThis.tick = function() {
    /* Text entry takes over when active */
    if (isTextEntryActive()) {
        tickTextEntry();
        drawTextEntry();
        return;
    }

    /* Status timeout */
    if (statusTicks > 0) {
        statusTicks--;
    }

    /* Draw current view */
    switch (currentView) {
        case VIEW_ROOT_PICKER:
            drawRootPicker();
            break;
        case VIEW_WARNING:
            drawWarning();
            break;
        case VIEW_BROWSER:
            drawBrowser();
            break;
        case VIEW_ACTIONS:
            drawActions();
            break;
        case VIEW_CONFIRM:
            drawConfirm();
            break;
        case VIEW_DEST_BROWSER:
            drawDestBrowser();
            break;
    }

    drawStatusOverlay();
};

globalThis.onMidiMessageInternal = function(data) {
    var status = data[0] & 0xF0;
    var d1 = data[1];
    var d2 = data[2];

    /* Text entry handles its own MIDI when active */
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    if (status === MidiCC) {
        handleCC(d1, d2);
    }
};
