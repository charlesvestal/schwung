# Move's control scheme, as Ableton documents it

The **documented** half of Move's control scheme, compiled 2026-09-14 from
Ableton's own publications only, and reconciled against the two measured
documents in this repo:

- `docs/MOVE_COPY_GESTURES.md` — Move's copy/paste for steps, pages and clips.
- `docs/MOVE_UI_MAP.md` (branch `docs/move-ui-map`) — Move's views, LED language,
  buttons and encoders, driven over USB-MIDI.

Those two are the authority on **what the device does**. This one is the
authority on **what Ableton says it does**, and the two are not the same
document for a reason: §5 is where they disagree.

---

## 0. The headline finding, before anything else

> **The public Move manual describes roughly firmware 1.5.x. The shipping
> firmware is 2.1.0. Nine releases of control-scheme change sit between them,
> and the manual documents none of them.**

Evidence for the dating, all internal to the manual:

| Manual says | Firmware reality | Verdict |
|---|---|---|
| Sample **Slicing** via Shift + wheel click → *Slice* (§15.8) | added in **1.5.0**, 2025-06-11 | manual is ≥ 1.5.0 |
| Parameter banks via Shift + **wheel turn** on the sample icon (§15.9) | moved there in **1.5.0** (was Shift + wheel *click*) | manual is ≥ 1.5.0 |
| Setup → MIDI is a toggle between **"MIDI In Only" and "MIDI Out Only"** (§4.1.3) | **1.5.0** made input and output simultaneous and added per-track channels | manual is **stale**, or was never updated for 1.5.0 |
| Brightness has **four levels** (§2.1.11) | **1.7.0** added two lower levels (Dim, Min) → six | manual is **< 1.7.0** |
| No *Reverse* in the Sample Options menu | **1.6.0** added it | manual is **< 1.6.0** |
| The word "audio track" appears **once**, about recording *into Live* | **2.0.0** added audio tracks to Move itself | manual is **< 2.0.0** |
| No time signatures, no bounce, no Erosion, no Auto Shift | 2.1.0 / 2.1.0 / 2.0.0 / 2.0.0 | manual is **< 2.0.0** |

The PDF the manual page offers for download is published under the path
`.../move-manual/**1**/**2025-07-25**/move1-manual-en.pdf` — a "version 1"
manual, dated between 1.5.1 (2025-06-19) and 1.6.0 (2025-09-02). That matches
the internal evidence exactly. **There is no Move 2 manual.** The only
documentation of everything after 1.5.x is the release-notes page.

**Consequence for this project:** the manual is a good source for the *shape* of
the control scheme and a poor source for its *current state*. Where the manual
and a release note disagree, the release note wins. Where a release note and the
device disagree, the device wins.

### What firmware were our measurements taken on?

Neither measured document records it — a gap worth closing. But `MOVE_UI_MAP.md`
§2.7 fingerprints it accidentally: the Shift+Step 3 screen is read as
**`Max Length …` / `Quantize …` / `Step Grid …`**. **`Max Length` was added in
2.0.0b6** (2026-03-24) and shipped in 2.0.0. So the device under test is
**≥ 2.0.0**, i.e. it *has* audio tracks, time signatures may or may not be
present, and everything in the manual about the Workflow Settings menu is
already out of date on it.

That has a direct consequence for the map's own open question, *"is there a
FOURTH pad mode?"*: on a ≥ 2.0.0 device there is at minimum an **audio track**
in Note mode, whose pads and steps mean something different from a MIDI track's,
and it was never visited.

---

## 1. Sources

Ableton's own domain only. Every claim below is tagged.

| Tag | Source | URL | Date / version |
|---|---|---|---|
| **[M §n]** | Ableton Move Manual (web) | <https://www.ableton.com/en/move/manual/> | edition published **2025-07-25**; content ≈ firmware 1.5.x; footer "Copyright 2024 Ableton AG" |
| **[M-PDF]** | Same manual, PDF | <https://cdn-resources.ableton.com/resources/pdfs/move-manual/1/2025-07-25/move1-manual-en.pdf> | 2025-07-25, 10.7 MB |
| **[RN x.y.z]** | Move Release Notes | <https://www.ableton.com/en/release-notes/move-1/> | covers 1.1.2 (2024-10-08) → **2.1.0 (2026-08-25)** |
| **[BRN x.y.zbN]** | Move **Beta** Release Notes | <https://www.ableton.com/en/release-notes/move-1-beta/> | covers 1.5.0b3 → 2.1.0b3 (2026-08-12); often more precise about controls than the stable notes |

**Current shipping version: 2.1.0, released 2026-08-25** [RN 2.1.0]. The newest
beta on the beta page (2.1.0b3, 2026-08-12) predates it, so 2.1.0 is the head of
both lines as of 2026-09-14.

### Sources I could not retrieve

- **help.ableton.com** (the Knowledge Base) is behind Cloudflare and returned
  **HTTP 403** to both `curl` and the fetch tool. Three articles are relevant by
  title and are cited **unverified**, never as evidence for a gesture:
  *Audio Tracks on Move FAQ* (`/hc/en-us/articles/26962324108572`),
  *Using MIDI with Move* (`/hc/en-us/articles/14661164865308`),
  *Using Move to control Ableton Live* (`/hc/en-us/articles/14661553738524`).
