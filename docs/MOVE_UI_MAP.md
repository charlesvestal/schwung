# Move's own UI, measured

A map of **Ableton Move's firmware UI** — its views, its LED language, its
buttons and its encoders — written so that a *program* can drive Move and know
where it is.

Everything below was produced on 2026-09-14 by injecting USB-MIDI packets into
Move and observing two channels that cost nothing: Move's native OLED
(mirrored to `/dev/shm/schwung-display-live`) and Move's outbound LED stream
(`led_capture.log`). Every entry says **how it was measured**. Anything believed
but not run is in [Not known](#not-known), which is the point of the document
rather than an apology.

The three questions every entry tries to answer are: **what packets**, **how do
I know it worked**, and **where am I now**.

The last of those is the hard one, and §2 is the result of testing it rather
than asserting it: a claim in the first draft — "CC 118 tells you which view you
are in" — did not survive, and is demoted there with the measurements that broke
it and a scored 17-trial "get lost, then find yourself" experiment in its place.

---

## 0. Instrumentation — and one correction worth having

### Move's OLED is `schwung-display-live`, NOT `schwung-display`

`/dev/shm/schwung-display` is **Schwung's shadow-UI framebuffer**. With
`shadow_control.display_mode == 0` it holds whatever the shadow UI last drew and
never changes; its md5 was constant for the whole session.

Move's *own* screen is reconstructed by the shim from the SPI display slices into
`/dev/shm/schwung-display-live`, and only when **`shadow_control.display_mirror`
(byte 33 of `/dev/shm/schwung-control`) is 1** (`schwung-manager/shmconfig.go`
names that offset; `schwung_shim.c` gates the copy on it). It defaults to 0.

```bash
# arm
python3 - <<'EOF'
import mmap, os
f=os.open("/dev/shm/schwung-control", os.O_RDWR); m=mmap.mmap(f,256); m[33]=1
EOF
# disarm when done  (m[33]=0)
```

*Measured:* with byte 33 at 0 the buffer was frozen; setting it to 1 made the
buffer follow every screen change (turning the master knob produced a "Volume"
overlay within 1 s).

**Move only redraws when something changes.** A frozen buffer is a normal idle
screen, not a broken capture. Force a repaint with a view change.

### LED capture

```bash
touch /data/UserData/schwung/led_capture_on     # start
rm    /data/UserData/schwung/led_capture_on     # stop
```

Each line is **three consecutive raw bytes** of Move's MIDI_OUT, printed as
`st=/d1=/d2=`. The logger does **not** reassemble SysEx — an `st=0xF0` line is the
start of a message and the following lines are its continuation, three bytes at a
time, padded with zeros after `F7`. A reassembler is at
`/data/UserData/schwung/ledp.py` on the device (written for this survey).

`mode=` on each line is **Schwung's own guess** at Move's view
(`shadow_control.move_ui_mode`), not Move's statement. It is set from a Track
press and from D-Bus screen-reader text, and it was observed *stale* repeatedly
during this session (it read `mode=3`/Set Overview for minutes after leaving Set
Overview). Do not trust it; use §2.

### Helper scripts left on the device

`/data/UserData/schwung/` now also contains `lib.py` (send/ocr/led helpers),
`run.py`, `act.py`, `shot.py`, `ocr.py`, `ledp.py`, `poke.py`, `lbl.py`,
`font.json`. They are the reproduction kit for everything below; delete them
freely.

`lib.text()` segments the OLED into glyphs, hashes each glyph's own ink bounding
box, and looks the hash up in `font.json` — that is how screen text is read in
this document rather than guessed from shapes.

---

## 1. The known-state reset

**This is the anchor every other recipe is written against.**

```
0BB0337F s90 0BB03300     Back
0BB0337F s90 0BB03300     Back
0BB0337F s90 0BB03300     Back
0BB0337F s90 0BB03300     Back
0BB02B7F s110 0BB02B00    Track 1   (CC 43 — the track CCs are REVERSED)
```

**Lands in:** Note mode, track 1 selected, that track's instrument screen.

**Observable:** the OLED becomes byte-identical every time. In the set used here
the frame md5 was `704bf3fe` and the text band at rows 33–44 read
`Drum Kit` (the track-1 instrument name). The md5 is **set-specific** — check the
*text band*, not the hash, on a different set. A driver already in this state
sees exactly one LED line, `CC d1=43 d2=122`.

*Measured:* run from four different starting states — the device-preset browser
two levels deep, Set Overview, a settings screen, and the reset state itself —
all four produced the identical frame.

**Why four Backs:** one Back pops one screen level. The deepest place reached in
this survey was three levels (carousel → device → preset list). Four is one spare.
A Back with nothing to pop is a no-op (measured: Back inside the device carousel
emitted no LED and changed nothing).

To reach **Session** mode from the reset, add one `Menu` tap (§4.1) and verify
`CC 118 → 0`. Note `0` means *not Note*, not *Session* — see §2.2; from the
reset state there is nothing else it could be, which is exactly why the reset is
worth having.

---

## 2. Which view am I in?

The hardest question in this document, and the one where the first draft of this
map was wrong. Read §2.1 before using anything in §2.

### 2.1 There are at least THREE pad modes, not two

"Session vs Note" is the wrong granularity. The pads mean three different things:

| Pad mode | A pad press does | Discovered by |
|---|---|---|
| **Note** | plays the note / drum voice and selects it for step entry | pad flashes `126`, settles to `122`, step row repaints |
| **Session** | launches the clip at `92 − 8·track + slot`; an **empty slot lights nothing at all** | ch 14 (queued) → ch 9 (playing), or total silence |
| **Set Overview** | **LOADS A DIFFERENT SET** | the OLED's set name changes |

That third one matters more than its share of the map. A driver that "probes
with a harmless pad press" to find out where it is **will swap the user's set**
if it happens to be in Set Overview. It happened here, twice, during this survey.

### 2.2 `CC 118` — demoted: it is a TRANSITION signal, not a state you can read

The first draft of this document called CC 118 "the Session/Note indicator" on
the strength of four driven mode switches. That claim was too strong in two
separate ways, and both were found by testing it rather than by reasoning:

**(a) `CC 118 = 0` does not mean Session.** It means *not Note*. Entering Set
Overview (Shift + Step 1) also emits `B0 76 00`. Session and Set Overview are
indistinguishable on this CC.

**(b) Move never emits it spontaneously**, so a driver arriving cold has nothing
to read. It is emitted only when the Note-mode flag actually flips. Measured, by
counting CC 118 in the LED stream for every route driven in this survey:

| Route | Emits CC 118? | Observations |
|---|---|---|
| Menu tap that toggles the mode | **yes** — `0` or `124` | many |
| Menu tap while an overlay screen is up | **no** — the tap dismisses the screen instead | 5+ |
| Track button tap that changes the selected track | **yes** — `124` | 2 |
| Track button tap on the track already selected | **no** — emits nothing at all | 1 |
| Shift + Step 1 (Set Overview) | **yes** — `0` | 2 |
| **Back** | **no** | every Back across 4 scored runs (`BACK_EMITTED_CC118: []`) |
| Pad press / clip launch | **no** | 20+ |
| Jog turn, jog click, Play/stop, knobs, Shift | **no** | all |

So CC 118 **is** trustworthy for *following* a mode change you are watching for —
which is more than `move_ui_mode` manages — and is **useless** for answering
"where am I?" from a cold start.

**Do not use "Menu twice" to provoke a restate.** It looks like a free
self-restoring probe and it is a trap: when an overlay screen is up the first
Menu tap only dismisses it, so two taps leave you in the **opposite** mode. A
scored run of this method got 1 of 5 right and silently flipped the device in
the rest.

### 2.3 Localising cold: the Shift-release repaint

This is the method that survived testing. It is **LED-only and
non-destructive** — it presses nothing but Shift.

```
0BB0317F        hold Shift
  (~350 ms)
0BB03100        release  <-- read the step row Move sends HERE
```

On release Move restores the step row it covered with the shortcut layer.

| What Move sends on release | Conclusion |
|---|---|
| step **Note-Ons** (`90 1x ..`), values from {70, 98, 104, 122, 124, 126} | **Note mode** — a step row exists |
| **only note-offs** (`80 1x 00`) | **not Note** — Session or Set Overview |
| **neither** — no step traffic at all | **inconclusive**: an overlay screen owns Shift. Press Back and retry |

To split Session from Set Overview, take the Shift **lamp** set in the same
gesture (which step CCs went to 127) and test step 16: **step 16 dark ⇒ you are
already in Set Overview.** Every Shift shortcut's lamp is dark while you are on
the screen it opens, so the *missing* lamp names where you are — step 20 dark
means the Tempo screen is open, and so on.

**Why not the lamp set alone?** It was the first candidate and it fails: the set
varies with the track's *content*, not only with the mode. Track 1 (a drum kit)
lit 12 lamps, track 3 lit 9, and a track with nothing on it lit none of the
Note-only markers at all — so "no note markers" does not mean Session. Lamps
21 and 23 also flip with the metronome/groove *state* rather than with the view.
Scored on its own it got 3 of 5.

### 2.4 The get-lost experiment — 16 / 17

Run as a scored experiment, exactly as asked. Each trial: drive the reset, then
a **pseudo-random walk of 7–9 presses** drawn from pads, track buttons, Back,
Menu, jog turns and clicks, all four arrows, four Shift+Step screens, knobs and
the master encoder — enough that the end state is genuinely not predictable —
then answer "where am I?" using **only** §2.3, then check against an arbiter.

*Arbiter (ground truth, deliberately not the rule under test):* the OLED's
Set-Overview set-tile glyph identifies Set Overview; otherwise tap pad 92 and
read Move's own answer — channel 9/14 ⇒ Session, no LED at all ⇒ Session (an
empty slot), anything else ⇒ Note.

