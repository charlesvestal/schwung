# Slot Preset CRUD Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement save/save-as/delete with overwrite confirmation and remove rename.

**Architecture:** Add CONFIRM_OVERWRITE view, modify editor menu items and action handlers, add conflict detection using patchNames array.

**Tech Stack:** JavaScript (chain/ui.js), text_entry.mjs keyboard

---

## Task 1: Add CONFIRM_OVERWRITE View

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add view constant**

Find `EDITOR_VIEW` object (around line 348) and add new view:

```javascript
const EDITOR_VIEW = {
    OVERVIEW: "overview",
    SLOT_MENU: "slot_menu",
    COMPONENT_PICKER: "component_picker",
    PARAM_EDITOR: "param_editor",
    KNOB_EDITOR: "knob_editor",
    KNOB_PARAM_PICKER: "knob_param_picker",
    CONFIRM_DELETE: "confirm_delete",
    CONFIRM_OVERWRITE: "confirm_overwrite"  // ADD THIS
};
```

**Step 2: Add state for overwrite target**

Find `editorState` initialization (around line 380) and add:
- `overwriteTargetName` - name being overwritten
- `overwriteTargetIndex` - index of patch to overwrite (-1 if new)
- `pendingSaveName` - name user entered (for retry after "No")

Add these to both createEditorState branches:
```javascript
overwriteTargetName: "",
overwriteTargetIndex: -1,
pendingSaveName: ""
```

**Step 3: Add drawConfirmOverwrite function**

Add after `drawConfirmDelete()` (around line 832):

```javascript
function drawConfirmOverwrite() {
    drawMenuHeader("Overwrite?");

    print(4, 24, `"${editorState.overwriteTargetName}"`, 1);

    const items = [
        { action: "cancel", label: "No" },
        { action: "confirm", label: "Yes" }
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.confirmIndex,
        listArea: {
            topY: 36,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label
    });
}
```

**Step 4: Add view to draw switch**

Find the tick/draw function that switches on `editorState.view`. Add case:

```javascript
case EDITOR_VIEW.CONFIRM_OVERWRITE:
    drawConfirmOverwrite();
    break;
```

**Step 5: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add CONFIRM_OVERWRITE view"
```

---

## Task 2: Add Conflict Detection Helper

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add findPatchByName function**

Add near other helper functions (around line 590):

```javascript
/* Find patch index by name, returns -1 if not found */
function findPatchByName(name) {
    for (let i = 0; i < patchNames.length; i++) {
        if (patchNames[i] === name) {
            return i;
        }
    }
    return -1;
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add findPatchByName helper"
```

---

## Task 3: Simplify Editor Menu Items

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Update drawEditorOverview menu items**

Find `drawEditorOverview()` (around line 602). Replace the items array logic:

```javascript
function drawEditorOverview() {
    const title = editorState.isNew ? "Untitled" : "Edit Chain";
    drawMenuHeader(title);

    const items = [
        ...SLOT_TYPES.map(slot => ({ type: "slot", slot })),
        { type: "knobs", label: "Knobs" },
        { type: "action", action: "save", label: "[Save]" }
    ];

    if (!editorState.isNew) {
        /* Existing chain: add Save As */
        items.push({ type: "action", action: "save_as", label: "[Save As]" });
    }

    items.push({ type: "action", action: "cancel", label: "[Cancel]" });

    if (!editorState.isNew && !editorState.editFromActive) {
        /* Existing chain (not from active): add Delete */
        items.push({ type: "action", action: "delete", label: "[Delete]" });
    }

    // ... rest of function unchanged
```

Note: Changed title from "New Chain" to "Untitled" for new chains.

**Step 2: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): simplify editor menu - remove Rename, show Untitled"
```

---

## Task 4: Implement New Chain Save Flow

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Update action handler for new chain Save**

Find the OVERVIEW click handler (around line 1106). Replace the `editorState.isNew` block:

```javascript
if (editorState.isNew) {
    /* New chain: Save(0), Cancel(1) */
    if (actionIndex === 0) {
        /* Save - open text entry for name */
        const defaultName = generateChainName();
        openTextEntry({
            title: "Save",
            initialText: defaultName,
            onConfirm: (newName) => {
                editorState.pendingSaveName = newName;
                trySaveWithName(newName, true);
            },
            onCancel: () => {
                needsRedraw = true;
            }
        });
    } else if (actionIndex === 1) {
        exitEditor();
    }
}
```

**Step 2: Add trySaveWithName function**

Add after the save functions (around line 1590):

```javascript
/* Try to save with given name, checking for conflicts */
function trySaveWithName(name, isNewChain) {
    editorState.chain.customName = name;

    const existingIndex = findPatchByName(name);

    if (existingIndex >= 0) {
        /* Name conflict - show overwrite confirmation */
        editorState.overwriteTargetName = name;
        editorState.overwriteTargetIndex = existingIndex;
        editorState.view = EDITOR_VIEW.CONFIRM_OVERWRITE;
        editorState.confirmIndex = 0;
        needsRedraw = true;
    } else {
        /* No conflict - save directly */
        doSaveChain();
    }
}

/* Actually save the chain (after conflict resolution) */
function doSaveChain() {
    if (!editorState.chain.synth) {
        showEditorError("Select a synth first");
        return;
    }

    const chainJson = buildChainJson();

    if (editorState.overwriteTargetIndex >= 0) {
        /* Overwriting existing patch */
        host_module_set_param("update_patch", `${editorState.overwriteTargetIndex}:${chainJson}`);
    } else {
        /* Creating new patch */
        host_module_set_param("save_patch", chainJson);
    }

    refreshPatchList();
    exitEditor();
}
```

**Step 3: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): implement new chain save with conflict detection"
```

---

## Task 5: Implement Existing Chain Save Flow

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Update action handler for existing chain**

Replace the existing chain block (around line 1129):

```javascript
} else {
    /* Existing chain: Save(0), Save As(1), Cancel(2), Delete(3) */
    if (actionIndex === 0) {
        /* Save - confirm overwrite */
        const currentName = editorState.chain.customName || patchNames[editorState.editIndex] || "Unknown";
        editorState.overwriteTargetName = currentName;
        editorState.overwriteTargetIndex = editorState.editIndex;
        editorState.view = EDITOR_VIEW.CONFIRM_OVERWRITE;
        editorState.confirmIndex = 0;
    } else if (actionIndex === 1) {
        /* Save As - open text entry for name */
        const currentName = editorState.chain.customName || generateChainName();
        openTextEntry({
            title: "Save As",
            initialText: currentName,
            onConfirm: (newName) => {
                editorState.pendingSaveName = newName;
                editorState.overwriteTargetIndex = -1; /* Force create new */
                trySaveWithName(newName, false);
            },
            onCancel: () => {
                needsRedraw = true;
            }
        });
    } else if (actionIndex === 2) {
        exitEditor();
    } else if (actionIndex === 3) {
        editorState.view = EDITOR_VIEW.CONFIRM_DELETE;
        editorState.confirmIndex = 0;
    }
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): implement existing chain save/save-as flows"
```

---

## Task 6: Handle CONFIRM_OVERWRITE Actions

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add jog wheel handler for CONFIRM_OVERWRITE**

Find jog wheel handler for CONFIRM_DELETE (search for `case EDITOR_VIEW.CONFIRM_DELETE`). Add similar case:

```javascript
case EDITOR_VIEW.CONFIRM_OVERWRITE: {
    editorState.confirmIndex = editorState.confirmIndex === 0 ? 1 : 0;
    break;
}
```

**Step 2: Add click handler for CONFIRM_OVERWRITE**

Find click handler for CONFIRM_DELETE. Add case after it:

```javascript
case EDITOR_VIEW.CONFIRM_OVERWRITE: {
    if (editorState.confirmIndex === 0) {
        /* No - return to text entry with same name */
        openTextEntry({
            title: "Save As",
            initialText: editorState.pendingSaveName,
            onConfirm: (newName) => {
                editorState.pendingSaveName = newName;
                trySaveWithName(newName, editorState.isNew);
            },
            onCancel: () => {
                editorState.view = EDITOR_VIEW.OVERVIEW;
                needsRedraw = true;
            }
        });
    } else {
        /* Yes - overwrite */
        doSaveChain();
    }
    break;
}
```

**Step 3: Add back button handler for CONFIRM_OVERWRITE**

Find back button handler. Add case to return to overview:

```javascript
case EDITOR_VIEW.CONFIRM_OVERWRITE:
    editorState.view = EDITOR_VIEW.OVERVIEW;
    break;
