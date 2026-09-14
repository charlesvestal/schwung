# Move's own UI, measured

A map of **Ableton Move's firmware UI** — its views, its LED language, its
buttons and its encoders — written so that a *program* can drive Move and know
where it is.

**Firmware: Move 2.1.0**, read on the device from **Setup → Update → Current
Version** (Shift+Step 2 → Update → Current Version → `Move 2.1.0 / installed`).
Every claim here is on that version, and everything was measured on **MIDI
tracks** — audio tracks (added in 2.0.0) were never visited.

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
| Capture | 52 | **nothing at all** (§7.1). The step animation once attributed to it was the recording already running | not tested | no, on this evidence |
| Undo | 56 | performs undo; screen changed to a `Notes` card | not tested | reverses the last edit |
| Loop | 58 | not tested alone | opens **Loop Length**: all 16 steps light 124, the current length marked with a ch-9 message on its step | selecting a length changes the clip |
| Copy | 60 | **duplicates the clip** (`Clip duplicated`, §7.1) | `Copy...` card; all knob rings go dark; **Copy + pad copies that pad's sample** | **yes** — and it arms a clipboard that persists, §7.5 |
| Delete | 119 | **deletes the clip** — `Clip deleted`, measured directly in §7.1 | `Delete...` card; **Delete + step clears that step** (step LED 122 → 98) | **yes** |
| Mute | 88 | **mutes the track**; **Shift+Mute solos** (§7.2) | `Mute...` card; **the eight knob rings go dark** — they do NOT report automation | no |
| Up | 55 | Note view, **melodic** track: **octave up**. **Nothing on a Drum Kit, nothing in Session or Set Overview** (§7.1) | not tested | no |
| Down | 54 | **octave down**, same conditions | not tested | no |
| Sampling | **87** | **no response of any kind**, tap or hold (§7.1) | — | — |
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

What was determined for every control, per pad mode. "not tested" is an honest
cell; a dash means *tested and nothing happened*. All on **MIDI tracks**,
firmware 2.1.0.

| Control | Note | Session | Set Overview | Under a settings overlay |
|---|---|---|---|---|
| Menu 50 | → Session | → Note | leaves it → Session | dismisses |
| Back 51 | – | – | card → tile screen | dismisses |
| Shift 49 | 12 step lamps | 7 lamps | 7 lamps | not tested |
| Track 40–43 | switch track | → Note on it | leaves it → Note | not tested |
| Play 85 | start/stop | start/stop | start/stop | not tested |
| Record 86 | transport + record | not tested | not tested | not tested |
| Capture 52 | **–** | not tested | not tested | not tested |
| **Sampling 87** | **–** | not tested | not tested | not tested |
| Undo 56 | undo | undo | undo | not tested |
| Loop 58 | Loop Length | – | – | not tested |
| Copy 60 | duplicates clip | – | – | not tested |
| Delete 119 | deletes clip | not tested | not tested | not tested |
| Mute 88 | mutes track | – | – | not tested |
| Up 55 / Down 54 | octave ± (melodic only; **– on a Drum Kit**) | **–** | **–** | not tested |
| Left 62 / Right 63 | clip page ∓1 | **–** | **–** | not tested |
| Jog click 3 | opens a screen | clip launch settings | – | not tested |
| Jog turn 14 | device carousel | device carousel | not tested | **edits the overlay** |
| Knobs 71–78 | device parameter | not tested | not tested | **unchanged — still the device parameter** |
| Master 79 | volume overlay | volume overlay | not tested | not tested |
| Knob touch 0–9 | **large unexplained LED burst** | not tested | not tested | not tested |
| Steps 16–31 | toggle note | – | **LOAD A SET** | not tested |
| Pads 68–99 | play + select | launch clip | **LOAD A SET** | still play |

Modifier combinations are in §7.4, the Shift+Step layer in §7.3, Shift+button in
§7.2, and overlay semantics in §7.6.

**Closed since the last draft:** Capture alone, Delete alone, Copy alone, Mute
alone, Sampling (CC 87), the arrows in all three modes, Shift+every button,
Shift+Step 8/10, the whole modifier×control matrix, steps in Set Overview, the
overlay semantics for all four control classes, and Move's native per-step
automation.

**Still not tested, deliberately, with the reason:**

