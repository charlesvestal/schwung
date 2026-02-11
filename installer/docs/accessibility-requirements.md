# Screen Reader Accessibility Requirements

## Status: ⚠️ BLOCKING - Must implement before release

The desktop installer currently has **ZERO screen reader accessibility**. This is a critical gap compared to the bash installer, which has comprehensive quiet mode and screen reader support.

---

## Why This Matters

The original `install.sh` script was specifically designed with screen reader accessibility:
- Line 4-16: Detects `--enable-screen-reader` flag and enables quiet mode
- Line 18-30: Skips ASCII art in quiet mode
- Line 43-53: `qecho` vs `iecho` functions for selective output
- Line 642-655: Deploys Flite TTS libraries for on-device screen reader
- Line 692-700: Creates screen reader state file

**This installer must maintain that accessibility.**

---

## WCAG 2.1 AA Compliance Checklist

### 1. Perceivable

#### ✅ Text Alternatives (1.1.1)
**Current status:** ❌ FAIL
- Spinner animation has no text alternative
- Progress bars don't announce completion percentages
- Error icons have no alt text

**Required changes:**
```html
<!-- Current -->
<div class="spinner"></div>

<!-- Fixed -->
<div class="spinner" role="status" aria-live="polite" aria-label="Searching for Move device">
  <span class="sr-only">Searching for Move device, please wait...</span>
</div>
```

#### ✅ Time-based Media (1.2.*)
**Current status:** N/A - No video/audio content

#### ✅ Adaptable (1.3.*)
**Current status:** ❌ FAIL
- No semantic HTML structure
- Missing ARIA landmarks
- No heading hierarchy
- Module list not announced as list

**Required changes:**
```html
<!-- Add semantic structure -->
<header role="banner">
  <h1>Move Everything Installer</h1>
</header>

<main role="main">
  <section aria-labelledby="current-screen-title">
    <h2 id="current-screen-title">Device Discovery</h2>
    <!-- Screen content -->
  </section>
</main>

<footer role="contentinfo">
  <!-- Status/error messages -->
</footer>
```

#### ✅ Distinguishable (1.4.*)
**Current status:** ⚠️ PARTIAL
- Color contrast likely OK (dark background, light text)
- BUT: Color-only indicators (error red, success green)
- No text resize support
- No high contrast mode

**Required changes:**
- Add icons + text for status (not just color)
- Test with 200% text zoom
- Add `prefers-contrast: high` media query

### 2. Operable

#### ✅ Keyboard Accessible (2.1.*)
**Current status:** ❌ FAIL
- Tab order not defined
- No visible focus indicators
- Modal dialogs (if any) don't trap focus
- No skip links

**Required changes:**
```css
/* Add visible focus indicator */
button:focus,
input:focus {
  outline: 3px solid #4A9EFF;
  outline-offset: 2px;
}

/* Ensure tab order is logical */
/* Use tabindex="0" for interactive elements */
/* Use tabindex="-1" to remove from tab order */
```

#### ✅ Enough Time (2.2.*)
**Current status:** ⚠️ PARTIAL
- No time limits, but polling operations may confuse users

**Required changes:**
- Announce when polling starts/stops
- Provide cancel option for long operations

#### ✅ Seizures and Physical Reactions (2.3.*)
**Current status:** ✅ PASS
- No flashing content

#### ✅ Navigable (2.4.*)
**Current status:** ❌ FAIL
- No page title
- No skip links
- No focus order
- No link text (all buttons, which is OK)
- No breadcrumbs for multi-step flow

**Required changes:**
```html
<!-- Add page title -->
<title>Move Everything Installer - Device Discovery</title>

<!-- Add skip link -->
<a href="#main-content" class="skip-link">Skip to main content</a>

<!-- Add step indicator -->
<nav aria-label="Installation progress">
  <ol>
    <li aria-current="step">1. Find Device</li>
    <li>2. Authenticate</li>
    <li>3. Setup SSH</li>
    <li>4. Install Modules</li>
    <li>5. Complete</li>
  </ol>
</nav>
```

#### ✅ Input Modalities (2.5.*)
**Current status:** ⚠️ PARTIAL
- Touch targets likely OK (buttons are large)
- No drag-and-drop (good for accessibility)

### 3. Understandable

#### ✅ Readable (3.1.*)
**Current status:** ⚠️ PARTIAL
- No `lang` attribute
- Technical jargon (SSH, SCP, tarball)

**Required changes:**
```html
<html lang="en">
<!-- Add explanatory text for technical terms -->
<abbr title="Secure Shell">SSH</abbr>
```

#### ✅ Predictable (3.2.*)
**Current status:** ⚠️ PARTIAL
- Navigation mostly predictable
- BUT: Automatic screen transitions may surprise users

**Required changes:**
- Announce screen changes
- Provide "Next" button instead of auto-advancing (where applicable)

#### ✅ Input Assistance (3.3.*)
**Current status:** ❌ FAIL
- Error messages not associated with inputs
- No labels for form fields
- No input validation hints