| Batch | Trials | Correct |
|---|---|---|
| Mixed walk, 9 presses (seeds 301–312) | 12 | **11** |
| Session-biased walk, 7 presses (seeds 501–505) | 5 | **5** |
| **Total** | **17** | **16 (94 %)** |

**The one failure was an `inconclusive`, not a wrong answer** (seed 310: walk
ended `... jogclick, back, pad92, left, down`). The Shift release produced no
step traffic at all, three Back presses did not clear it, and the rule gave up
and said so. Truth was Note mode. That is the failure mode you want: it never
answered confidently and wrongly in 17 trials.

Two honest caveats on the number:

- **The arbiter was wrong before the rule was.** Its first version classified
  "pad 92 produced no LED" as Note, which made the rule look 0-for-5 on the
  Session batch. The walk had switched the loaded set, so pad 92 no longer had a
  clip under it. The rule was right in all five; the oracle was broken. Anyone
  re-running this should expect to debug their oracle first.
- **Zero of the 12 mixed trials landed in Session**, which is why the
  Session-biased batch exists. Track buttons and Shift+Step both force you out
  of Session, and a random walk containing them almost never ends there.

### 2.5 When in doubt, do not read — RESET

For anything that matters, §1 beats every probe in this section: five presses,
lands in a byte-identical frame from every state tried, and leaves you knowing
the mode *and* the track *and* the screen. The probes above exist for the case
where resetting would destroy something you need (a queued clip, a held gesture).