| Untested | Why |
|---|---|
| Record / Capture / Delete in Session and Set Overview | each is destructive and the Note-mode result already establishes what they do; Set Overview additionally risks a set switch |
| The 32 pads individually in Note mode | the pad→pitch map needs one clip write per pad and an 8–14 s file settle each, ~8 minutes of device time for a map that `Song.abl` would give directly |
| Knobs 2–7 individually | knobs 1 and 8 behaved identically (a parameter overlay + a ring value); the class looks uniform and was sampled, not enumerated |
| Audio tracks, and any MIDI track that is not a Schwung slot | none existed in the set under test |
| The sampling flow, Wi-Fi, Update | Update was opened only as far as Current Version; running one would reflash the user's instrument |
| Set Overview's jog, arrows and Back beyond the tile screen | every probe there can change the loaded set |

---

## 7. Breadth sweep — the control scheme

Firmware **Move 2.1.0**, read from **Setup → Update → Current Version** (`Move
2.1.0 / installed`). Every measurement in this document is on that version. The
published manual describes ~1.5.x, so where the two disagree **the device wins**
and nothing below has been adjusted to match documentation.

**Track types matter.** Move 2.0.0 added audio tracks, so there are three kinds.
Everything below was measured on **MIDI tracks** — track 1 a Drum Kit, track 2
a melodic instrument — unless a row says otherwise. Audio tracks were not
visited at all.

### 7.1 Every named button, alone, in each pad mode

Driven from the §1 reset, one button per trial, mode re-established each time.

| Button | CC | Note mode | Session | Set Overview |
|---|---|---|---|---|
| Menu | 50 | → Session + mode card | → Note + mode card | **leaves Set Overview** → Session |
| Back | 51 | nothing | nothing | Set Overview card → the set tile screen |
| Jog click | 3 | opens a screen (`Back`/device row) | opens **clip launch settings** | nothing |
| Undo | 56 | performs undo | performs undo | performs undo |
| Loop | 58 | **Loop Length** chooser, 16 steps → 124 | nothing | nothing |
| **Copy** | 60 | **duplicates the clip** (`Clip duplicated`) | nothing visible | nothing visible |
| **Mute** | 88 | **mutes the track** (`<track> muted`) | nothing visible | nothing visible |
| Play | 85 | start / stop | start / stop | start / stop |
| Up / Down | 55 / 54 | **octave ±1 on a melodic track; nothing on a Drum Kit** | **nothing** | **nothing** |
| Left / Right | 62 / 63 | clip page ∓1; LED 0 when unavailable | **nothing** | **nothing** |
| Track 1–4 | 43,42,41,40 | switch track (no repaint if already selected) | → Note on that track | **leaves Set Overview** → Note |
| Shift | 49 | lights 12 step lamps | lights 7 | lights 7 |
| **Capture** | 52 | **nothing at all** | not tested | not tested |
| **Record** | 86 | **starts transport AND records** | not tested | not tested |
| **Delete** | 119 | **deletes the clip** (`Clip deleted`) | not tested | not tested |
| **Sampling** | **87** | **no response of any kind**, tap or hold | not tested | not tested |

Three cells that were guesses in the last draft and are now measured: **Capture
alone does nothing** (the step animation previously attributed to it was the
recording that was already running), **Delete alone deletes the clip** (measured
here, not inherited), and **a lone Copy tap duplicates the clip** — which is the
incident Schwung's own `CLAUDE.md` records as "an unclaimed Copy duplicated a
clip mid-gesture", now reproduced deliberately.

**CC 87 produced nothing** on injection — no LED, no OLED, tapped or held. Either
Move's Sampling button is not CC 87, or an injected CC 87 is filtered somewhere.
Not resolved.

### 7.2 Shift + every button

Driven as one gesture: Shift down, button down, button up, Shift up. Note mode.

| Combination | Effect |
|---|---|
| Shift + Menu | same as Menu (toggles mode); Shift is ignored |
| Shift + Loop | **Loop Length** — same as holding Loop |
| Shift + Copy | **duplicates the clip** — same as Copy |
| Shift + Delete | **deletes the clip** — same as Delete |
| **Shift + Mute** | **SOLO** (`<track> soloed`) — Mute alone is mute, Shift+Mute is solo |
| **Shift + Track N** | **a per-track menu: `MIDI Out Ch1` / `MIDI In Auto` / `Color`** |
| **Shift + knob 1–8** | **a second parameter bank** — knob 1 became `Transpose`, knob 8 `Grain Size` |
| Shift + Play | starts the transport |
| Shift + Record | starts transport + recording |
| Shift + master | `Volume` — same as master alone |
| Shift + Back / Jog click / Capture / Undo / arrows / jog turn | **nothing** |

