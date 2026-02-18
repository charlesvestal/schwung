# Help Viewer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add an on-device help system accessible from Master FX Settings, with two sections (Move Everything / Move Manual), topic lists, scrollable text, and full screen-reader accessibility.

**Architecture:** Three-level hierarchy (Section → Topic → Scrollable Text) rendered using existing `drawMenuList` and `drawScrollableText` primitives. Help content lives in a separate JSON file loaded on first access. Scrollable text gets an `onActionSelected` callback for TTS. Module Store gets TTS on detail views.

**Tech Stack:** QuickJS JavaScript, shared menu_layout.mjs / scrollable_text.mjs / screen_reader.mjs utilities.

---

### Task 1: Create help_content.json

**Files:**
- Create: `src/shared/help_content.json`

**Step 1: Create the help content file**

Write `src/shared/help_content.json` with all Move Everything topics plus a Move Manual placeholder. Lines are pre-wrapped to 20 characters max. Content is condensed from MANUAL.md.

```json
{
  "sections": [
    {
      "title": "Move Everything",
      "topics": [
        {
          "title": "Shortcuts",
          "lines": [
            "All shortcuts use",
            "Shift+Vol as modifier",
            "(hold Shift, touch",
            "Volume knob).",
            "",
            "+Track 1-4: Slot ed.",
            "+Menu: Master FX",
            "+Jog Click: Overtake",
            "+Knob 8: Standalone",
            "",
            "Shift+Sample: Record",
            "Shift+Capture:",
            "  Skipback (30s save)"
          ]
        },
        {
          "title": "Connecting Tracks",
          "lines": [
            "To hear ME synths,",
            "connect Move tracks:",
            "",
            "1. Set a Move track's",
            "   MIDI Out to ch 1-4",
            "2. Set the matching",
            "   slot's Recv Ch",
            "3. Play the track -",
            "   MIDI triggers the",
            "   slot's synth",
            "",
            "Pitch bend, mod",
            "wheel, sustain, and",
            "CCs are forwarded",
            "from external MIDI."
          ]
        },
        {
          "title": "Instrument Slots",
          "lines": [
            "4 slots, each with:",
            "MIDI FX > Synth >",
            "Audio FX 1 > FX 2",
            "",
            "Navigation:",
            "Jog wheel: scroll",
            "Jog click: enter",
            "Back: go back",
            "",
            "To select a module,",
            "navigate to an empty",
            "position & click.",
            "",
            "Shift+Jog Click on",
            "a loaded module to",
            "swap it directly."
          ]
        },
        {
          "title": "Slot Settings",
          "lines": [
            "Last position in",
            "each slot:",
            "",
            "Knob 1-8: Assign",
            "  any param to knob",
            "Volume: Slot level",
            "Recv Ch: MIDI listen",
            "Fwd Ch: MIDI output",
            "",
            "Forward Channel:",
            "Auto: Remap to recv",
            "Thru: Pass original",
            "1-16: Force channel",
            "",
            "Save/Load presets",
            "for entire slot."
          ]
        },
        {
          "title": "Master FX",
          "lines": [
            "Shift+Vol+Menu opens",
            "Master FX.",
            "",
            "4 audio FX slots",
            "process mixed output",
            "of all instrument",
            "slots.",
            "",
            "Scroll to Settings",
            "for volume, Link",
            "Audio, resample,",
            "display mirror, and",
            "screen reader opts."
          ]
        },
        {
          "title": "Link Audio",
          "lines": [
            "Requires Move 2.0+.",
            "Routes Move track",
            "audio through ME FX.",
            "",
            "Setup:",
            "1. Enable Link in",
            "   Move Settings",
            "2. Toggle Link Audio",
            "   On in MFX Settings",
            "",
            "Each Move track maps",
            "to matching slot",
            "(Track 1 = Slot 1).",
            "",
            "When on, Move native",
            "Master FX bypassed.",
            "ME rebuilds the mix",
            "from per-track audio.",
            "",
            "Brief play delay",
            "from Link quantum."
          ]
        },
        {
          "title": "Recording",
          "lines": [
            "QUANTIZED SAMPLER",
            "Shift+Sample to open",
            "",
            "Source: Resample or",
            "  Move Input",
            "Duration: bars or",
            "  until stopped",
            "Starts on note or",
            "  pressing Play",
            "",
            "Saved to Samples/",
            "  Move Everything/",
            "",
            "SKIPBACK",
            "Shift+Capture saves",
            "last 30s of audio.",
            "Same source setting",
            "as sampler."
          ]
        },
        {
          "title": "Overtake Modules",
          "lines": [
            "Full-screen apps",
            "that take over Move's",
            "display and controls.",
            "",
            "Open: Shift+Vol+",
            "  Jog Click",
            "Exit: Same shortcut",
            "  (works anytime)",
            "",
            "After exiting, pad",
            "LEDs may not refresh.",
            "Change tracks or sets",
            "to restore them."
          ]
        },
        {
          "title": "Display Mirror",
          "lines": [
            "Stream Move's OLED",
            "to any browser.",
            "",
            "Setup:",
            "1. Master FX >",
            "   Settings",
            "2. Mirror Display On",
            "3. Open browser:",
            "   move.local:7681",
            "",
            "Updates at ~30 fps.",
            "Up to 8 clients.",
            "Off by default,",
            "persists on reboot."
          ]
        },
        {
          "title": "Screen Reader",
          "lines": [
            "Text-to-speech for",
            "accessibility.",
            "",
            "Toggle via Master FX",
            "> Settings > Screen",
            "Reader, or Shift+Menu",
            "when Shadow UI is",
            "disabled.",
            "",
            "Settings:",
            "Speed: 0.5x to 2x",
            "Pitch: Low to high",
            "Volume: 0 to 100"
          ]
        }
      ]
    },
    {
      "title": "Move Manual",
      "topics": [
        {
          "title": "Coming Soon",
          "lines": [
            "The Ableton Move",
            "manual will be",
            "available in a",
            "future update."
          ]
        }
      ]
    }
  ]
}
```