**Required changes:**
```html
<!-- Current -->
<input type="text" placeholder="Enter code">

<!-- Fixed -->
<label for="auth-code">
  Enter 6-digit code from Move screen
  <span class="help-text" id="code-help">
    The code is displayed on your Move device
  </span>
</label>
<input
  type="text"
  id="auth-code"
  aria-describedby="code-help code-error"
  aria-invalid="false"
  maxlength="6"
  pattern="[0-9]{6}"
/>
<div id="code-error" role="alert" aria-live="assertive"></div>
```

### 4. Robust

#### ✅ Compatible (4.1.*)
**Current status:** ❌ FAIL
- No ARIA roles
- No ARIA states (aria-busy, aria-disabled, aria-hidden)
- No live regions for dynamic content

**Required changes:**
```html
<!-- Add ARIA states -->
<button aria-busy="true" aria-disabled="true">Installing...</button>

<!-- Add live regions -->
<div role="status" aria-live="polite" aria-atomic="true">
  Installation 50% complete
</div>

<!-- Hide decorative content -->
<div class="decorative-icon" aria-hidden="true"></div>
```

---

## Screen Reader Testing Checklist

### VoiceOver (macOS)
- [ ] All screens navigate correctly with Tab
- [ ] All interactive elements are announced
- [ ] All state changes are announced
- [ ] Error messages are read immediately
- [ ] Progress updates are announced
- [ ] Module selection list is navigable

### NVDA (Windows)
- [ ] Same as VoiceOver tests
- [ ] High contrast mode works
- [ ] Text resize to 200% works

### Narrator (Windows)
- [ ] Basic navigation works
- [ ] Screen reader mode toggle announced

---

## Implementation Priority

### BLOCKING (Must Fix)

1. **Semantic HTML structure**
   - Add proper headings (h1, h2)
   - Add ARIA landmarks (main, header, footer)
   - Add roles to interactive elements

2. **Keyboard navigation**
   - Define tab order
   - Add visible focus indicators
   - Trap focus in modals

3. **Screen announcements**
   - Add aria-live regions for dynamic content
   - Announce screen transitions
   - Announce progress updates

4. **Form labels**
   - Label all inputs
   - Associate errors with fields
   - Add help text

### HIGH PRIORITY

5. **Progress indicators**
   - Make progress bars accessible
   - Announce percentages
   - Provide cancel option

6. **Error handling**
   - Use role="alert" for errors
   - Focus on error message
   - Provide recovery actions

7. **Module selection**
   - Make checkboxes accessible
   - Group by category
   - Announce selection count

### MEDIUM PRIORITY

8. **High contrast mode**
   - Test with Windows High Contrast
   - Add prefers-contrast media query

9. **Text resize**
   - Test at 200% zoom
   - Ensure no content cut off

10. **Step indicator**
    - Show progress through flow
    - Mark current step

---

## Code Examples

### Accessible Screen Template

```html
<div class="screen" id="screen-discovery" role="region" aria-labelledby="discovery-title">
  <header>
    <h1 id="discovery-title">Finding Your Move</h1>
  </header>

  <main>
    <div class="content">
      <div
        class="spinner"
        role="status"
        aria-live="polite"
        aria-label="Searching for Move device"
      >
        <span class="sr-only">Searching for Move device on your network...</span>
      </div>

      <p class="status-text" aria-live="polite">
        Looking for move.local...
      </p>
    </div>
  </main>

  <footer>
    <button
      type="button"
      class="button-secondary"
      onclick="showManualEntry()"
    >
      Can't find it? Enter IP manually
    </button>
  </footer>
</div>
```

### Accessible Form

```html
<form onsubmit="submitCode(event)" role="form" aria-labelledby="code-form-title">
  <h2 id="code-form-title">Enter Authentication Code</h2>

  <div class="form-group">
    <label for="auth-code">
      6-digit code from Move screen
      <span class="required" aria-label="required">*</span>
    </label>

    <p id="code-help" class="help-text">
      The code is shown on your Move device's display
    </p>

    <input
      type="text"
      id="auth-code"
      name="code"
      required
      maxlength="6"
      pattern="[0-9]{6}"
      aria-describedby="code-help"
      aria-invalid="false"
      autocomplete="off"
    />

    <div id="code-error" role="alert" aria-live="assertive" class="error-message"></div>
  </div>

  <button type="submit" aria-busy="false">
    Submit Code
  </button>
</form>
```

### Accessible Progress

```html
<div class="progress-container">
  <label for="install-progress">Installation Progress</label>

  <progress
    id="install-progress"
    value="50"
    max="100"
    aria-valuenow="50"
    aria-valuemin="0"
    aria-valuemax="100"
    aria-valuetext="50 percent complete"
  >
    50%
  </progress>

  <div role="status" aria-live="polite" aria-atomic="true">
    Installing Braids module (2 of 5)
  </div>
</div>
```

### Accessible Module Selection