**Shift + Track is the likeliest collision with Schwung**, which claims the track
CCs for its own gestures: behind that claim sits Move's per-track MIDI routing
menu.

### 7.3 Shift + Step — the complete layer, and why two entries looked absent

| Step | Note | Opens | Context |
|---|---|---|---|
| 1 | 16 | Set Overview | any |
| 2 | 17 | System (Battery / Wi-Fi / Update / …) | any |
| 3 | 18 | Clip settings — `Max Length` / `Quantize` / `Step Grid` | any |
| 4 | 19 | *nothing — lamp dark in every mode tested* | — |
| 5 | 20 | Tempo | any |
| 6 | 21 | Metronome (`On` / `Off`) | any |
| 7 | 22 | Groove | any |
| **8** | **23** | **16 Pitches — a toggle, reports `On` / `Off`** | **Drum Kit track only** |
| 9 | 24 | Scale — `C Chromatic` / `Major` | any |
| **10** | **25** | **Full Velocity — a toggle, reports `On` / `Off`** | **Drum Kit track only** |
| 11 | 26 | Note Repeat (rate) | Note mode |
| 12, 13 | 27, 28 | *nothing — lamp dark* | — |
| 14 | 29 | New clip — **creates a clip** | Note mode |
| 15 | 30 | LED-only change, no screen (believed Double Loop; **still unverified**) | Note mode |
| 16 | 31 | LED-only change, no screen — **still undetermined** | Note mode |

**Steps 8 and 10 are context-dependent, not absent.** The last draft recorded
them as unlit with no action; both were probed from Set Overview. Re-run from
the reset (track 1, a Drum Kit) both fire and both are toggles that print their
new state. That is the general lesson for this layer: **a dark Shift lamp means
"not available *here*", never "does nothing".** Both were toggled twice during
this survey and confirmed back at `Off`.

### 7.4 Held modifiers × control classes

Modifier held down, one control operated, modifier released. Note mode, track 1.

| Combination | Effect |
|---|---|
| **Copy + pad** | **copies that drum pad's sample** (`Pad Sample copied`) |
| Copy + step | arms that step as the copy **source** (see §7.5); on an already-armed clipboard it printed `Clipboard cleared` |
| Copy + track button | nothing |
| Copy + jog / knob / Play | nothing — the knob keeps its normal parameter |
| **Delete + step** | **clears that step** (LED 122 → 98) |
| **Delete alone** | deletes the clip |
| **Mute held** | `Mute...` card; **all eight knob-ring LEDs go DARK** |
| Loop held | Loop Length chooser; all 16 steps → 124, current length marked on channel 9 |
| **Hold step + knob 1–8** | **per-step parameter automation — see §7.5** |
| **Hold step + jog** | **Note Length** |

**The Mute layer does not report automation on the rings.** Holding Mute sets
CCs 71–78 to 0 — the rings go out. Measured directly from the CC stream; no
`3B 10` ring colours are sent while Mute is held.

### 7.5 The two gestures that matter most

#### Move's native per-step automation is real

**Hold a step that carries a note, then turn an encoder.** The held step lights
`d2 = 127`, and each of knobs 1–8 addresses a *per-step* parameter — the OLED
titles it `<track name> K1` … `K8`. **The jog, in the same gesture, edits `Note
Length`.** Releasing the step leaves the step's content value unchanged (122);
the p-lock is not visible in the step row.

*Measured* on a MIDI track, holding a step with a note, turning knobs 1, 2, 3, 4
and 8 and then the jog. A first attempt found nothing because the held step was
**empty** — the gesture needs a note under it.

**This is the collision the brief anticipated**: Schwung has built its own
p-lock on the same physical gesture, and Move already owns it.

#### The Copy contradiction, settled — the manual model wins on 2.1.0

Three experiments, read back from the **step LED row** rather than `Song.abl`
(`122` = has a note, `98` = empty, already verified against the file, and the
LEDs update immediately instead of 8–14 s later):