- **No official MIDI implementation chart for Move exists** that I could find.
  Searched Ableton's domain; the only hits were Live-side CC articles and forum
  threads. The KB's line that *"MIDI CC is not supported on Move"* (surfaced in
  search, unverified) is about **musical CC over the MIDI ports** and says
  nothing about the internal surface CCs Schwung reads off the SPI mailbox —
  those are a private protocol Ableton does not publish at all.
- **The printed "controls overview" card** that ships in the box [M §1.1] is not
  published online in any form I could find.
- No community source was consulted for any claim in this document.

**One caution about search summaries.** A web-search summariser attributed
*"press Shift + Step 16 + pad to quantize all notes of a single drum pad"* to the
manual. That sentence **is not in the manual**; it is in [RN 1.4.0]. Every claim
below was taken from the retrieved page text, not from a search snippet.

---

## 2. The documented control scheme (Standalone Mode)

Ableton's naming differs from Schwung's. The mapping, so you can look either up:

| Ableton's name [M §20] | Schwung's name | CC (measured, not documented) |
|---|---|---|
| Note/Session toggle | **Menu** | 50 |
| Plus / Minus buttons | **Up / Down** | 55 / 54 |
| Left / Right arrow buttons | Left / Right | 62 / 63 |
| Wheel | Jog (click / turn) | 3 / 14 |
| Volume encoder | Master knob | 79 |
| Encoders (8) | Knobs 1–8 | 71–78 |
| Track buttons (4) | Track 1–4 | 43, 42, 41, 40 (**reversed**) |
| Sampling | Sample | 87 |
| Shift, Back, Play, Record, Capture, Undo, Loop, Copy, Delete, Mute | same | 49, 51, 85, 86, 52, 56, 58, 60, 119, 88 |

Ableton's manual **never mentions a button called "Menu"**. Schwung's CC 50 is
Ableton's Note/Session toggle; the measured behaviour (toggles Session ↔ Note,
shows a mode card) confirms it.

### 2.1 Hardware inventory [M §1.2]

32 velocity-sensitive pads with **polyphonic** aftertouch (channel aftertouch is
explicitly **not** supported [M §9.4]); a touch-sensitive clickable wheel; **9**
touch-sensitive encoders (8 + Volume); 16 step buttons; **20** other backlit
buttons; 128×64 OLED; stereo line in/out; mic; speaker; USB-A (MIDI host) and
USB-C.

### 2.2 Shift + Step — the shortcut layer (Standalone)

Holding Shift draws icons under the steps that have an action [M §20].