### 2.6 The mode's LED refresh has two different shapes

When Move *does* change mode it repaints the whole surface, and the two repaints
are unmistakable — useful when you are watching a transition rather than
arriving cold.

**Note mode** — writes *all sixteen* step notes and *every* pad note:

```
90 10 62 … 90 1F 62      steps 16-31, d2 = 98 or 122
90 46 7B … 90 63 7B      pads 68-99, ch0, d2 = 123 / 122 / 17
B0 76 7C                 CC 118 = 124
```

**Session mode** — writes **no step notes at all**, note-OFFs every empty pad,
and writes each clip pad *twice*, on channel 0 and channel 9:

```
80 46 00 …               note-off for slots with no clip
90 54 62 / 99 54 7A      pad 84: colour on ch0, ch9 marker
B0 76 00                 CC 118 = 0
```

### 2.7 Screen-level views, by OLED signature

The OLED is **always readable and always current**, and it is the authority on
the *screen* (as against the pad mode). These screens layer *over* the pad mode;
opening one does not change what the pads do — except Set Overview, which does.

| View | OLED signature (text band) | How you got there | Back leaves you |
|---|---|---|---|
| Track / instrument screen | track name, e.g. `Schwung S2`, `Drum Kit`, with an icon row above | reset, or Track tap | (nothing to pop) |
| Mode card *(transient ~2 s)* | `Session Mode` / `Note Mode`, large, centred, boxed icon above | Menu tap | auto-dismiss |
| Device carousel | a device name, e.g. `Dynamics`, `Saturator`, one boxed icon centred + neighbour icons | Menu tap then any jog turn | Back is a **no-op** here |
| Device preset browser | three stacked rows of preset names | jog click on the carousel | back to the carousel |
| **Set Overview** | a distinctive set-tile icon glyph + the set's name (`Set 3`, `Empty Set`, `BNYX Demo 3`) | Shift + Step 1 | stays in Set Overview |
| Tempo | `Tempo` + graphic | Shift + Step 5 | Set Overview / previous |
| Metronome | `Metronome` / `On` | Shift + Step 6 | ditto |
| Groove | `Groove` + graphic | Shift + Step 7 | ditto |
| Scale | `C Chromatic` / `Major` | Shift + Step 9 | ditto |
| Note Repeat | `C Repeat` / `Rate 1/1?` | Shift + Step 11 | ditto |
| System | `Battery 1??%` / Wi-Fi line / `Update` | Shift + Step 2 | ditto |
| Clip settings | `Max Length …` / `Quantize …` / `Step Grid …` | Shift + Step 3 | ditto |
| Loop Length | `Loop Length` + graphic; **all 16 steps light 124** | hold Loop (CC 58) | on release |
| Modifier hint | `Copy...` / `Delete...` / `Mute...` | hold Copy / Delete / Mute | on release |
| Volume overlay | `Volume` + bar | turn CC 79 | times out |
| Parameter overlay | track name + param name + value + 8 mini bars | turn a knob (CC 71–78) | times out |

*Every row above was reached and captured during this session.* Where a line
reads `1??%` or `1/1?` the glyphs at those positions were not in the font table
and were left unlabelled rather than guessed.