| Experiment | Gesture | Result |
|---|---|---|
| **M** | Copy↓, source step, **Copy↑**, destination step | destination went to **122** — it **pasted** |
| **C** | Copy↓, source, Copy↑, **Copy↓ Copy↑**, destination | destination went to **122** — the second press did **NOT** cancel |
| **R** | Copy↓, source **held**, second step tapped, release all | second step stayed **98** — **nothing was written** |

So, on 2.1.0:

- **You do NOT have to hold Copy for the paste.** The armed source survives the
  release. This is the manual's model, and `docs/MOVE_COPY_GESTURES.md`'s "pairs
  while held" model is *incomplete* rather than wrong — both describe the same
  Copy-held sequences correctly, and only this case separates them.
- **There is no cancel.** A second Copy press leaves the source armed. The
  manual is wrong here on this firmware.
- **Range copy does not write.** Holding the source and tapping a second step is
  a *selection*, not a paste — which the pairs model would have got backwards,
  exactly as the brief feared.

**And the armed source persists indefinitely, across screens and across the
reset.** It was still armed several minutes and dozens of presses later, when a
`hold step 1` in an unrelated experiment printed `Notes pasted` and wrote a note
nobody asked for. That is almost certainly the stuck `Paste...` screen this
whole survey opened on. **A driver that arms Copy must clear it** — `Copy + an
empty step` printed `Clipboard cleared`.

### 7.6 Overlays: what changes meaning while one is up

Tested on Tempo, Groove, Scale, Metronome and Clip settings by operating each
control class with the overlay on screen.

| Control | While a Shift+Step overlay is up |
|---|---|
| **Jog turn** | **CAPTURED** — edits the value, or moves the `<` cursor between rows. On the two-state Metronome screen a jog turn dismissed it |
| **Knobs 1–8** | **NOT captured** — still the track's device parameters, and their own parameter overlay *replaces* the settings screen |
| **Pads** | **NOT captured** — still play (the Metronome screen showed `Pad Sample` when a pad was struck) |
| **Menu** | dismisses, and also shows a mode card |
| **Back** | dismisses |

So exactly one control class changes meaning under an overlay: **the jog**. That
is a small, checkable rule, and it is the answer to "does a control silently mean
something else here".

Transient overlays that time out on their own: the mode card (~2 s), the volume
overlay, the knob parameter overlay, and the `… copied` / `… deleted` /
`… muted` toasts. Modifier hint cards (`Copy...`, `Delete...`, `Mute...`) and the
Loop Length chooser last exactly as long as the button is held.

### 7.7 Set Overview is a hazard surface

Two ways to change the loaded set by accident, both measured:

- **Any of the 32 pads loads the set under it.** Known from the last pass.
- **The 16 STEP buttons also load sets.** New: steps 1, 5 and 16 each produced a
  ~100-event repaint and the set name on screen changed (`Set 3` → `BNYX Demo
  1`). Whether they address the same grid as the pads was not established.

Everything else tested in Set Overview (Loop, Copy, Mute, Play, the four arrows,
jog click) did nothing. Track buttons and Menu leave it.

**For a driver: establish the pad mode before injecting a pad OR a step**, and
if you have lost track, run the §1 reset first. Both of this survey's set
switches came from "harmless" probes.

### 7.8 The held-Menu mode preview — real, and a worse probe

Holding Menu **does** flip the mode for the duration of the hold and flip it back
on release: `CC 118 → 0` on the press and `→ 124` on the release, with a full
surface repaint each way. So it is non-destructive in the sense that you end
where you started — the documented "preview" exists.

As a localisation probe it was scored head-to-head against §2.3 over six random
walks:

| Probe | Score |
|---|---|
| Held Menu | **4 / 6** |
| Shift release repaint (§2.3) | **6 / 6** |

Both its misses were **Set Overview**, where the press emits no CC 118 at all, so
the probe is blind to exactly the mode where a wrong guess is dangerous. §2.3
remains the recommended method.

### 7.9 Knob touch (notes 0–9) — unresolved