| Step | Action | Source | Added |
|---|---|---|---|
| 1 | **Set Overview** | [M §5] | ≤ 1.1.2 |
| 2 | **Setup menu** (battery, Wi-Fi, Update, Cloud, Control Live, Link, MIDI, USB-C Audio, Move Manager, Advanced, Brightness) | [M §2.1] | ≤ 1.1.2 |
| 3 | **Workflow Settings** — Quantize, Step Grid, Count-In, Autoload | [M §13] | ≤ 1.1.2 |
| 3 | … **+ Max Length** | [BRN 2.0.0b6] | 2.0.0 |
| 3 | … **+ Time Signature** | [RN 2.1.0] | 2.1.0 |
| 4 | *not documented* | — | — |
| 5 | **Tempo** (wheel sets it; Shift+wheel = fine) | [M §10.1] | ≤ 1.1.2 |
| 6 | **Metronome** on/off (the press also switches it on) | [M §10.3] | ≤ 1.1.2 |
| 7 | **Groove** — triplet-16th swing, 0–>100 % | [M §10.2] | ≤ 1.1.2 |
| 8 | **16 Pitches** layout on/off — **Drums Track Presets only** | [M §9.2], [RN 1.1.2] | 1.1.2 |
| 9 | **Keys & Scales** — In-Key/Chromatic, Octaves/**4ths**, key, scale | [M §9.1] | 4ths added 1.4.0 |
| 10 | **Full Velocity** on/off | [M §9.3] | ≤ 1.1.2 |
| 11 | **Repeat menu** — rate, and for melodic tracks Repeat / Arp Up / Arp Down / Arp Random | [M §11.6], [RN 1.1.2] | Arp added 1.1.2 |
| 12 | *not documented* | — | — |
| 13 | *not documented* | — | — |
| 14 | **New clip** in the selected track (Note Mode), and selects it | [RN 1.4.0] — **not in the manual body** | 1.4.0 |
| 15 | **Double Loop** — doubles the loop including notes and automation | [M §12.2] | ≤ 1.1.2 |
| 16 | **Quantize** the clip, by the Workflow Settings amount | [M §11.7] | ≤ 1.1.2 |
| 16 | **+ pad** — quantize all notes of **one drum pad** in the clip | [RN 1.4.0] | 1.4.0 |

Also: Shift + Step 5 **returns to the Device View** from the layout/key/scale
screens [M §9.1].

In **Session Mode from 2.0.0, Key & Scale and Full Velocity are hidden**
[BRN 2.0.0b1] — i.e. steps 9 and 10 have no action there.

### 2.3 Shift + other controls

| Gesture | Effect | Source |
|---|---|---|
| Shift + **Play** | retrigger all playing clips from the start (works in Note Mode too) | [M §17.1.5] |
| Shift + **Undo** | redo | [M §20] |
| Shift + **Capture** | clear the capture input (also cleared by start/stop transport, or a track button) | [M §14.3] |
| Shift + **Track button** | per-track menu: **Color**, **MIDI Out**, **MIDI In** | color [RN 1.4.0]; MIDI added [RN 1.5.0]; phrased as one menu in [BRN 1.5.0b1] |
| Shift + **Mute** + Track button | **solo** that track (repeat to unsolo) | [M §16.3], [RN 1.3.0] |
| Shift + **pad** (Set Overview) | Set options: **Color**, **Cloud** (Upload to Cloud / Make Local) | [M §6.1.2–6.1.3] |
| Shift + **wheel turn** | fine increments, wherever a value is being set (tempo, groove, loop length, encoder params) | [M §10.1, §12.1, §15.9.1] |
| Shift + **wheel click** on a highlighted sample | **Sample Options** menu → *Slice* (1.5.0), *Reverse* (1.6.0) | [M §15.8], [RN 1.6.0] |
| Shift + hold, then **wheel turn** on a highlighted sample/instrument | switch **parameter bank** (Drum Sampler: Main / Filter & Setup; Melodic Sampler: Main / Setup; Drift: Main / Macro from 1.7.0) | [M §7.1, §15.9], [RN 1.7.0] |
| Shift + **encoder turn** | fine increments / alternate parameter (e.g. Transpose→Detune in cents; Sample Start→zoom) | [M §15.9.1] |
| Shift + **encoder tap** | the encoder's *second* parameter (Decay→Length, Release→Playback Length, Velocity→Vol, Filter LFO→LFO Rate) | [M §15.9.1–15.9.2] |
| Shift + **arrow** while holding a step | nudge by **1 %** of a step (vs 10 % unshifted) | [M §11.4] |
| Shift (held) on the Wi-Fi password keyboard | uppercase; repeated holds cycle character pages | [M §2.1.2] |
| Shift + **paste** into an audio clip slot (Session) | bounce an **audio** clip to audio | [RN 2.1.0] |

**"Shift lock" exists and is undocumented as a gesture.** [RN 1.2.0] says
"Restarting all clips is now achieved only by holding Shift and pressing the
Play button, and **not when Shift is locked**" — so Move has a Shift latch, and
neither the manual nor the release notes say how to engage it.

### 2.4 Held-modifier combinations (no Shift)

| Hold | Then | Effect | Source |
|---|---|---|---|
| **Copy** | pad (Set Overview) | copy a Set; press the destination pad; a second press confirms an overwrite | [M §6.1.4] |
| **Copy** | step, **then release Copy**, then destination step | copy notes **and automation** from one step to another | [M §11.8] |
| **Copy** | hold start step, press end step, **release both**, press destination | copy a **range** of steps, pasted in sequence including the empty steps | [M §11.8] |
| **Copy** (tapped alone, Note Mode) | — | **duplicate the selected clip**; the duplicate becomes selected | [M §12.3] |
| **Copy** | pad (Session) | copy a clip; then press the destination pad | [M §17.1.3] |
| **Copy** (tapped again mid-gesture) | — | **clears the clipboard / cancels** | [M §11.8, §20] |
| **Delete** (alone) | — | delete the currently playing clip in the selected track | [M §12.4] |
| **Delete** | pad | delete all notes for that Drum Rack pad | [M §12.4] |
| **Delete** | step (Loop Mode) | delete all notes in that bar | [M §12.4] |
| **Delete** | pad (Set Overview) | delete the Set; second press confirms | [M §6.1.5] |
| **Delete** | pad (Session) | delete that clip, immediately, no confirm | [M §17.1.4] |
| **Delete** | encoder **tap** | delete that parameter's automation | [M §14.2.3] |
| **Mute** (alone, Note Mode) | — | mute the selected track | [M §16.4] |
| **Mute** | track button | mute / unmute that track | [M §16.4] |
| **Mute** | pad | mute / unmute that Drum Rack pad | [M §16.5] |
| **Mute** | encoder **tap** | deactivate / reactivate that parameter's automation | [M §14.2.1] |
| **Mute** (held, look at encoder LEDs) | — | automation status: unlit = none, **red** = active, **white** = deactivated | [M §14.2.2] |
| **Loop** | wheel | shorten / lengthen the clip (Shift = fine) | [M §12.1] |
| **Track button** | Volume encoder | that track's volume, in dB | [M §16.2] |
| **Track button** (held, unselected) | — | momentarily *preview* that track without selecting it | [M §20] |
| **Note/Session toggle** (held) | — | briefly preview the other mode without switching | [M §5, §20] |
| **pad** | Volume encoder | sample **gain** (Drum Rack / Melodic Sampler) | [M §16.6] |
| **pad** (Set Overview) | Volume encoder | **Set volume** — all four track volumes at once | [M §6.1, §16.7] |
| **pad** (held) | 16-Pitches pad | — see §2.5 | |

### 2.5 Holding a step — the note editor [M §11]

Multiple steps can be held at once and every one of these applies to all of them.

| Hold a step, then | Effect |
|---|---|
| Volume encoder | **velocity** |
| wheel | **note length**, 10 % of a step per detent. Cannot extend past the next step holding the same note |
| **+ / −** | transpose ± semitone; **long press** = ± octave |
| **left / right arrow** | nudge ± 10 % of a step; **Shift** = 1 %; **long press** = a full step |
| **encoder 1–8** | **per-step automation** — the steps after it light **red** to show the automation's time range, which normally runs to the next note [M §14.2.4] |
| **a pad** | add / remove that note from the step (white pad LEDs show the step's current notes) |
| **another step, briefly** | set the note length to that span [RN 1.4.0] |

A step tapped alone **removes all notes in it** [M §11.9]. In **Loop Mode** the
same gestures apply to a whole bar, and holding a bar + a pad adds that note to
**every step in the bar** [M §11.5, §11.9].

### 2.6 Loop Mode [M §12.1]

Enter with the **Loop** button; the 16 steps become the clip's bars, max 16 bars.

- Two steps pressed **simultaneously** (or hold-start then press-end) set the loop.
- A step pressed **twice quickly** sets a one-bar loop on that bar.
- Hold Loop + wheel adjusts the length; Shift for fine.
- Step LEDs: white = selected bar, track colour = in the clip, dim = empty/outside.

### 2.7 Step and pad LED language, as documented [M §9.5, §14.1, §17.1]

Ableton describes this qualitatively only — no numbers, ever.

| Steps (Note Mode) | Meaning |
|---|---|
| white | the step holds a note |
| dim, in the track colour | empty step within the bar |
| dim grey | empty clip, or a bar outside the loop |
| green | the play position |
| red (following a held step) | per-step automation time range |

| Session pads | Meaning |
|---|---|
| unlit | empty clip slot |
| track colour | an existing clip |
| white | selected empty slot |
| pulsing white | selected existing clip |
| pulsing track colour | about to stop |
| pulsing green | lined up to play |
| **flashing red** | a **bounce** is rendering into it (2.1.0) [RN 2.1.0] |

| Set Overview pads | Meaning |
|---|---|
| coloured | an existing Set |
| pulsing | the selected Set |
| unlit | empty slot |
| white | selected empty slot |

Track button LEDs: white = selected (Control Live), track colour = not selected,
**blue = soloed** [M §16.3], red = armed (Control Live), and during playback the
LED brightens toward white with the track's level [M §16.2.1].

### 2.8 Session Mode gestures [M §17]

- Pad = launch the clip; immediate if stopped, next bar if running.
- **Slide a finger vertically down a column of pads** = play that scene.
  **Sliding over an unlit column stops all playing clips**, without stopping the
  transport. *(This is the only documented gesture that is neither a press nor a
  hold.)*
- Wheel selects one of the Set's two main effects; the encoders edit it.

### 2.9 Sampling [M §15]

**Sampling** button → Sampling Mode; pad LEDs turn **pink**. A pad press starts
recording into that pad; the same pad, any other pad, or the Sampling button
stops it. Hold-to-record / release-to-stop also works. Max 240 s. Back or the
Sampling button exits without recording; switching track or to Session Mode also
exits. Sources cycle on the wheel: **Mic**, **Line in**, **Line in – Mono**
(2.1.0) [RN 2.1.0], **Resampling**, **USB-C** (1.3.0) [RN 1.3.0].

**Multi-pad recording**: while recording, press a different pad to continue the
same take onto that pad, with its own start offset.

### 2.10 Control Live Mode is a DIFFERENT control scheme [M §18]

Do not carry standalone gestures into it, and vice versa. The differences that
matter:

- **Shift + Note/Session toggle** switches between the default Session sub-mode
  and the **Session Overview** [M §18.7] — a Shift+button combination that does
  not exist in Standalone.
- **Shift + Step 14** prepares the next clip slot for recording (standalone, the
  same combo *creates* a clip).
- **Hold Record + track button** arms/unarms; a double tap on the track button
  also arms.
- **Hold Shift + pad** selects a Drum Rack pad / a slice / a clip without playing
  or launching it.
- **Hold Mute + wheel click** deactivates the selected device.
- Odd step buttons select tracks, even ones stop that track's clip; Step 15 =
  Main track, Step 16 = stop all.
- **Not available at all in Control Live Mode:** 16 Pitches, Arpeggiator,
  capturing automation, Sampling Mode, Shift+Capture clearing.

---

## 3. Firmware timeline of control-scheme changes

Only entries that **add, move or remove a control or gesture**. Bug fixes and
Core Library additions are omitted. Source: [RN], with [BRN] where it is more
precise.

| Version | Date | Control-scheme change |
|---|---|---|
| **1.1.2** | 2024-10-08 | **16 Pitches** layout (Shift+Step 8). **Arpeggiator** styles added to the Repeat menu (Shift+Step 11). **Control Live Mode** introduced (Shift+Step 2 → Setup → Control Live). |
| **1.2.0** | 2024-12-10 | 16 Pitches: **sequencing by holding a step + pressing a pad**, and **transposing with +/−**, both newly possible. Restart-all-clips narrowed to *held* Shift + Play — **explicitly not while Shift is locked**. Pressing an empty drum pad with the browser open no longer resets the browser to root. |
| **1.3.0** | 2025-02-11 | **Shift + Mute + Track = solo** (new gesture). **USB-C** added as a sampling input source. |
| **1.4.0** | 2025-03-25 | **4ths** layout added under Shift+Step 9. **Shift + Step 14 = new clip** (Note Mode). **Shift + Step 16 + pad = quantize one drum pad**. **Shift + Track button = track colour**. **Hold a step, briefly press another = set note length**. |
| **1.5.0** | 2025-06-11 | **Shift + wheel click** repurposed to **Slicing**; **parameter banks moved to Shift + wheel *turn***. **Shift + Track button** becomes a menu with **MIDI Out / MIDI In per track** beside Color. MIDI in and out now simultaneous; MIDI Sync gains an **In** option. |
| **1.6.0** | 2025-09-02 | **Reverse** added to the Sample Options menu (Shift + wheel click). Display/LED **Refresh Rate** setting added under Setup → Advanced. |
| **1.7.0** | 2025-10-14 | **Drift parameter banks** reachable by Shift + wheel turn (new surface for an existing gesture). Brightness gains **Dim** and **Min** (four levels → six). Move becomes a USB-C **MIDI device** to a host in Standalone Mode. |
| **1.8.0** | 2025-11-25 | No control change. (Auto Pan-Tremolo; sample budget 400 → **800 MB**.) |
| **2.0.0** | 2026-05-05 | **Audio tracks** — a new track type, so Note Mode's pads/steps have a *third* meaning. **Max Length** added to Workflow Settings (Shift+Step 3). **Key & Scale and Full Velocity hidden in Session Mode** [BRN 2.0.0b1] — Shift+Steps 9 and 10 lose their action there. Per-step automation extended to audio-track effects [BRN 2.0.0b3]. Holding a step over a custom parameter view shows **"Cannot automate…"** [BRN 2.0.0b6]. Link Audio. Requires Live 12.4 to open Sets. |
| **2.0.5** | 2026-06-25 | No control change. |
| **2.1.0** | 2026-08-25 | **Bounce to audio** — *Copy a clip in Session and paste it into an audio clip slot or a drum pad*; **Shift + paste** for audio→audio. **Time signatures** in Workflow Settings (Shift+Step 3), which can split one bar across **multiple step pages**. **Line in – Mono** sampling source. |

**Two changes stand out for anything that reads Move's surface:**

1. **2.0.0 added a track type.** Every "in Note Mode the steps do X" statement in
   the manual, and in our measured map, is now implicitly *"on a MIDI track"*.
2. **2.1.0 made a Copy+paste gesture mean "render audio"**, and gave Shift a
   meaning *inside* the paste. A passive copy-mirror that assumes paste is cheap
   and instantaneous is wrong on this firmware.

---

## 4. Documented, but never measured

Drive these. Each row is written to be executable. "Map" = `docs/MOVE_UI_MAP.md`,
"Copy doc" = `docs/MOVE_COPY_GESTURES.md`.

| # | Gesture to drive | Why it matters | Documented at |
|---|---|---|---|
| 1 | **Shift + Step 8 on a Drum Rack track, in Note Mode.** Then re-take the Shift lamp set. | The map records steps **4, 8, 12, 13 unlit in both modes**. Step 8 is *16 Pitches*, which the manual says exists **only on Drums Track Presets** — so the lamp is probably content-conditional, not absent. If it lights on a drum track, the map's "unlit = no action" line needs a qualifier and the Note-only lamp mystery (§Not known) partly closes. | [M §9.2] |
| 2 | **Shift + Step 10 in Note Mode on a melodic track.** | Map tried it *from Set Overview* and got nothing, and recorded "not determined". It is **Full Velocity**, a silent toggle with no screen — so "no screen change" is the expected success. Confirm via the pad velocities Move sends, or the lamp. Then repeat in **Session Mode**, where 2.0.0 removed it. | [M §9.3], [BRN 2.0.0b1] |
| 3 | **Shift + Track button (CC 49 + CC 43/42/41/40).** | Opens a per-track menu — **Color, MIDI Out, MIDI In**. The map tested *no* Shift+button combination other than Shift+Step. Schwung claims the track CCs for its own long-press/jump gestures, so this is the single most likely collision in the whole scheme. | [RN 1.4.0], [RN 1.5.0], [BRN 1.5.0b1] |
| 4 | **The entire Mute layer**: Mute alone; Mute+Track; Mute+pad; Mute + *encoder tap*; Mute held while reading the eight encoder ring colours. | `Mute (CC 88) alone, and Mute + anything` is in the map's *Not known* list, and it is four documented gestures, one of which (**Mute held ⇒ encoder LEDs report automation status: unlit / red / white**) is a free read of Move's automation state through the SysEx `3B 10` ring path the map already decoded. | [M §14.2.1–14.2.2, §16.4–16.5] |
| 5 | **Hold a step + turn encoder 1–8 = per-step automation.** Watch the steps *after* the held one turn **red**. | This is Move's native p-lock, and it is precisely what the automation-lanes work mirrors. Never driven. Also confirms a **fifth step palette value** (red) that the map's table does not have. On 2.0.0+ it also works on audio tracks. | [M §14.2.4], [BRN 2.0.0b3] |
| 6 | **Hold step + Volume (CC 79) = velocity; + wheel = length; + ± = transpose (long press = octave); + arrows = nudge (Shift = 1 %, long press = a full step).** | Five documented step-modifier axes, zero measured. The copy doc explicitly lists *"velocity, length and micro-timing"* as unknown — these gestures are how you'd generate a corpus to diff. | [M §11.1–11.4] |
| 7 | **Hold step A, then briefly press step B** (both in Note Mode). | Sets the note length across that span. **This is a two-step gesture that is not a copy**, and the copy doc's pairs model would read it as one. Drive it with Copy *not* held and confirm the notes are unchanged. | [RN 1.4.0] |
| 8 | **Shift + Step 16 + pad** on a drum track. | A **three-key** combination — the only one Ableton documents. Quantizes one pad's notes. Tests whether Move's chord detection tolerates our injection timing at all. | [RN 1.4.0] |
| 9 | **Shift + Play** (retrigger all clips) and **Shift + Undo** (redo). | Two Shift+button combos, both trivially safe, neither measured. Shift+Play is also the documented probe for whether "Shift lock" is engaged. | [M §17.1.5, §20] |
| 10 | **Loop Mode's two-step loop set**, and **a step double-tapped** for a one-bar loop. | The map measured only *hold Loop* (the peek). The loop *editing* gestures — simultaneous steps, hold-start/press-end, double tap, Loop+wheel — are all documented and all unmeasured, and "hold start, press end" is the exact shape the copy doc's pairs model collides with. | [M §12.1] |
| 11 | **Hold a pad + Volume encoder** in Note Mode (sample gain) and **in Set Overview** (Set volume). | A held pad as a modifier is a shape nothing in the map covers. In Set Overview it is also the only documented non-destructive pad gesture — useful, because a *press* there loads a set. | [M §16.6, §6.1] |
| 12 | **Hold the Note/Session toggle (CC 50)** rather than tapping it. | Documented as "briefly preview the other mode **without switching**". If true, this is a **safe, non-destructive mode probe** — strictly better than the Shift-release repaint of map §2.3, which is 16/17 and needs a fallback. Worth testing first among all of these. | [M §5, §20] |
| 13 | **Delete + encoder tap** = delete that parameter's automation. | Destructive, and the map lists Delete as measured only against clips and steps. | [M §14.2.3] |
| 14 | **The Sampling button, CC 87.** | It is **absent from the map's control table entirely** — the one named Move button with no row. The whole sampling flow (pink pads, source cycling on the wheel, hold-to-record, multi-pad) is unmapped, and Sampling Mode is a candidate **fourth pad mode**. | [M §15] |
| 15 | **A vertical finger slide down a Session pad column** = launch the scene; **over an unlit column** = stop all clips. | The only documented gesture that is neither a press nor a hold. Whether it is reproducible by injection (overlapping note-ons down a column) is unknown and is a genuinely interesting question. | [M §17.1.2] |
| 16 | **Bounce (2.1.0):** Copy a clip in Session, paste into an audio clip slot or drum pad; Shift+paste for audio→audio. | Confirms the firmware version *and* establishes whether a paste can now take seconds and show a modal with its own wheel-cancel — which changes the timing assumptions of any copy mirror. | [RN 2.1.0] |
| 17 | **An audio track**, at all. | 2.0.0 added a third track type and our device is ≥ 2.0.0. Note Mode on an audio track is unvisited territory, and the map's open question about a fourth pad mode is partly this. | [RN 2.0.0] |

---

## 5. Measured, but undocumented

Ableton documents **none** of the following. Everything here is ours, and
everything here is therefore **at risk on a firmware update** — with no release
note to warn us, because Ableton does not consider these part of the product's
surface. Ordered by how much would break.

| # | What we measured | Risk on update | Where |
|---|---|---|---|
| 1 | **The entire surface MIDI protocol**: every button CC, the **reversed** track CCs (43 = Track 1), steps as notes 16–31, pads as notes 68–99, relative encoder encoding (`01`/`7F`). | **This is a private protocol.** No published chart exists, no compatibility promise exists. It has been stable across every firmware we have run, which is evidence and not a guarantee. | Map §4, Schwung `schwung_shim.c` |
| 2 | **Step LED palette indices**: 98 = empty, 122 = has a note, 126 = playhead, 124 = selectable option, 127 = recording. | The manual describes these as *colours* (white / track colour / green), so the **indices are an implementation detail Ableton is free to renumber**. Any decoder keyed on the literal 98/122 is exposed. Note the documented **red = per-step automation range** is a *sixth* value we have not seen. | Map §3.3 |
| 3 | **SysEx `F0 00 21 1D 01 01 3B 10 …` = button/ring RGB**, 14-bit per channel — and the eight knob rings reporting their parameter value through it. | A free read path for Move's own parameter values that costs no param round-trip. Entirely undocumented, sub-command `0x10` is the only one ever seen, and the vendor is free to change it. | Map §3.2 |
| 4 | **The channel nibble as clip transport state**: ch 0 = colour, ch 9 = clip present, ch 14 = **queued**; ch 14 → ch 9 is the launch. A bare ch 9 is a refresh, not "playing". | Ableton documents the *pulsing green / pulsing track colour* LED states [M §17.1] but nothing about how they are encoded. | Map §3.3 |
| 5 | **CC 118 is a Note-mode transition flag** — emitted only on a flip, `0` meaning *not Note* (Session **or** Set Overview), never emitted spontaneously. | Undocumented and demoted even in our own map. Note Schwung's `schwung_shim.c` names CC 118 `CC_RECORD`, which the measurement does not support — worth reconciling in-house. | Map §2.2 |
| 6 | **The Shift-release repaint as a cold localisation probe** (16/17), and the rule that **the one Shift lamp that is dark names the screen you are on**. | Pure reverse-engineering of a repaint. Entirely at Ableton's mercy, and already known to go silent on some screens. | Map §2.3 |
| 7 | **Set Overview is a third pad mode, and a pad press there loads a different Set.** | Ableton documents the *behaviour* [M §6.1] but not that it makes "probe with a harmless pad press" unsafe. The hazard is ours to remember. | Map §2.1 |
| 8 | **Copy behaves as held pairs** — source, destination, source, destination — with no cancel and a self-tap consuming the pair. | See §6.1: this **contradicts** the manual. | Copy doc |
| 9 | **Double Loop repeats the existing pages** (1–2 becomes 1–2–1–2) rather than making every page identical. | The manual says "double an existing loop, including its notes and automation" [M §12.2], which is compatible but not specific enough to have predicted this. Ours is the sharper statement. | Copy doc |
| 10 | **An empty copy source is a no-op, not a clear.** | Undocumented, and load-bearing: if it cleared, a lock mirror would have to clear locks too. | Copy doc |
| 11 | **Back is a no-op inside the device carousel**; the jog **clamps** at both ends of a list. | Undocumented ergonomics; harmless, but they are what make the four-Back reset reliable. | Map §2.7, §4.2 |
| 12 | **Move writes `Song.abl` ~8–14 s after an edit.** | Not a control at all, but it is the reason every measurement of a gesture's *effect* needs a wait, and no documentation mentions it. | Copy doc, Map §5 |
| 13 | **SysEx `F0 00 21 1D 01 01 08 7F 7F F7`** on Undo and on octave changes; **`BE 86 00`** (channel 14) on Record. | Meanings unknown on both sides. | Map §3.4–3.5 |
| 14 | **Steps 4, 8, 12, 13 unlit in both modes; steps 10, 11, 14, 15, 16 lit only in Note mode.** | Ableton documents actions for 8, 10, 11, 14, 15, 16 — see §6.2. The *lamp* map is ours. | Map §4.3 |

---

## 6. Contradictions

The device is the authority. Where a contradiction may be a firmware difference,
it says which firmware each side belongs to. None of these is resolved by
assuming Ableton is right.

### 6.1 Copy: the protocol itself — **the most consequential disagreement**

| | Manual [M §11.8, §20], firmware ≈ 1.5.x | Measured (`MOVE_COPY_GESTURES.md`, 2026-09-14, firmware ≥ 2.0.0) |
|---|---|---|
| Holding | Hold Copy, press the **source**, then **release Copy**, then press the destination | Copy is **held throughout**; presses **pair up**: source, dest, source, dest |
| Multiple pastes | Implied single paste per copy; "clipboard" language | `Copy↓, s1, s2, s3, Copy↑` pasted to **s2 only** — s3 became a new source |
| Cancel | "press the Copy button again to **clear the clipboard**" | "**There is no cancel** in this gesture: to abandon a source, release Copy." Re-tapping the source **consumes** the pair instead |
| Range copy | Hold Copy, **press-and-hold** the start step, press the end step, release both, then press the destination | Not measured. The pairs model would read the same two presses as *source → paste*, i.e. as a destructive write |
| Empty source | silent | measured: **no-op**, not a clear |

**These are not obviously reconcilable, and they may both be true.** The manual's
release-then-paste and our held-pairs could be two entry paths into the same
machinery (a clipboard that a *tap* of Copy clears, and a paired mode while it is
*held*). But **the range copy is a genuine hazard either way**: our model
predicts a paste where the manual predicts a selection. Nothing in this project
should mirror a copy until that one gesture has been driven and diffed.

**What to drive, exactly:** in Note Mode on a clip with notes on steps 1 and 3
and nothing on 5–8 — (a) `Copy↓, s1, Copy↑, s5` (the manual's single copy);
(b) `Copy↓, hold s1, s3, release both, Copy↑, s5` (the manual's range copy);
(c) the same as (b) with Copy held throughout. Diff `Song.abl` after each,
allowing 14 s. If (b) pastes steps 1–3 onto 5–7, the manual is right about
ranges and the pairs model needs a hold-aware exception.

### 6.2 Shift + Steps 8 and 10: documented actions, measured as unlit

| | Ableton | Measured |
|---|---|---|
| Step 8 | **16 Pitches** toggle, "available in tracks that use **Drums** Track Presets" [M §9.2] | "*unlit — no action*", in **both** modes | 
| Step 10 | **Full Velocity** toggle [M §9.3] | "no screen change observed **from Set Overview** — not determined" |

**Most likely not a contradiction at all**, but a context difference the map's
table does not record: step 8 needs a drum track, and step 10 is a silent toggle
probed from the wrong mode. Both are cheap to settle (§4 rows 1–2). Until they
are, the map's "*unlit — no action*" for step 8 should be read as "*unlit on the
track tested*".

Genuinely open: **steps 4, 12 and 13, which Ableton documents no action for and
which measure as unlit** — agreement, and the only three where the two halves
independently say "nothing". And **steps 15 and 16**, which measure as "LED-only
change, no screen": that is exactly what a *silent action* (Double Loop,
Quantize) looks like, so the map's "not determined" and the manual's
documentation agree once you stop expecting a screen.

### 6.3 Shift + Step 3's contents

| | Says |
|---|---|
| Manual [M §13] (≈ 1.5.x) | Quantize, Step Grid, Count-In, Autoload |
| [BRN 2.0.0b6] | **+ Max Length** |
| [RN 2.1.0] | **+ Time Signature** |
| Measured (Map §2.7) | OLED read as `Max Length …` / `Quantize …` / `Step Grid …` |

The manual is simply out of date. **The measurement is the firmware fingerprint**
(§0): Max Length ⇒ ≥ 2.0.0. Note the OLED shows three rows at a time, so the
measured read is a window, not the menu.

### 6.4 Shift + Step 14

The manual documents Shift+Step 14 **only under Control Live Mode** [M §18.1],
where it *prepares a slot for recording*. [RN 1.4.0] documents it for
**Standalone Note Mode**, where it *creates a clip*. The map measured the
standalone behaviour and matched the release note.

**Not a contradiction — a documentation hole.** The manual never backfilled a
1.4.0 feature into its standalone chapters. It is the clearest single example of
why the release notes must be read alongside the manual.

### 6.5 Brightness levels

Manual: "four levels of brightness" [M §2.1.11]. [RN 1.7.0]: **Dim** and **Min**
added, so six. The manual belongs to < 1.7.0; the device is ≥ 2.0.0. Nothing
measured here, but it is a clean demonstration that the manual is a snapshot.

### 6.6 MIDI In / MIDI Out

Manual: a Setup toggle between **"MIDI In Only"** and **"MIDI Out Only"**
[M §4.1.3]. [RN 1.5.0]: simultaneous, with **per-track** send and receive
channels reached by **Shift + Track button**. The manual's paragraph describes a
firmware that has not shipped for fifteen months.

### 6.7 Naming

Not a behavioural contradiction, but it causes real confusion and is worth
stating once: **Move has no button called "Menu"**. Ableton's control reference
[M §20] lists a **Note/Session toggle**, which is Schwung's CC 50, and the
measured behaviour (toggles the mode, shows a mode card) is exactly what Ableton
describes. Likewise Schwung's **Up/Down** are Ableton's **plus and minus**
buttons, and Schwung's `CC_RECORD 118` names a CC that our own measurement says
is a Note-mode flag, not Record (Record is CC 86, `CC_REC`).

---

## 7. Confidence, and what I could not establish

**High confidence:**

- The Standalone control scheme **as of ≈ firmware 1.5.x** is completely and
  accurately captured from the manual; the manual is thorough, internally
  consistent, and its §20 control reference is a genuine per-control index.
- The firmware timeline is complete from **1.1.2 (2024-10-08) to 2.1.0
  (2026-08-25)** for stable releases, and the beta page fills in wording for
  several changes the stable notes state loosely.
- **2.1.0 is the current shipping version** as of 2026-09-14.
- The manual is **not** current, and the gap is nine releases. The dating is
  supported by six independent internal contradictions plus the PDF's own path.

**Medium confidence:**

- Which firmware our measurements were taken on. `Max Length` on the Shift+Step 3
  screen puts it at **≥ 2.0.0**, which is solid; distinguishing 2.0.x from 2.1.0
  from the map alone, I cannot. **Someone should read Setup → Update → Current
  Version and write it at the top of `MOVE_UI_MAP.md`.** Every measured claim in
  this repo is a claim about one firmware and none of them say which.
- Whether §6.1's copy disagreement is a firmware change or two entry paths into
  one mechanism. I lean towards two paths — nothing in any release note between
  1.5 and 2.1 mentions copy semantics — but I cannot show it.

**Low confidence / could not establish:**

- **Nothing from help.ableton.com.** Cloudflare returned 403 to every attempt by
  two different fetchers. Three Move articles are named in §1 and are cited as
  *existing*, never as evidence. If someone can open them in a browser, the
  *Using MIDI with Move* article is the one most likely to contain gestures this
  document is missing.
- **No official MIDI implementation chart, no control-surface specification, and
  no published account of Move's internal surface protocol.** Everything in
  `MOVE_UI_MAP.md` §3–§4 is undocumented by Ableton, which is why §5 exists.
- **"Shift lock."** [RN 1.2.0] proves it exists by saying what it *doesn't* do.
  No source I found says how to engage it, what it looks like, or whether it
  survives a mode change. It is a documented-by-omission gesture and a real gap.
- **Shift + Step 4, 12, 13.** Ableton documents no action; our measurement says
  unlit. Two silences agreeing is the weakest possible evidence that there is
  nothing there.
- **The printed controls-overview card** from the box is not online, and it is
  the one Ableton artifact that would be a single-page control reference.

**Sources I judged unreliable and did not use:**

- **Web-search summaries.** One attributed a [RN 1.4.0] gesture to the manual
  (§1). Every claim here comes from retrieved page text.
- **All community sources** — forums, videos, third-party guides. None was used,
  for the reason the brief gives: they are frequently wrong about modifier
  behaviour, and a wrong modifier claim blended into a documented one is worse
  than no claim. If a gesture below is not cited to `ableton.com`, it is not in
  this document.