The Set Overview row is the one worth hard-coding: its icon glyph identified Set
Overview correctly in **every** observation (7/7), including from cold, and it is
the only way to tell Set Overview from Session without touching a pad.

---

## 3. The LED language

Move paints its surface with three kinds of message.

### 3.1 Button LEDs — plain CC

`B0 <cc> <value>` on cable 0. Values observed:

| Value | Seen on |
|---|---|
| 0 | off / that action is unavailable (Left arrow at the first page) |
| 24 | dim — present and available (idle Menu, Loop, Copy, Undo, arrows) |
| 122 | selected (the track button of the selected track) |
| 124 | Play while **stopped**; a step in a selectable set |
| 126 | Play while **running** |
| 127 | held down (every button while pressed), or a Shift-layer step |

*Measured:* Play (CC 85) read 124 stopped and 126 running across four
start/stop cycles, with `pul=` (MIDI-clock pulses) advancing only in the 126
state.

### 3.2 Button RGB — SysEx `3B 10`

```
F0 00 21 1D 01 01 3B 10 <cc> <r_lo> <r_hi> <g_lo> <g_hi> <b_lo> <b_hi> F7
value = lo | (hi << 7)        0-255 per channel
```

*Measured* by decoding the byte stream and matching the CC numbers to controls:
`cc=43` (Track 1) → `(133, 32, 120)`, a purple that is the track colour;
`cc=79` (master ring) → `(221,221,221)` while touched and `(145,145,145)` idle.

**The eight knob rings report their parameter's value here.** Turning knob 1
one detent up and one down moved `cc=71` from `(32,32,32)` to `(30,30,30)`;
opening a device page reissues all eight, e.g.
`71=(142,142,142) 72=(30,30,30) 73=(72,72,72) 74=(161,161,161)`. This is a
**read path for Move's own parameter values that costs nothing** — no param
channel, no polling.

`cc=78` was also seen as `(0,0,255)`, pure blue, so these are true colours and
not only greyscale.

### 3.3 Steps and pads — Note On, velocity = palette index

`90 <note> <index>` on cable 0, note 16–31 for steps and 68–99 for pads. A
note-off means dark.

#### Steps — **decoded, and the previous guess was wrong**

| `d2` | Meaning |
|---|---|
| **98** | step is **empty** |
| **122** | step **holds a note** |
| **126** | **playhead** is on this step |
| 124 | step is an option in a transient chooser (Shift layer, Loop Length) |
| 127 | seen on the step under the playhead while **recording** |

*Measured:* in Note mode on the track whose clip (`Song.abl`, track index 1)
contains notes at `startTime` 2.75, 3.5 and 3.75, Move lit **exactly** steps 27,
30 and 31 at 122 and every other step at 98. Steps are 0.25 quarters apart, so
2.75 → index 11 → note 27, 3.5 → 14 → 30, 3.75 → 15 → 31. Then, with the
transport running, the playhead walked 16…31 at `d2=126` and each step it *left*
was restored to 122 or 98 according to the same rule.

*(The prior belief recorded in the brief was "122 vs 102". The empty value is
**98**, not 102.)*

#### Session pads

`note = 92 - 8*track + slot`, tracks 0–3 left to right, slots 0–7 top to bottom.

*Measured:* `Song.abl` had clips only at track 0 / slot 0 and track 1 / slot 0;
the Session repaint lit exactly pads **92** and **84** and note-off'd every other
pad.

The channel nibble is the clip's transport state:

| Message | Meaning |
|---|---|
| `90 <pad> <colour>` | the clip's colour (index; 98 and 112 for the two clips here) |
| `99 <pad> 7A` (ch 9) | clip present — emitted both at rest and while playing |
| `9E <pad> 7A` (ch 14) | **queued** |

*Measured* by launching pad 84 with the transport running: Move emitted
`90 54 7E` + `9E 54 7A` immediately (white, queued), then at the launch boundary
`90 54 62` + `99 54 7A` (clip colour, playing).

**Channel 9 does not mean "playing".** It was emitted identically on a plain
repaint with the transport stopped. Channel 14 → channel 9 is the transition that
means "it started". A bare channel 9 is a refresh.

#### Note-view pads

| `d2` | Meaning |
|---|---|
| 123 | normal / in layout |
| 17 | dim |
| 122 | the **currently selected note**, and its octave twins |
| 126 | sounding right now |

*Measured:* tapping pad 80 moved 122 from pads 81/84 onto pad 80 and flashed 126
for the duration of the note; during playback pads 75/78 and 91/94 flashed 126 on
the beats the clip's notes fall on.

### 3.4 Channel as animation

`BE 86 00` (CC 86, **channel 14**) was emitted on each Record tap alongside the
plain `B0 86 7F`. This is the animation channel the brief describes for Record.
The exact animation vocabulary was **not** decoded.

### 3.5 An undo acknowledgement

