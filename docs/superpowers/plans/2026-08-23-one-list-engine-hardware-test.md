# One list engine — hardware test plan

Branch `one-list-engine`. Everything below is verified against host tests and
rendered PNGs; **none of it has run on a device.** Ordered by risk: the first
three are things host tests structurally cannot check.

---

## 1. THE HANG RISK — a modal you cannot dismiss

**Why first:** the bespoke Global Settings path raised its warnings through the
shared message overlay, whose single dismiss site sat *below* the param-pages
early-out. Over the new page chrome the overlay drew fine but **the grid ate
every button, so no press could clear it.** The fix reorders that check. It was
read, not run — this is the check that matters most.

Shift+Vol+Step 2 → **Audio**:

| do | expect |
|---|---|
| Toggle **Move→Schwung** on (with Link disabled) | "Link disabled" warning appears |
| Press **Back** | warning clears, you are back on the Audio page |
| Toggle **Schwung→Link** on | same warning, same dismiss |
| Set **Sample Src** to `Schwung Mix` | "Schwung Mix" warning, clears on Back |
| **Services** → toggle **File Browser** | info modal, clears on Back |

**If any modal will not dismiss, stop and tell me — that is a wedged UI**, and
the workaround is Shift+Vol+Jog-Click or a reboot.

Also try Jog-Click and Menu to dismiss, not just Back — I want to know if only
one button works.

---

## 2. PERSISTENCE ACROSS A REBOOT

**Why:** the old code was delta-based and side-effectful — each branch also
called `saveMasterFxChainConfig()` and set a cache var. A converted write that
drops either sets the param and loses it on reboot, **silently**. Six keys use
that shared sink.

1. Global Settings → **Audio** → turn **Move→Schwung** ON
2. Also change **Sample Src**, **Latency Comp**, **USB-C Persist**
3. Reboot the Move fully
4. Return to Global Settings → Audio

**All four must still hold their new values.** A value that reverted means a
dropped persistence call — tell me which key.

Then the same for the other save kind: change **Param View** and **Pad Typing**
(these use their own savers, not the shared sink), reboot, confirm.

---

## 3. SCREEN READER

**Why:** Global Settings is *the screen you go to to turn TTS off*, and it had
no hierarchy-editor fallback — so it now enters the page chrome unconditionally,
in list layout. **This is the first time a TTS user reaches the list layout at
all**, and its announcements have never been validated by ear.

1. Turn the screen reader ON
2. Shift+Vol+Step 2 → does it announce the page you land on?
3. Jog through all seven sections — is each announced?
4. Click into one, jog the rows — is each row announced with its value?
5. Change a value — is the new value announced?
6. Now open a **component's** params (a synth in a slot) — you should still get
   the **old hierarchy editor**, unchanged. That fork is deliberately untouched.

Tell me if any of 2–5 is silent or says the wrong thing. This is the area I am
least confident in.

Also: from the shortcut that jumps straight to Screen Reader settings — does it
land on the Screen Reader page *and announce that page's name*? (It used to land
silently, which named the wrong page out loud.)

---

## 4. GLOBAL SETTINGS NAVIGATION

Was two levels (section list → item list). Now one jog axis of seven pages.

- Jog left/right — pages: Display, Audio, Screen Reader, Set Pages, Shortcuts,
  Services, Updates
- **Shift+Jog** — the section picker
- **Audio has exactly 8 params and must fit one page** with no pagination. In
  Knobs view all 8 cells; in List view 5 rows plus a scroll arrow.
- **Updates** is a menu page: `[Check Updates]`, `[Module Store]`, and
  **`[Help...]`** — Help moved here, it is no longer a peer of the sections.
  Confirm Help still opens and its back-navigation works.
- **Skipback Len** (6 options) — hold its knob and click: an enum picker list
  should open. Back cancels without changing anything.

---

## 5. THE ONE LIST — does it look like one application?

The point of the whole branch. Visit all of these and confirm they wear the same
header band, list and footer:

- Slot settings (hold a Track button)
- Slot **Save As** (name preview)
- Master FX **Save As** — this and the previous one were literally the same code
  twice; they must be identical now
- Master FX settings
- Global Settings
- The knob-mapping editor
- An LFO edit page
- **The file picker** (dive an opaque param — a sample path)
- A component's param page

**Footer note:** BACK is now at the **right** edge everywhere (movy canon). It
used to be left on ~10 screens. Confirm that reads right in use rather than just
in a screenshot.

---

## 6. LIST vs KNOBS

Global Settings → Display → **Param View**.

- **Knobs**: components draw the 8-cell grid (unchanged)
- **List**: components draw as rows **inside the page chrome** — bank bar,
  footer hints — *not* the old hierarchy editor. That fork is what this branch
  removes.
- Toggle it while a param page is open: it should switch layout without needing
  to re-enter.

---

## 7. THE LABEL FLOOR AND THE MARQUEE

Open a module with long values — **breakbeat** is the case (`A Sample`,
`B Sample`, both `kick_01.wav`) — in **List** view.

- Every label readable; the two sample rows distinguishable
- Values truncate (`kick...`), which is intended
- **The selected row's value marquees** so you can read it in full — this is the
  only way to see a truncated value from here

---

## Known and deliberate

- Entering the Audio section no longer fires a bare "Link disabled" reminder;
  the warning rides the **write** instead.
- `[Help...]` demoted from section-peer to an entry on the Updates page.
- On surge, two rows still both read `Filter…` — there the *value* is short, so
  right-alignment sets the label budget, not the floor. Different fix, not done.

## Not done

`../schwung-catalog-site/manual.html` needs the new navigation documented.
