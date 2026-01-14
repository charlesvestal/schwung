# Text Entry Component Design

A shared text entry component for Move Anything that provides an on-screen keyboard for text input.

## API

**File:** `src/shared/text_entry.mjs`

### Opening the keyboard

```javascript
import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry
} from './text_entry.mjs';

openTextEntry({
    title: "Rename Patch",
    initialText: "My Patch",
    onConfirm: (text) => { /* save the new name */ },
    onCancel: () => { /* user pressed back */ }
});
```

### Integration in calling module

```javascript
function onMidiMessage(msg) {
    if (isTextEntryActive()) {
        handleTextEntryMidi(msg);
        return;
    }
    // ... normal handling
}

function tick() {
    if (isTextEntryActive()) {
        drawTextEntry();
        return;
    }
    // ... normal tick
}
```

## Layout

128x64 display with 4 character pages:

```
Line 0 (y=2):   Title "Rename Patch"
Line 1 (y=12):  ─────────────────── (rule)
Line 2 (y=15):  a b c d e f g h     (8 chars per row)
Line 3 (y=26):  i j k l m n o p
Line 4 (y=37):  q r s t u v w x
Line 5 (y=48):  y z [..] [___] [⌫] [✓]
```

### Special buttons (bottom row)

- `[..]` - Page switcher (cycles through 4 pages)
- `[___]` - Space bar (wider button)
- `[⌫]` - Backspace
- `[✓]` - Confirm

### Character pages

- Page 0: `abcdefghijklmnopqrstuvwxyz`
- Page 1: `ABCDEFGHIJKLMNOPQRSTUVWXYZ`
- Page 2: `1234567890.-!@#$%^&*`
- Page 3: `'";:?/\<>()[]{}=-+`

## Navigation

- Jog wheel scrolls through all items (letters + 4 special buttons)
- Navigation wraps at row ends
- Selected item shown inverted (filled background)
- Jog click enters character or activates button

## Interaction Flow

### Character entry
1. User scrolls to character, presses jog click
2. Character appended to buffer
3. Switch to preview screen showing full text with cursor: `"My Patch_"`
4. After ~30 ticks (~0.5 sec), return to keyboard grid

### Special buttons
- `[..]` - Cycle page (0→1→2→3→0), stay on keyboard
- `[___]` - Append space, show preview
- `[⌫]` - Remove last character, show preview (or stay if empty)
- `[✓]` - Call `onConfirm(buffer)`, close keyboard

### Back button
Call `onCancel()`, close keyboard, discard changes

## State

```javascript
let state = {
    active: false,
    title: "",
    buffer: "",
    selectedIndex: 0,
    page: 0,
    showingPreview: false,
    previewTimeout: 0,
    onConfirm: null,
    onCancel: null
};
```

## Constraints

- Maximum buffer length: 512 characters
- Preview shows last ~18 chars with `...` prefix if text is long
- No LED usage (display only)