**Step 2: Commit**

```bash
git add src/shared/help_content.json
git commit -m "feat: add help content JSON for on-device help viewer"
```

---

### Task 2: Add onActionSelected callback to scrollable_text.mjs

**Files:**
- Modify: `src/shared/scrollable_text.mjs`

**Step 1: Update createScrollableText to accept onActionSelected**

In `createScrollableText` (line 52), add `onActionSelected` to the destructured options and store it in the returned state object:

```js
export function createScrollableText({ lines, actionLabel, visibleLines = 4, onActionSelected }) {
    return {
        lines: lines || [],
        actionLabel: actionLabel || 'OK',
        visibleLines,
        scrollOffset: 0,
        actionSelected: false,
        onActionSelected: onActionSelected || null
    };
}
```

**Step 2: Call the callback in handleScrollableTextJog**

In `handleScrollableTextJog` (line 68), after `state.actionSelected = true` on line 78, call the callback:

```js
export function handleScrollableTextJog(state, delta) {
    const maxScroll = Math.max(0, state.lines.length - state.visibleLines);

    if (delta > 0) {
        /* Scroll down */
        if (state.actionSelected) {
            return false; /* Already at bottom */
        }
        if (state.scrollOffset >= maxScroll) {
            /* At end of text, select action */
            state.actionSelected = true;
            if (state.onActionSelected) {
                state.onActionSelected(state.actionLabel);
            }
            return true;
        }
        state.scrollOffset++;
        return true;
    } else if (delta < 0) {
        /* Scroll up */
        if (state.actionSelected) {
            state.actionSelected = false;
            return true;
        }
        if (state.scrollOffset > 0) {
            state.scrollOffset--;
            return true;
        }
    }
    return false;
}
```

**Step 3: Commit**

```bash
git add src/shared/scrollable_text.mjs
git commit -m "feat: add onActionSelected callback to scrollable text"
```

---

### Task 3: Add TTS to Module Store detail view

**Files:**
- Modify: `src/modules/store/ui.js`

**Step 1: Import announce**

At the top of `src/modules/store/ui.js`, add screen_reader import alongside the existing imports (after line 34):

```js
import {
    announce
} from '/data/UserData/move-anything/shared/screen_reader.mjs';
```

**Step 2: Announce full description when entering module detail**