`F0 00 21 1D 01 01 08 7F 7F F7` was emitted on Undo (CC 56) and again on each
Octave up/down. Command `08`; meaning **not known**.

---

## 4. Controls

`0BB0<cc><7F>` press, `0BB0<cc><00>` release. Press and release must be sent on
**one connection** with a timed gap — two `tap.py` invocations are a *hold*,
because each carries ~0.35 s of its own.

### 4.1 Buttons

| Control | CC | Alone | Held | Destructive |
|---|---|---|---|---|
| Menu | 50 | **toggles Session ↔ Note**; shows the mode card ~2 s | not tested | no |
| Back | 51 | pops one screen level; no-op in the device carousel | not tested | no |
| Shift | 49 | modifier; lights the step shortcut layer (§4.3) | — | no |
| Track 1–4 | **43, 42, 41, 40** | select that track **and enter Note mode** | ≈1 s previews the track, reverts on release *(1 observation)* | no |
| Play | 85 | start / stop. LED 124↔126 | not tested | no |
| Record | 86 | toggle; the second tap **started the transport and recorded** | not tested | **yes — writes notes into the clip** |
| Capture | 52 | ran a step animation across the 16 steps; effect not confirmed | not tested | **assume yes** |
| Undo | 56 | performs undo; screen changed to a `Notes` card | not tested | reverses the last edit |
| Loop | 58 | not tested alone | opens **Loop Length**: all 16 steps light 124, the current length marked with a ch-9 message on its step | selecting a length changes the clip |
| Copy | 60 | not tested alone | `Copy...` card; all knob rings go dark | pairs with steps/pages — see `docs/MOVE_COPY_GESTURES.md` |
| Delete | 119 | **deletes the selected clip** *(measured previously, not re-run here)* | `Delete...` card; **Delete + step clears that step** (step LED 122 → 98) | **yes** |
| Mute | 88 | not tested alone | `Mute...` card | not determined |
| Up | 55 | Note view, melodic track: **octave up** (`Octave up` card) | not tested | no |
| Down | 54 | **octave down** | not tested | no |
| Left | 62 | **previous clip page**; its own LED is **0 when there is no previous page**, 24 when there is | not tested | no |
| Right | 63 | **next clip page**; same LED rule | not tested | no |
| Jog click | 3 | opens the highlighted carousel item (device → its preset browser) | not tested | no |

### 4.2 Encoders

| Control | Encoding | Effect |
|---|---|---|
| Jog wheel | CC 14, relative. `01` = +1, `7F` = −1 | moves the carousel / list selection. **Clamps at both ends** — from the last device carousel entry, four further `+1` packets changed nothing. |
| Knobs 1–8 | CC 71–78, relative, same encoding | edit the parameter under that knob; opens a parameter overlay naming the track and the parameter and drawing eight mini bars. The ring RGB (§3.2) is the value. |
| Master | CC 79, relative | volume; `Volume` overlay; ring goes `(221,221,221)` while adjusting, `(145,145,145)` after |
| Knob touch | notes 0–9 | injected touch on/off produced **no observable change** |

### 4.3 Shift + Step

Hold Shift, tap the step, release Shift:

```
0BB0317F s80 0990<nn>77 s120 0980<nn>00 s80 0BB03100     nn = 0x10 + (step-1)
```

| Step | Note | Opens |
|---|---|---|
| 1 | 16 | **Set Overview** |
| 2 | 17 | **System** — battery %, Wi-Fi, Update |
| 3 | 18 | **Clip settings** — Max Length / Quantize / Step Grid |
| 4 | 19 | *unlit — no action* |
| 5 | 20 | **Tempo** |
| 6 | 21 | **Metronome** (showed `On`) |
| 7 | 22 | **Groove** |
| 8 | 23 | *unlit — no action* |
| 9 | 24 | **Scale** — `C Chromatic` / `Major` |
| 10 | 25 | no screen change observed from Set Overview — **not determined** |
| 11 | 26 | **Note Repeat** — rate |
| 12, 13 | 27, 28 | *unlit — no action* |
| 14 | 29 | **New clip** — `New clip selected`. **Creates a clip** (an empty row 1 appeared in `Song.abl`) |
| 15 | 30 | LED-only change, no screen. Schwung's `CLAUDE.md` calls this Double Loop; **not verified here** |
| 16 | 31 | LED-only change, no screen — **not determined** |

Steps 4, 8, 12 and 13 are unlit in *both* modes; steps 10, 11, 14, 15, 16 are lit
only in Note mode.

### 4.4 Pads and steps, per mode

| Surface | Session | Note | Set Overview |
|---|---|---|---|
| Pads 68–99 | **launch the clip** at `92 − 8·track + slot`; queued (ch 14) then playing (ch 9); **also starts the transport**. An empty slot emits **nothing at all** | play the note and make it the **selected** note for step entry (pad goes to `d2=122`) | **LOADS THE SET under that pad.** Measured twice, unintentionally both times |
| Steps 16–31 | tap produced **no observable effect** | **toggle a note** at that step for the selected pad note | not tested |