Injecting knob-touch note-on/note-off produced **large LED bursts (120–137
events)** including `3B 10` RGB writes to the track-button CCs 41–43 with vivid
colours. The effect was not isolated from the surrounding state and **is not
understood**; it is in Not known rather than described here.

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
- **Shift + Step 15 and 16.** Both produce LED traffic and no screen. Step 15 is
  believed to be Double Loop (Schwung's `CLAUDE.md` says so) and that was **not**
  verified here; step 16 is unidentified.
- **Shift + Step 4, 12, 13.** Their lamps were dark in every mode tested, but
  steps 8 and 10 looked exactly like that until they were tried from a Drum Kit
  track — so "dark" is evidence of context, not of absence. There is some
  context in which these three may do something, and it was not found.
- **Knob touch (notes 0–9).** Injecting a touch produced 120–137 LED events
  including `3B 10` RGB writes to the track-button CCs with vivid colours. The
  effect was never isolated from surrounding state and is not understood.
- **CC 87 (Sampling).** Nothing at all on injection, tapped or held. Either it is
  not the Sampling button or an injected CC 87 is filtered before Move sees it;
  not distinguished.
- **The per-step automation parameters themselves.** `<track> K1`…`K8` were
  observed as titles; which parameter each knob addresses, what range, and how
  the value is stored were not measured — nor was the *red* step colour the
  manual describes, which never appeared (the held step reads `d2 = 127`).
- **Whether Set Overview's steps and pads address the same grid.** Both load
  sets; the mapping between them was not established, and every probe costs a
  set switch.
- **Menu's behaviour on an overlay is inconsistent between two runs.** In one
  batch the first Menu tap after an overlay emitted no CC 118 (dismiss only); in
  the overlay sweep it dismissed *and* showed a mode card. The two runs differed
  in which overlay was actually on screen at the moment of the press, and that
  was not controlled.
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
  **single observation**; no threshold was measured. The held-Menu preview (§7.8)
  is the one hold whose mechanism is now measured.
- **Audio tracks.** Added in Move 2.0.0 and never visited — so every "in Note
  mode the steps do X" claim in this document is implicitly *on a MIDI track*.
- **Clip paste onto an audio track or a drum pad.** The user states this bounces
  to audio in 2.1.0 rather than pasting instantly. **Recorded as the user's
  statement, not as a measurement** — it was deliberately not tested, and nothing
  in this document should be read as saying clip paste is generally a bounce.
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
 {
  "action": "reset",
  "packets": "0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB0337F s90 0BB03300 s90 0BB02B7F s110 0BB02B00",
  "observe": "OLED identical from any state; text band rows 33-44 = track-1 instrument name; LED 'B0 2B 7A'",
  "state": "note_mode, track 1"
 },
 {
  "action": "toggle_session_note",
  "packets": "0BB0327F s110 0BB03200",
  "observe": "LED 'B0 76 00' => NOT note (session OR set overview), 'B0 76 7C' => note; OLED card ~2s",
  "state": "session_mode | note_mode",
  "warning": "with an overlay screen up this tap DISMISSES the screen and emits no CC118 \u2014 it does not toggle. Never assume two taps are a no-op."
 },
 {
  "action": "localise_cold",
  "packets": "0BB0317F s350 0BB03100",
  "note": "read the step row Move sends on the RELEASE; see docs section 2.3",
  "observe": "step Note-Ons (90 1x ..) => NOTE mode; only note-offs (80 1x 00) => not note; neither => inconclusive, press Back and retry. To split session from set overview, check whether step CC 16 went to 127 while Shift was held: dark => set overview.",
  "state": "unchanged (non-destructive)",
  "measured": "16 of 17 scored trials; the miss was a self-reported inconclusive"
 },
 {
  "action": "select_track",
  "packets": "0BB0<2B|2A|29|28>7F s110 0BB0<..>00",
  "note": "CC 43,42,41,40 = tracks 1,2,3,4 (REVERSED)",
  "observe": "LED 'B0 <cc> 7A' on the new track, previous track drops; full note-mode repaint",
  "state": "note_mode, that track"
 },
 {
  "action": "open_device_carousel",
  "packets": "0BB0327F s110 0BB03200 s400 0BB00E01",
  "observe": "OLED text band = a device name, boxed icon centred",
  "state": "device_carousel",
  "warning": "the Menu tap here ALSO toggles session/note"
 },
 {
  "action": "carousel_next",
  "packets": "0BB00E01",
  "observe": "OLED device name changes; clamps at the end",
  "state": "device_carousel"
 },
 {
  "action": "carousel_prev",
  "packets": "0BB00E7F",
  "observe": "same, clamps at the start",
  "state": "device_carousel"
 },
 {
  "action": "open_preset_browser",
  "packets": "0BB0037F s110 0BB00300",
  "observe": "OLED shows three stacked preset rows",
  "state": "preset_browser"
 },
 {
  "action": "back",
  "packets": "0BB0337F s90 0BB03300",
  "observe": "'B0 33 7F' then 'B0 33 18'; one screen level pops; NO-OP in device_carousel",
  "state": "one level up"
 },
 {
  "action": "transport_toggle",
  "packets": "0BB0557F s110 0BB05500",
  "observe": "LED CC 85 -> 126 running, 124 stopped; 'pul=' advances only while 126",
  "state": "unchanged"
 },
 {
  "action": "launch_clip",
  "packets": "0990<pad>77 s110 0980<pad>00",
  "note": "SESSION MODE ONLY; pad = 92 - 8*track + slot. The SAME packets in Set Overview LOAD A DIFFERENT SET \u2014 localise before you press a pad.",
  "observe": "'90 <pad> 7E' + '9E <pad> 7A' (queued), then '90 <pad> <colour>' + '99 <pad> 7A' (playing); also starts the transport",
  "state": "session_mode"
 },
 {
  "action": "toggle_step_note",
  "packets": "0990<step>77 s110 0980<step>00",
  "note": "note mode only; step note = 0x10 + index; writes the currently selected pad note",
  "observe": "step LED 98 -> 122; Song.abl gains the note after ~14 s",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "clear_step",
  "packets": "0BB0777F s150 0990<step>77 s110 0980<step>00 s150 0BB07700",
  "observe": "OLED 'Delete...' while held; step LED 122 -> 98",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "select_pad_note",
  "packets": "0990<pad>77 s110 0980<pad>00",
  "note": "note mode",
  "observe": "pad flashes 126 then settles to 122; the previous 122 pads return to 123",
  "state": "note_mode"
 },
 {
  "action": "octave_up",
  "packets": "0BB0377F s110 0BB03700",
  "observe": "OLED card 'Octave up'; the 122 pads move",
  "state": "note_mode"
 },
 {
  "action": "octave_down",
  "packets": "0BB0367F s110 0BB03600",
  "observe": "OLED card 'Octave down'",
  "state": "note_mode"
 },
 {
  "action": "page_next",
  "packets": "0BB03F7F s110 0BB03F00",
  "observe": "steps repaint for the new page; 'B0 3E 18' appears (Left becomes available)",
  "state": "note_mode"
 },
 {
  "action": "page_prev",
  "packets": "0BB03E7F s110 0BB03E00",
  "observe": "steps repaint; 'B0 3E 00' when there is no earlier page",
  "state": "note_mode"
 },
 {
  "action": "shift_layer_peek",
  "packets": "0BB0317F s400 0BB03100",
  "observe": "steps 16,17,18,20,21,22,24 -> 127 in session; + 25,26,29,30,31 in note",
  "state": "unchanged"
 },
 {
  "action": "shift_step",
  "packets": "0BB0317F s80 0990<nn>77 s120 0980<nn>00 s80 0BB03100",
  "map": {
   "16": "Set Overview",
   "17": "System",
   "18": "Clip settings",
   "20": "Tempo",
   "21": "Metronome",
   "22": "Groove",
   "24": "Scale",
   "26": "Note Repeat",
   "29": "New clip (CREATES a clip)"
  },
  "observe": "OLED text band matches the map entry",
  "state": "that settings screen"
 },
 {
  "action": "loop_length_peek",
  "packets": "0BB03A7F s500 0BB03A00",
  "observe": "OLED 'Loop Length'; all 16 steps -> 124, the current length also gets a ch-9 message",
  "state": "unchanged on release"
 },
 {
  "action": "knob_turn",
  "packets": "0BB0<47..4E><01|7F>",
  "observe": "parameter overlay on the OLED; 'F0 00 21 1D 01 01 3B 10 <cc> ...' re-reports the ring value",
  "state": "unchanged"
 },
 {
  "action": "read_knob_values",
  "packets": "0BB0327F s110 0BB03200",
  "note": "any view change reissues all eight ring colours",
  "observe": "eight 'SYS 3B 10 n=71..78 rgb=(v,v,v)' messages; v is the parameter value 0-255",
  "state": "toggled session/note \u2014 pick a cheaper refresh if that matters"
 },
 {
  "action": "read_firmware_version",
  "packets": "0BB0317F s80 09901177 s120 09801100 s80 0BB03100 s1200 0BB00E01 s500 0BB00E01 s600 0BB0037F s110 0BB00300 s1500 0BB00E01 s500 0BB00E01 s800 0BB0037F s110 0BB00300",
  "note": "Shift+Step2 (System) -> jog to Update -> click -> jog to Current Version -> click",
  "observe": "OLED reads 'Move <version> / installed'; measured 'Move 2.1.0'",
  "state": "System > Update > Current Version"
 },
 {
  "action": "copy_paste_step",
  "packets": "0BB03C7F s250 0990<src>50 s110 0980<src>00 s350 0BB03C00 s450 0990<dst>50 s110 0980<dst>00",
  "note": "Copy is RELEASED before the destination; the armed source survives it (firmware 2.1.0)",
  "observe": "destination step LED 98 -> 122",
  "state": "note_mode",
  "destructive": true,
  "warning": "the armed source PERSISTS indefinitely across screens and resets; the next step press anywhere pastes. Clear it with copy_clear."
 },
 {
  "action": "copy_clear",
  "packets": "0BB03C7F s250 0990<empty_step>50 s110 0980<empty_step>00 s350 0BB03C00",
  "observe": "OLED 'Clipboard cleared'",
  "state": "note_mode"
 },
 {
  "action": "copy_range_select",
  "packets": "0BB03C7F s250 0990<a>50 s300 0990<b>50 s110 0980<b>00 s300 0980<a>00 s250 0BB03C00",
  "note": "source HELD while a second step is tapped",
  "observe": "NOTHING is written - it is a selection, not a paste (measured: destination stayed 98)",
  "state": "note_mode"
 },
 {
  "action": "per_step_automation",
  "packets": "0990<step>50 s400 0BB0<47..4E><01|7F> ... 0980<step>00",
  "note": "Move's OWN p-lock. The held step MUST already carry a note. The jog in the same gesture edits Note Length.",
  "observe": "held step LED -> 127; OLED titles '<track name> K1'..'K8'; the step's content value is unchanged on release",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "solo_track",
  "packets": "0BB0317F s90 0BB0587F s130 0BB05800 s90 0BB03100",
  "observe": "OLED '<track> soloed'",
  "state": "unchanged",
  "note": "Mute alone MUTES; Shift+Mute SOLOS"
 },
 {
  "action": "track_midi_menu",
  "packets": "0BB0317F s90 0BB0<2B|2A|29|28>7F s130 0BB0<..>00 s90 0BB03100",
  "observe": "OLED rows 'MIDI Out Ch<n>' / 'MIDI In Auto' / 'Color'; jog moves the cursor",
  "state": "per-track MIDI menu"
 },
 {
  "action": "shift_knob_bank",
  "packets": "0BB0317F s90 0BB0<47..4E><01|7F> s130 0BB03100",
  "observe": "a DIFFERENT parameter from the same knob unshifted (measured: knob1 'Transpose', knob8 'Grain Size')",
  "state": "unchanged"
 },
 {
  "action": "copy_pad_sample",
  "packets": "0BB03C7F s250 0990<pad>50 s110 0980<pad>00 s350 0BB03C00",
  "observe": "OLED 'Pad Sample copied'",
  "state": "note_mode, drum track"
 },
 {
  "action": "held_menu_preview",
  "packets": "0BB0327F s1300 0BB03200",
  "observe": "CC118 flips on the press and back on the release; the surface repaints both ways",
  "note": "a real non-destructive preview, but BLIND to Set Overview (no CC118 there) - scored 4/6 against localise_cold's 6/6",
  "state": "unchanged"
 }
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
Knobs 71-78 = 0x47..0x4E   Master 79=0x4F   Knob touch = notes 0-9
Sampling 87=0x57 (no observed effect)
Steps 1-16 = notes 16-31 = 0x10..0x1F
Pads = notes 68-99 = 0x44..0x63
```