Find where `detailScrollState = createScrollableText(...)` is called (around line 797). After creating the state, announce the module name and full description text. Also add `onActionSelected` callback:

```js
detailScrollState = createScrollableText({
    lines: descLines,
    actionLabel,
    visibleLines: 3,
    onActionSelected: (label) => announce(label)
});
detailScrollState.moduleId = currentModule.id;

/* Announce module detail for screen reader */
announce(currentModule.name + ". " + descLines.join(". "));
```

**Step 3: Commit**

```bash
git add src/modules/store/ui.js
git commit -m "feat: add screen reader TTS to Module Store detail view"
```

---

### Task 4: Add help viewer to shadow_ui.js

**Files:**
- Modify: `src/shadow/shadow_ui.js`

This is the largest task. It adds the `[Help]` action to MFX Settings and implements the three-level help viewer (sections → topics → scrollable detail).

**Step 1: Add help state variables**

After the existing `inMasterFxSettingsMenu` variable block (around line 570), add:

```js
/* Help viewer state */
let helpContent = null;  /* Cached parsed help_content.json */
let inHelpSectionList = false;
let inHelpTopicList = false;
let inHelpTopicDetail = false;
let selectedHelpSection = 0;
let selectedHelpTopic = 0;
let helpDetailScrollState = null;
```

**Step 2: Add [Help] action to MASTER_FX_SETTINGS_ITEMS_BASE**

In `MASTER_FX_SETTINGS_ITEMS_BASE` (line 524), add a `[Help]` entry before the save/save_as/delete actions:

```js
    { key: "screen_reader_volume", label: "Voice Vol", type: "int", min: 0, max: 100, step: 5 },
    { key: "help", label: "[Help]", type: "action" },
    { key: "save", label: "[Save]", type: "action" },
```

**Step 3: Handle the help action in handleMasterFxSettingsAction**

In `handleMasterFxSettingsAction` (line 2431), add a case for `"help"` before the existing `if (key === "save")`:

```js
function handleMasterFxSettingsAction(key) {
    if (key === "help") {
        /* Load help content if not cached */
        if (!helpContent) {
            try {
                const raw = host_read_file("/data/UserData/move-anything/shared/help_content.json");
                if (raw) helpContent = JSON.parse(raw);
            } catch (e) {
                debugLog("Failed to load help content: " + e);
            }
        }
        if (helpContent && helpContent.sections && helpContent.sections.length > 0) {
            inHelpSectionList = true;
            selectedHelpSection = 0;
            needsRedraw = true;
            announce("Help, " + helpContent.sections[0].title);
        }
        return;
    }
    if (key === "save") {
```

**Step 4: Add draw functions**

Add three draw functions after `drawMasterFxSettingsMenu` (after line 6910):

```js
/* ========== Help Viewer Draw Functions ========== */

function drawHelpSectionList() {
    drawHeader("Help");

    const sections = helpContent.sections;
    drawMenuList({
        items: sections,
        selectedIndex: selectedHelpSection,
        getLabel: (item) => item.title,
        getValue: () => "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    drawFooter("Back: Settings");
}

function drawHelpTopicList() {
    const section = helpContent.sections[selectedHelpSection];
    drawHeader(truncateText(section.title, 18));

    drawMenuList({
        items: section.topics,
        selectedIndex: selectedHelpTopic,
        getLabel: (item) => item.title,
        getValue: () => "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true
    });

    drawFooter("Back: Help");
}

function drawHelpTopicDetail() {
    const section = helpContent.sections[selectedHelpSection];
    const topic = section.topics[selectedHelpTopic];
    drawHeader(truncateText(topic.title, 18));

    if (helpDetailScrollState) {
        drawScrollableText({
            state: helpDetailScrollState,
            topY: 16,
            bottomY: 43,
            actionY: 52
        });
    }
}
```

**Step 5: Wire draw dispatch into drawMasterFx**

In `drawMasterFx()` (around line 6955), add help viewer checks before the existing `inMasterPresetPicker` check:

```js
    /* Check if we're in help viewer */
    if (inHelpTopicDetail) {
        drawHelpTopicDetail();
        return;
    }
    if (inHelpTopicList) {
        drawHelpTopicList();
        return;
    }
    if (inHelpSectionList) {
        drawHelpSectionList();
        return;
    }

    /* Check if we're in preset picker mode */
    if (inMasterPresetPicker) {
```

**Step 6: Wire jog wheel navigation**

In the MASTER_FX jog handler (around line 4915 where `inMasterFxSettingsMenu` is checked), add help viewer cases before it:

```js
            } else if (inHelpTopicDetail) {
                if (helpDetailScrollState) {
                    handleScrollableTextJog(helpDetailScrollState, delta);
                }
            } else if (inHelpTopicList) {
                const topics = helpContent.sections[selectedHelpSection].topics;
                selectedHelpTopic = Math.max(0, Math.min(topics.length - 1, selectedHelpTopic + delta));
                announce(topics[selectedHelpTopic].title);
            } else if (inHelpSectionList) {
                const sections = helpContent.sections;
                selectedHelpSection = Math.max(0, Math.min(sections.length - 1, selectedHelpSection + delta));
                announce(sections[selectedHelpSection].title);
            } else if (inMasterFxSettingsMenu) {
```

**Step 7: Wire jog click**

In the MASTER_FX click handler (around line 5230 where `inMasterFxSettingsMenu` is checked), add help viewer cases before it:

```js
            } else if (inHelpTopicDetail) {
                /* Click on Back action or anywhere = go back to topic list */
                if (isActionSelected(helpDetailScrollState)) {
                    inHelpTopicDetail = false;
                    needsRedraw = true;
                    announce(helpContent.sections[selectedHelpSection].title + ", " + helpContent.sections[selectedHelpSection].topics[selectedHelpTopic].title);
                }
            } else if (inHelpTopicList) {
                /* Enter topic detail */
                const topic = helpContent.sections[selectedHelpSection].topics[selectedHelpTopic];
                helpDetailScrollState = createScrollableText({
                    lines: topic.lines,
                    actionLabel: "Back",
                    visibleLines: 4,
                    onActionSelected: (label) => announce(label)
                });
                inHelpTopicDetail = true;
                needsRedraw = true;
                announce(topic.title + ". " + topic.lines.join(". "));
            } else if (inHelpSectionList) {
                /* Enter topic list for selected section */
                const section = helpContent.sections[selectedHelpSection];
                selectedHelpTopic = 0;
                inHelpTopicList = true;
                needsRedraw = true;
                if (section.topics.length > 0) {
                    announce(section.title + ", " + section.topics[0].title);
                }
            } else if (inMasterFxSettingsMenu) {
```

**Step 8: Wire back button**

In the MASTER_FX back handler (around line 5817 where `inMasterFxSettingsMenu` is checked), add help viewer cases before it:

```js
            } else if (inHelpTopicDetail) {
                /* Back from detail to topic list */
                inHelpTopicDetail = false;
                helpDetailScrollState = null;
                needsRedraw = true;
                const section = helpContent.sections[selectedHelpSection];
                announce(section.title + ", " + section.topics[selectedHelpTopic].title);
            } else if (inHelpTopicList) {
                /* Back from topic list to section list */
                inHelpTopicList = false;
                needsRedraw = true;
                announce("Help, " + helpContent.sections[selectedHelpSection].title);
            } else if (inHelpSectionList) {
                /* Back from section list to settings */
                inHelpSectionList = false;
                needsRedraw = true;
                announce("Master FX Settings");
            } else if (inMasterFxSettingsMenu) {
```

**Step 9: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add on-device help viewer to Master FX Settings"
```

---

### Task 5: Build and deploy

**Step 1: Build**

```bash
cd /Volumes/ExtFS/charlesvestal/github/move-everything-parent/move-anything
./scripts/build.sh
```

**Step 2: Deploy**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 3: Test on device**

1. Enter Shadow Mode, open Master FX (Shift+Vol+Menu)
2. Scroll to Settings, enter it
3. Scroll to [Help], click
4. Verify section list shows "Move Everything" and "Move Manual"
5. Click "Move Everything", verify topic list
6. Click a topic, verify scrollable text
7. Back button navigates correctly through all levels
8. Test with Screen Reader enabled - verify announcements at each level