**The Set Overview column is the reason a driver must localise before it
probes.** A pad press is the obvious "harmless" way to ask Move what mode it is
in, and in one of the three modes it swaps the user's document.

*Measured (Note view):* tapping step 16 lit it at 122 and, 14 s later, `Song.abl`
carried a new note `(0.0, 60)` on that track. `Delete` + step 16 returned it to 98
and the note was gone.

---

## 5. Timing you must respect

| Thing | Measured |
|---|---|
| `Song.abl` settles | ~8–14 s after an edit. A 14 s wait was sufficient every time; a diff taken sooner shows nothing and reads as "the gesture did nothing". |
| Mode card | auto-dismisses after ≈2 s back to the previous screen |
| Press/release | must be one connection with an explicit gap. `seq.py`/`lib.send` do this; two `tap.py` calls do not. |
| LED settle | a view change's LED burst completes in <100 ms; 700 ms after the release was always enough |

---

## 6. Coverage

What was determined for every control. "not tested" is an honest cell.

| Control | Session | Note | Menu/carousel | Settings screens |
|---|---|---|---|---|
| Menu 50 | toggles to Note | toggles to Session | not tested | not tested |
| Back 51 | not tested | no-op at top level | **no-op in carousel**; pops in preset browser | pops to Set Overview |
| Shift 49 | lights 7 steps | lights 12 steps | not tested | not tested |
| Track 40–43 | not tested | switches track + stays in Note; long press previews | not tested | not tested |
| Play 85 | start/stop | start/stop | not tested | not tested |
| Record 86 | not tested | toggle, starts transport + records | not tested | not tested |
| Capture 52 | not tested | step animation, effect unconfirmed | not tested | not tested |
| Undo 56 | not tested | performs undo | not tested | not tested |
| Loop 58 | not tested | Loop Length chooser | not tested | not tested |
| Copy 60 | not tested | `Copy...` card | not tested | not tested |
| Delete 119 | deletes selected clip *(prior measurement)* | + step clears the step | not tested | not tested |
| Mute 88 | not tested | `Mute...` card | not tested | not tested |
| Up 55 / Down 54 | not tested | octave up/down | not tested | not tested |
| Left 62 / Right 63 | not tested | clip page ±1, LED says if available | not tested | not tested |
| Jog click 3 | not tested | opens carousel item | enters preset browser | not tested |
| Jog turn 14 | moves carousel | moves carousel | moves selection, clamps | not tested |
| Knobs 71–78 | not tested | parameter edit + overlay | not tested | not tested |
| Master 79 | volume overlay | volume overlay | not tested | not tested |
| Knob touch 0–9 | not tested | no effect observed | not tested | not tested |
| Steps 16–31 | no effect observed | toggle note | not tested | not tested |
| Pads 68–99 | launch clip | play + select note | not tested | not tested |

**Set Overview is absent from the columns above** on purpose: apart from "a pad
loads that set" and "Back stays inside it", nothing in that mode was mapped.

**Not covered at all:** every one of the 32 pads individually (only the
layout-wide colour rule was measured, not the per-pad note mapping in Note view);
the 8 knobs individually (only knob 1 was turned); Shift with any button other
than Menu and the steps; any two-button combination other than Delete+step and
Shift+step; anything on an audio or MIDI track that is not a Schwung slot; the
sampling flow; Wi-Fi/Update; and the whole of Set Overview beyond its title
screen.

---

## Not known

Untested. A driver must not assume any of it.

- **Does a jog/knob delta greater than 1 move more than one item?** Only `01`
  and `7F` were proved to register. A `03` and a `7D` were sent, but from a
  position where the list clamped, so they prove nothing. Likewise **whether
  repeated identical packets coalesce** — every repeat test was run against a
  clamped two-item list.
- **What CC 118 physically is.** Only its correlation with the Note-mode flag
  is measured.
- **Whether there is a FOURTH pad mode.** Three were found (Note, Session, Set
  Overview) and nothing systematic was done to look for more — the sampling
  flow, a MIDI track and an audio track were never visited.
- **Whether any state Move emits, anywhere, encodes the pad mode statically.**
  Four candidates were tested and the results are in §2.2–2.3; no exhaustive
  search of the LED surface was made. There may be a CC or a SysEx that simply
  answers the question, and it was not found.
- **Why the Shift-release repaint goes silent on some screens** (the one
  inconclusive trial, and the Tempo/Groove screens). Something about those
  screens claims Shift; which ones, and whether more than three Backs always
  clears it, is unmeasured — three Backs was enough in 16 of 17 trials.
- **What the Note-only Shift lamps (25, 26, 29, 30, 31) each require.** Their
  presence varies with the track's content as well as the mode, which is what
  made the lamp-set rule fail; the exact condition per lamp was not isolated.