```

**Step 4: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): handle CONFIRM_OVERWRITE user actions"
```

---

## Task 7: Clean Up Old Functions

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Remove or simplify old save functions**

The old `saveChain()`, `saveChainAsNew()`, and `updateCurrentPatch()` functions can be removed or simplified since `trySaveWithName()` and `doSaveChain()` now handle everything.

Keep `deleteChain()` as-is.

Remove:
- `saveChain()` - replaced by trySaveWithName
- `saveChainAsNew()` - replaced by trySaveWithName
- `updateCurrentPatch()` - replaced by doSaveChain with overwriteTargetIndex

**Step 2: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "refactor(chain): remove redundant save functions"
```

---

## Task 8: Test All Flows

**Manual Testing Checklist:**

1. **New chain → Save:**
   - Create new chain, select synth
   - Click Save → keyboard appears with generated name
   - Enter unique name → saves, exits

2. **New chain → Save with conflict:**
   - Create new chain, select synth
   - Click Save → keyboard appears
   - Enter existing patch name → "Overwrite?" appears
   - Click No → back to keyboard
   - Click Yes → overwrites, exits

3. **Existing chain → Save:**
   - Edit existing patch
   - Click Save → "Overwrite [name]?" appears
   - Click No → back to editor
   - Click Yes → saves, exits

4. **Existing chain → Save As:**
   - Edit existing patch
   - Click Save As → keyboard with current name
   - Enter new unique name → saves as new, exits

5. **Existing chain → Save As with conflict:**
   - Edit existing patch
   - Click Save As → keyboard
   - Enter different existing patch name → "Overwrite?" appears
   - Test Yes and No paths

6. **Delete:**
   - Edit existing patch
   - Click Delete → "Delete?" appears
   - Test Yes and No paths

**Step 1: Commit test notes**

```bash
git commit --allow-empty -m "test(chain): verify all CRUD flows manually"
```

---

## Summary

After completing all tasks:
- New chains show "Untitled" and have [Save], [Cancel]
- Existing chains have [Save], [Save As], [Cancel], [Delete]
- [Rename] removed
- All saves check for name conflicts
- Conflicts trigger "Overwrite?" confirmation
- "No" on overwrite returns to keyboard with same text
