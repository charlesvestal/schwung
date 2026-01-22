# Slot Preset CRUD Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable users to save, update, and delete slot presets with consistent overwrite confirmation.

**Architecture:** Modify chain UI editor menu to simplify operations and add overwrite confirmations. Remove rename (redundant with Save As + Delete).

**Tech Stack:** JavaScript (chain/ui.js), existing text_entry.mjs keyboard component

---

## Operations Summary

| Context | Menu Options |
|---------|--------------|
| New chain | [Save], [Cancel] |
| Existing patch | [Save], [Save As], [Cancel], [Delete] |

## Detailed Flows

### New Chain

**Display:** Shows as "Untitled" while editing (no name assigned yet)

**[Save]:**
1. Open keyboard with default name = signal chain abbreviations (e.g., "DX + CF + SE")
2. User confirms or edits name
3. If name conflicts with existing patch → "Overwrite [existing]?" → Yes/No
   - Yes = replace existing file
   - No = return to keyboard
4. Save and exit editor

**[Cancel]:** Exit editor, discard changes

### Existing Patch

**[Save]:**
1. Show "Overwrite [current name]?" → Yes/No
2. Yes = save and exit
3. No = return to editor (no changes)

**[Save As]:**
1. Open keyboard with current patch name pre-filled
2. User confirms or edits name
3. If name conflicts with existing patch → "Overwrite [existing]?" → Yes/No
   - Yes = replace existing file
   - No = return to keyboard
4. Save as new file and exit editor

**[Cancel]:** Exit editor, discard changes

**[Delete]:**
1. Show "Delete [name]?" → Yes/No
2. Yes = delete file, exit editor
3. No = return to editor

## Conflict Resolution

When user enters a name that matches an existing patch:
- Display: "Overwrite [existing name]?"
- Options: Yes / No
- Yes = replace the existing file with new content
- No = return to keyboard to choose different name

## Removed Features

- **[Rename]** - Removed from menu. Users can rename via Save As (new name) + Delete (old).

## UI Components

### Existing (no changes needed)
- `text_entry.mjs` - On-screen keyboard for name input
- `CONFIRM_DELETE` view - Yes/No confirmation dialog

### New/Modified
- `CONFIRM_OVERWRITE` view - "Overwrite X?" Yes/No dialog (similar to CONFIRM_DELETE)
- `generateChainName()` - Already exists, generates abbreviation-based names
- Editor menu item list - Remove [Rename], adjust indices

## Implementation Notes

1. New chain should display "Untitled" as its name in the editor header
2. Default save name uses `generateChainName()` which builds from component abbreviations
3. Overwrite confirmation reuses the same dialog pattern as delete confirmation
4. When "No" is selected on overwrite prompt, return to keyboard with same text (don't clear)