- **Whether the Shift probe is safe on every screen.** It was screen-neutral
  wherever it was checked, but `disturbed: true` appeared on frames where a
  transient card happened to be expiring, so a genuine Shift side effect on some
  screen would not have been distinguished from that.
- **The animation vocabulary in the channel nibble.** `BE 86 00` on Record was
  seen; nothing was decoded beyond "channel 14 appears on Record".
- **SysEx command `08`** (`F0 00 21 1D 01 01 08 7F 7F F7`) — emitted on Undo and
  on octave changes. Purpose unknown.
- **Whether `3B 10` has siblings.** Every SysEx captured used sub-command `0x10`
  (button RGB). Pads and steps were always plain Note On, so a pad RGB path, if
  one exists, was never provoked.
- **Capture (CC 52).** It made the steps animate and I did not establish what it
  captured, or whether it is destructive. Treat as destructive.
- **Mute (CC 88) alone**, and Mute + anything.
- **Shift + Step 10, 15, 16.** All three produced LED traffic and no screen.
- **What the device carousel actually contains.** It held exactly two entries
  (`Dynamics`, `Saturator`) on the track tested, both audio effects — the track's
  instrument was *not* in it. Whether the instrument is a third entry elsewhere,
  or on a different row, was not established.
- **The 4-segment bar at OLED rows 58–60.** It changes with the clip's page count
  and the current page, but no controlled experiment pinned the encoding.
- **The Note-view pad → note-number map.** Only the *colour* classes were
  measured (123 / 17 / 122 / 126); which pad plays which pitch under which scale
  was not.
- **Long-press semantics generally.** Track hold previewing and reverting is a
  **single observation**; no threshold was measured and no other button was held
  long enough to distinguish tap from hold.
- **Anything in Session mode below the pad layer** — steps did nothing
  observable, but "nothing observable on the OLED and LED stream" is not the same
  as "nothing happened".
- **What Set Overview's own surface does** beyond "a pad loads that set": the
  jog, the steps, the arrows and Back were never mapped there, and it is the one
  mode where a wrong press swaps the user's document.
- **Whether the reset survives a modal that claims Back.** Four Backs cleared
  every state reached here; a confirmation dialog that swallows Back would defeat
  it and none was encountered.

---

## Appendix: machine-readable

Only entries that were actually run. `packets` is the `seq.py` / `lib.send`
form: hex USB-MIDI packets and `sNNN` sleeps in milliseconds, on **one**
connection.

