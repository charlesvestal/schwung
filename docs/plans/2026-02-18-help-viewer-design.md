# Help Viewer Design

## Overview

Add an on-device help system to Move Everything, accessible from Master FX Settings. Displays condensed manual content in a two-level hierarchy: topic list → scrollable text. All content is screen-reader accessible.

## Entry Point

A `[Help]` action item at the bottom of the Master FX Settings menu (in `MASTER_FX_SETTINGS_ITEMS_BASE`).

## Architecture

### Files

| File | Change |
|------|--------|
| `src/shared/help_content.json` | **New** - Help topics with pre-wrapped lines |
| `src/shared/scrollable_text.mjs` | Announce action button label on selection |
| `src/shadow/shadow_ui.js` | Add `[Help]` to MFX settings, help viewer states |
| `src/modules/store/ui.js` | Add TTS announcement when entering module detail |

### Navigation

```
MFX Settings → [Help] → Section List → Topic List → Scrollable Text
```

Three boolean flags in shadow_ui.js (matching existing pattern):
- `inHelpSectionList` - viewing section list ("Move Everything" / "Move Manual")
- `inHelpTopicList` - viewing topic list for selected section
- `inHelpTopicDetail` - reading a topic

### Help Content Format

`src/shared/help_content.json`:
```json
{
  "sections": [
    {
      "title": "Move Everything",
      "topics": [
        {
          "title": "Shortcuts",
          "lines": [
            "Shift+Vol is the modifier",
            "for all shortcuts.",
            "",
            "+Track 1-4: Open slot",
            "+Menu: Master FX",
            ...
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
            "The Ableton Move manual",
            "will be available in a",
            "future update."
          ]
        }
      ]
    }
  ]
}
```

Lines are pre-wrapped to 20 characters for the 128x64 display. No runtime word-wrapping needed.

### Move Everything Topics

1. Shortcuts
2. Connecting Tracks
3. Instrument Slots
4. Slot Settings
5. Master FX
6. Link Audio
7. Recording
8. Overtake Modules
9. Display Mirror
10. Screen Reader

### Move Manual Topics

Placeholder for now. Will be ported from the Ableton Move manual later.

### UI Rendering

- **Section list**: `drawMenuList` with header "Help", footer "Back: Settings"
- **Topic list**: `drawMenuList` with header = section title, footer "Back: Help"
- **Topic detail**: `drawMenuHeader` for topic title + `drawScrollableText` with `actionLabel: "Back"`

Content loaded once via `host_read_file()` on first `[Help]` click, then cached.

### Accessibility

- **Enter section list**: `announce("Help, " + sectionTitle)`
- **Scroll section list**: `announce(sectionTitle)`
- **Enter topic list**: `announce(sectionTitle + ", " + topicTitle)`
- **Scroll topic list**: `announce(topicTitle)`
- **Enter topic detail**: `announce(topicTitle + ". " + allLinesJoined)`
- **Action button selected**: `announce("Back")`

Module Store also gets TTS when entering module detail views (same `announce` pattern, applied where `createScrollableText` is used).

### Scrollable Text Change

Add announcement when action button becomes selected in `handleScrollableTextJog`: accept an optional `onActionSelected` callback, called with the action label string. Keeps scrollable_text.mjs decoupled from screen_reader.