```html
<fieldset>
  <legend>Choose Modules to Install</legend>

  <div role="group" aria-labelledby="sound-generators-heading">
    <h3 id="sound-generators-heading">Sound Generators</h3>

    <div class="checkbox-group">
      <input
        type="checkbox"
        id="module-braids"
        name="modules"
        value="braids"
        checked
      />
      <label for="module-braids">
        <strong>Braids</strong> - Macro Oscillator
        <span class="description">47 synthesis algorithms</span>
      </label>
    </div>

    <div class="checkbox-group">
      <input
        type="checkbox"
        id="module-sf2"
        name="modules"
        value="sf2"
      />
      <label for="module-sf2">
        <strong>SF2</strong> - SoundFont Synth
        <span class="description">Load .sf2 files</span>
      </label>
    </div>
  </div>

  <div role="status" aria-live="polite">
    2 modules selected
  </div>
</fieldset>
```

---

## CSS for Screen Reader Support

```css
/* Screen reader only text (visually hidden but readable) */
.sr-only {
  position: absolute;
  width: 1px;
  height: 1px;
  padding: 0;
  margin: -1px;
  overflow: hidden;
  clip: rect(0, 0, 0, 0);
  white-space: nowrap;
  border-width: 0;
}

/* Skip link (hidden until focused) */
.skip-link {
  position: absolute;
  top: -40px;
  left: 0;
  background: #000;
  color: #fff;
  padding: 8px;
  text-decoration: none;
  z-index: 100;
}

.skip-link:focus {
  top: 0;
}

/* Visible focus indicator */
*:focus {
  outline: 3px solid #4A9EFF;
  outline-offset: 2px;
}

/* High contrast mode support */
@media (prefers-contrast: high) {
  .button {
    border: 2px solid currentColor;
  }

  .spinner {
    border: 3px solid currentColor;
  }
}

/* Reduced motion support */
@media (prefers-reduced-motion: reduce) {
  .spinner {
    animation: none;
  }

  * {
    transition: none !important;
  }
}
```

---

## Testing with Real Screen Readers

### VoiceOver (macOS)

```bash
# Enable VoiceOver
Cmd + F5

# Navigation
Control + Option + Right Arrow  # Next element
Control + Option + Left Arrow   # Previous element
Control + Option + Space        # Activate
```

**Test script:**
1. Launch installer with VoiceOver on
2. Navigate through all screens using only keyboard
3. Verify all content is announced
4. Verify all controls are usable
5. Test error scenarios

### NVDA (Windows)

```
# Start NVDA
Control + Alt + N

# Navigation
Tab                    # Next element
Shift + Tab            # Previous element
H                      # Next heading
Space / Enter          # Activate
```

**Test script:**
1. Same as VoiceOver test
2. Test with high contrast mode enabled
3. Test with text size at 200%

---

## Comparison with install.sh Accessibility

The bash installer has:
- ✅ Quiet mode (lines 4-16)
- ✅ Screen reader friendly output (qecho vs iecho)
- ✅ No unnecessary visual noise in quiet mode
- ✅ Concise, informative messages
- ✅ TTS deployment for on-device screen reader

The desktop installer must:
- ✅ Match or exceed bash installer accessibility
- ✅ Provide visual AND screen reader experience
- ✅ Support standard platform screen readers (VoiceOver, NVDA, Narrator)
- ✅ Follow WCAG 2.1 AA guidelines

---

## Implementation Timeline

**Week 1: Foundation**
- [ ] Add semantic HTML structure
- [ ] Add ARIA landmarks and roles
- [ ] Implement keyboard navigation
- [ ] Add focus indicators

**Week 2: Content**
- [ ] Label all form inputs
- [ ] Add live regions for dynamic content
- [ ] Implement screen announcements
- [ ] Add help text and descriptions

**Week 3: Polish**
- [ ] Test with VoiceOver
- [ ] Test with NVDA
- [ ] Fix issues found in testing
- [ ] Add high contrast mode support

**Week 4: Validation**
- [ ] Run axe DevTools
- [ ] Run WAVE browser extension
- [ ] Manual testing with real screen reader users
- [ ] Fix remaining issues

---

## Tools for Testing

### Automated

- **axe DevTools** (Chrome extension) - Comprehensive accessibility testing
- **WAVE** (Browser extension) - Visual accessibility checker
- **Lighthouse** (Chrome DevTools) - Accessibility audit

### Manual

- **VoiceOver** (macOS) - Built-in screen reader
- **NVDA** (Windows) - Free, open-source screen reader
- **Narrator** (Windows) - Built-in screen reader
- **Keyboard only** - Test without mouse

---

## Success Criteria

The installer is accessible when:
- ✅ All WCAG 2.1 Level AA criteria pass
- ✅ axe DevTools reports zero critical issues
- ✅ Complete flow possible with keyboard only
- ✅ Complete flow possible with screen reader
- ✅ All content announced correctly
- ✅ All errors announced immediately
- ✅ All state changes announced
- ✅ Works in high contrast mode
- ✅ Works at 200% text zoom

**This is MANDATORY before public release.**