```json
[
  {"action":"reset",              "packets":"0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB02B7F s110 0BB02B00",
   "observe":"OLED identical from any state; text band rows 33-44 = track-1 instrument name; LED 'B0 2B 7A'",
   "state":"note_mode, track 1"},

  {"action":"toggle_session_note","packets":"0BB0327F s110 0BB03200",
   "observe":"LED 'B0 76 00' => NOT note (session OR set overview), 'B0 76 7C' => note; OLED card ~2s",
   "state":"session_mode | note_mode",
   "warning":"with an overlay screen up this tap DISMISSES the screen and emits no CC118 — it does not toggle. Never assume two taps are a no-op."},

  {"action":"localise_cold",       "packets":"0BB0317F s350 0BB03100",
   "note":"read the step row Move sends on the RELEASE; see docs section 2.3",
   "observe":"step Note-Ons (90 1x ..) => NOTE mode; only note-offs (80 1x 00) => not note; neither => inconclusive, press Back and retry. To split session from set overview, check whether step CC 16 went to 127 while Shift was held: dark => set overview.",
   "state":"unchanged (non-destructive)",
   "measured":"16 of 17 scored trials; the miss was a self-reported inconclusive"},

  {"action":"select_track",       "packets":"0BB0<2B|2A|29|28>7F s110 0BB0<..>00",
   "note":"CC 43,42,41,40 = tracks 1,2,3,4 (REVERSED)",
   "observe":"LED 'B0 <cc> 7A' on the new track, previous track drops; full note-mode repaint",
   "state":"note_mode, that track"},

  {"action":"open_device_carousel","packets":"0BB0327F s110 0BB03200 s400 0BB00E01",
   "observe":"OLED text band = a device name, boxed icon centred",
   "state":"device_carousel",
   "warning":"the Menu tap here ALSO toggles session/note"},

  {"action":"carousel_next",      "packets":"0BB00E01", "observe":"OLED device name changes; clamps at the end", "state":"device_carousel"},
  {"action":"carousel_prev",      "packets":"0BB00E7F", "observe":"same, clamps at the start",                  "state":"device_carousel"},
  {"action":"open_preset_browser","packets":"0BB0037F s110 0BB00300", "observe":"OLED shows three stacked preset rows", "state":"preset_browser"},
  {"action":"back",               "packets":"0BB0337F s90 0BB03300",  "observe":"'B0 33 7F' then 'B0 33 18'; one screen level pops; NO-OP in device_carousel", "state":"one level up"},

  {"action":"transport_toggle",   "packets":"0BB0557F s110 0BB05500",
   "observe":"LED CC 85 -> 126 running, 124 stopped; 'pul=' advances only while 126", "state":"unchanged"},

  {"action":"launch_clip",        "packets":"0990<pad>77 s110 0980<pad>00",
   "note":"SESSION MODE ONLY; pad = 92 - 8*track + slot. The SAME packets in Set Overview LOAD A DIFFERENT SET — localise before you press a pad.",
   "observe":"'90 <pad> 7E' + '9E <pad> 7A' (queued), then '90 <pad> <colour>' + '99 <pad> 7A' (playing); also starts the transport",
   "state":"session_mode"},

  {"action":"toggle_step_note",   "packets":"0990<step>77 s110 0980<step>00",
   "note":"note mode only; step note = 0x10 + index; writes the currently selected pad note",
   "observe":"step LED 98 -> 122; Song.abl gains the note after ~14 s",
   "state":"note_mode", "destructive":true},

  {"action":"clear_step",         "packets":"0BB0777F s150 0990<step>77 s110 0980<step>00 s150 0BB07700",
   "observe":"OLED 'Delete...' while held; step LED 122 -> 98",
   "state":"note_mode", "destructive":true},

  {"action":"select_pad_note",    "packets":"0990<pad>77 s110 0980<pad>00",
   "note":"note mode",
   "observe":"pad flashes 126 then settles to 122; the previous 122 pads return to 123",
   "state":"note_mode"},

  {"action":"octave_up",          "packets":"0BB0377F s110 0BB03700", "observe":"OLED card 'Octave up'; the 122 pads move", "state":"note_mode"},
  {"action":"octave_down",        "packets":"0BB0367F s110 0BB03600", "observe":"OLED card 'Octave down'",                  "state":"note_mode"},

  {"action":"page_next",          "packets":"0BB03F7F s110 0BB03F00",
   "observe":"steps repaint for the new page; 'B0 3E 18' appears (Left becomes available)", "state":"note_mode"},
  {"action":"page_prev",          "packets":"0BB03E7F s110 0BB03E00",
   "observe":"steps repaint; 'B0 3E 00' when there is no earlier page",                     "state":"note_mode"},

  {"action":"shift_layer_peek",   "packets":"0BB0317F s400 0BB03100",
   "observe":"steps 16,17,18,20,21,22,24 -> 127 in session; + 25,26,29,30,31 in note",
   "state":"unchanged"},

  {"action":"shift_step",         "packets":"0BB0317F s80 0990<nn>77 s120 0980<nn>00 s80 0BB03100",
   "map":{"16":"Set Overview","17":"System","18":"Clip settings","20":"Tempo","21":"Metronome","22":"Groove","24":"Scale","26":"Note Repeat","29":"New clip (CREATES a clip)"},
   "observe":"OLED text band matches the map entry",
   "state":"that settings screen"},

  {"action":"loop_length_peek",   "packets":"0BB03A7F s500 0BB03A00",
   "observe":"OLED 'Loop Length'; all 16 steps -> 124, the current length also gets a ch-9 message",
   "state":"unchanged on release"},

  {"action":"knob_turn",          "packets":"0BB0<47..4E><01|7F>",
   "observe":"parameter overlay on the OLED; 'F0 00 21 1D 01 01 3B 10 <cc> ...' re-reports the ring value",
   "state":"unchanged"},

  {"action":"read_knob_values",   "packets":"0BB0327F s110 0BB03200",
   "note":"any view change reissues all eight ring colours",
   "observe":"eight 'SYS 3B 10 n=71..78 rgb=(v,v,v)' messages; v is the parameter value 0-255",
   "state":"toggled session/note — pick a cheaper refresh if that matters"}
]
```

### Packet cheat sheet

```
CC press   0BB0<cc_hex>7F        CC release  0BB0<cc_hex>00
Note on    0990<nn_hex>77        Note off    0980<nn_hex>00

Shift 49=0x31   Menu 50=0x32   Back 51=0x33   Jog click 3=0x03
Jog turn 14=0x0E (rel: 01=+1, 7F=-1)
Tracks 1-4 = 43,42,41,40 = 0x2B,0x2A,0x29,0x28      (REVERSED)
Up 55=0x37  Down 54=0x36  Left 62=0x3E  Right 63=0x3F
Play 85=0x55  Record 86=0x56  Capture 52=0x34  Undo 56=0x38
Loop 58=0x3A  Copy 60=0x3C  Delete 119=0x77  Mute 88=0x58
Knobs 71-78 = 0x47..0x4E   Master 79=0x4F
Steps 1-16 = notes 16-31 = 0x10..0x1F
Pads = notes 68-99 = 0x44..0x63
```
