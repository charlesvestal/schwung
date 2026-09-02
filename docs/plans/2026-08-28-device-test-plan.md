# Device test plan — everything merged since v0.12.1

Build: `schwung.tar.gz`, from `main` at `2ca9f871`. Deploy with
`./scripts/install.sh local --skip-modules --skip-confirmation`.

Covers the seven outside-contributor PRs (#210, #221, #281, #291, #292, #293,
#307) and the in-house follow-ups (#308-#313, #318, #319). Ordered by
consequence, not by convenience — stop after 3 if time is short.

---

## 1. Master FX survives a reboot — #221 + #311

The actual user-reported bug (two Discord reports): a master chain vanished on
the next boot. **Nothing here has run on hardware**, and #311 replaced the fix's
read path with a brand-new shim param (`master_fx:modules`) that has never been
served on a device.

- [ ] **Baseline.** Shift+Vol+Menu → load a Master FX slot. Reboot. Slot is
      still there after boot.
- [ ] **The reported case.** Load a master module *through movy* (it writes
      `master_fx:fxN:module` to the shim directly, which is what the mirror
      never saw). Reboot. It comes back.
- [ ] **The other drift direction.** Clear a master slot through movy, reboot —
      it stays cleared, rather than the stale mirror writing the old module back.
- [ ] **Watch for a hitch.** #311 exists because the first fix put 8+N IPC reads
      on the autosave tick. Leave the shadow UI up, untouched, for a minute —
      there should be no periodic stutter every ~5 s.

**Fails how:** a slot empty after reboot, or a slot you cleared coming back.

---

## 2. Power button, TAP as well as hold — #292

Settles the one question the review left genuinely open: whether the matched
byte is the command or the varying id. A **hold** is already known to work — it
is what the author verified.

- [ ] Load any overtake module (movy, Chord Finder).
- [ ] **Tap** the power button. Move's "Press wheel to shut down" prompt should
      appear.
- [ ] Back dismisses it without shutting down.
- [ ] The module does **not** enter Loop mode on either a tap or a hold (that
      was the cable-14 collision, CC value `0x3A` = 58 = Move's Loop CC).

**If the tap does nothing but the hold works:** my reading of the offset is
wrong and the author's comment was right — the match only catches holds. Tell
me; it's a one-line widening, not a redesign.

---

## 3. Movy's track-volume gesture — #291 + #309

Every line is gated behind a flag nothing in the shipped fleet sets, so this is
inert unless movy is loaded. #309 is the fix for the bug #291 shipped with.

- [ ] In movy, hold a track button + turn master volume. The per-track overlay
      draws **with no Shift held**.
- [ ] **Master volume does not jump** during or after the gesture. That jump is
      the bug #309 fixed — the volume-bar scanner was parsing Schwung's own OLED
      frame as if it were Move's volume overlay.
- [ ] Exit movy. The master volume knob works normally again.

---

## 4. The known-unfixed latch — #291 finding 2a

Deliberately not fixed; it needs a synthesised release. Confirming it is real on
hardware is what decides whether it's worth doing.

- [ ] Touch and hold the volume knob **first**, then press a track button
      (raising the flag mid-touch), then release both.
- [ ] Does the volume knob still respond afterwards?

**Expected to FAIL.** A "yes it still works" is the interesting answer — it
would mean the theory is wrong and nothing needs doing.

---

## 5. Chain knob CC out — #307 + #313

Needs a controller that can receive CC 102-109. Skip if the Roto-Control isn't
to hand; the author tested the happy path.

- [ ] Slot Settings → Knobs → **Knob CC Out** on. Every mapped knob dumps once,
      so the surface starts in sync.
- [ ] Turn a chain knob from Move's own encoder — the surface follows.
- [ ] Load a patch — all eight knobs report at once.
- [ ] **Then exit an overtake module** and confirm the chain's packets still
      flow. That is #313 specifically: an overtake unload must not eat them.

---

## 6. Cheap UI checks — #308, #317, #319

- [ ] **#308:** on a knob page with a string cell (a preset name), open the
      keyboard, type, confirm → you land back on **that page**. Repeat with
      Back/cancel → same page.
- [ ] **#319:** in an overtake module, LEDs paint fully with no dropped packets
      at init (this was the `uint8_t write_idx` truncating a 512-byte buffer at
      255).
- [ ] **#317:** momentary-from-knob — still untested on hardware per the notes.

---

## Not testable on device

**#210 / #310** is `tools/pytest-schwung` only. Nothing in the tree calls
`wait_for_overtake_dsp`, and the suite is not a CI gate, so merging it changed
no runtime behaviour. Verified by reading, not by running.

## Not in this build

`tests/host/Makefile` (PR #321) is a test-harness fix with no runtime effect —
the tarball is identical with or without it.
