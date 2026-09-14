# Move's own UI, measured

A map of **Ableton Move's firmware UI** — its views, its LED language, its
buttons and its encoders — written so that a *program* can drive Move and know
where it is.

**Move has THREE readable channels and most of this document is about one of
them.** The control surface is §0–§10. **Move Manager — Ableton's own web app on
port 80** (`http://move.local/`) — is §11–§12. **The firmware image**,
`/opt/move/MoveOriginal`, is §13. The second and third were not considered until
late, which is why several conclusions here that read "unreachable" were really
"untried" — and both of them then overturned something.

**Firmware: Move 2.1.0**, confirmed two ways: on the device at **Setup → Update →
Current Version** (`Move 2.1.0 / installed`), and over HTTP from
`GET /api/v1/system/version`, which adds the build — commit `a6233f89a28a`,
2026-08-19, AbletonOS v3.18. Unless a row says otherwise, surface behaviour was
measured on **MIDI tracks**; **audio tracks are swept in §11.4** and differ
substantially.

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
`CC 118 → 0` **if it fires at all** — §2.2/§11.4: it is the Sampling lamp and its
correlation with the mode is set-dependent. From the reset state the mode is
known by construction, which is exactly why the reset is worth having.

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

### 2.2 `CC 118` — DO NOT USE IT FOR THE MODE (demoted three times; see §11.4)

The first draft of this document called CC 118 "the Session/Note indicator" on
the strength of four driven mode switches. That claim was too strong in two
separate ways, and both were found by testing it rather than by reasoning:

**(a) `CC 118 = 0` does not mean Session.** It means *not Note*. Entering Set
Overview (Shift + Step 1) also emits `B0 76 00`. Session and Set Overview are
indistinguishable on this CC.

**(c) It is not a mode flag at all.** §9.1 swept every CC and found CC 118 is
**Move's Sampling button**: pressing it prompts `Press pad`, and a pad press
starts `Recording...`. Its lamp means *"Sampling is available here"*.

**(d) And the correlation is SET-DEPENDENT.** §11.4: on `BNYX Demo 3`, toggling
Note↔Session emitted **no CC 118 at all** in either direction on any of its three
tracks, while the screen changed to `Session Mode` normally. The lamp only
transmits when availability *changes*, and whether it changes across a mode
toggle depends on the set. It tracked the mode in the one set this map was first
written against — that is the whole of it.

**Use §2.3.** The tables below are kept because they are accurate for what CC 118
*is*, and a driver watching a transition may still see it; they are no longer a
way to answer "which mode am I in".

**(b) Move never emits it spontaneously**, so a driver arriving cold has nothing
to read. It is emitted only when the Note-mode flag actually flips. Measured, by
counting CC 118 in the LED stream for every route driven in this survey:

| Route | Emits CC 118? | Observations |
|---|---|---|
| Menu tap that toggles the mode | **yes** — `0` or `124` | many |
| Menu tap while an overlay screen is up | **yes, it toggles** — 18/18 under control (§8.4). An earlier "no" is **not reproducible** and came from an uncontrolled starting state | 18 controlled |
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
already in Set Overview.**

**The lamps are for that split ONLY — never as the primary mode test.** On an
**audio track** in Note mode the Note-only lamps (25, 26, 30, 31) are absent
entirely (§11.4), so a lamp-based mode test would report "not Note" while you are
in it. The release repaint is unaffected: an audio track's step row is still
populated (every lit step reads `126`). Every Shift shortcut's lamp is dark while you are on
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
| **122** | step **holds a note for the currently selected voice** |
| **126** | **playhead** is on this step |
| **127** | the step is **held down**; also seen under the playhead while recording |
| 124 | step is an option in a transient chooser (Shift layer, Loop Length) |
| 98 / 112 / … | **empty — but the exact value is a per-track colour index**, see §8.8 |

**Test `== 122`, never `122 vs 98`.** The empty value was 98 on one track, 112
on another and 124 on a track with no clip; and 122 follows the *selected drum
voice*, so the same step flips between 122 and the empty value as you change
pads without the clip changing at all. §8.8 has the measurements.

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
| **Sampling** | **118** | **`Press pad` → pick a pad → `Recording...`**; Back cancels (§9.1). CC **87** is not it — 87 gives no response at all | **inert** — unavailable in Session | not tested |
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
| **Sampling 118** | `Press pad` → one of the **16 drum pads** → `Recording...`; Back / that pad / CC 118 stop it, **Play does not** (§10.1) | **inert** | not tested | ignored while armed |
| Sampling 87 *(not a control)* | – | not tested | not tested | not tested |
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
| Knob touch 0–9 | **no measurable effect** — the "burst" was Move's idle LED animation (§8.5, §9.4) | not tested | not tested | not tested |
| **every other CC 0–127** | **swept; none responds** (§9.1) | – | – | – |
| Steps 16–31 | toggle note; **hold + encoder = per-step automation** (§8.2) | – | **an indicator row; a press does nothing** (§9.5) | not tested |
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
| **Creating** an audio track | ten surface routes and Move Manager all fail (§9.2, §11.2). **Observing** one is done — see §11.4 |
| Move Manager's write side | fully **enumerated** in §12.2 from the app's source map; **not invoked** for the reasons in §12.4 (firmware/ssh/flags can take the instrument out of service or cut the channel Schwung depends on). One reversible directory create/delete round-trip was exercised. |
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
| 3 | 18 | **Workflow Settings** (`WorkflowSettingsDialog`, §13.4) — the screen is untitled; it shows `Max Length` / `Quantize` / `Step Grid` | any |
| 4 | 19 | **nothing, in five contexts** (§8.1) | — |
| 5 | 20 | Tempo | any |
| 6 | 21 | Metronome (`On` / `Off`) | any |
| 7 | 22 | Groove | any |
| **8** | **23** | **16 Pitches — a toggle, reports `On` / `Off`** | **Drum Kit track only** |
| 9 | 24 | Scale — `C Chromatic` / `Major` | any |
| **10** | **25** | **Full Velocity — a toggle, reports `On` / `Off`** | **Drum Kit track only** |
| 11 | 26 | **Arpeggiator** (rate / settings) — first read as "Note Repeat"; the firmware has `ArpeggiatorRateDialog` and no note-repeat class (§13.4) | Note mode |
| 12, 13 | 27, 28 | **nothing, in five contexts** (§8.1) | — |
| 14 | 29 | New clip — **creates a clip** | Note mode |
| **15** | **30** | **Double Loop** — `Loop doubled`, and the step row grows (§8.1) | Note mode |
| **16** | **31** | **Quantize** — `Clip <n>% Quantized` (§8.1) | Note mode, both track kinds, also while running |

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
| **Mute held** | `Mute...` card; CCs 71–78 show a **per-track automation mask** — 127 = that parameter has per-step automation, 0 = it does not (**§8.3**; an earlier claim here that they "go dark" was wrong) |
| Loop held | Loop Length chooser; all 16 steps → 124, current length marked on channel 9 |
| **Hold step + knob 1–8** | **per-step parameter automation — see §7.5** |
| **Hold step + jog** | **Note Length** |

**CORRECTED in §8.3 — the Mute layer DOES report automation.** This paragraph
originally said the rings go dark; that came from a broken filter in the capture
script. Holding Mute drives CCs 71–78 to a binary mask where **127 means that
parameter carries per-step automation somewhere in the clip**, proven by making
one bit flip on demand. See §8.3.

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

§8.2 enumerates the eight parameters, proves the per-step independence, and
settles the red question: **the red is on the encoder RINGS, and a p-locked step
is never recoloured** — it stays at 122.

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
- ~~**The 16 STEP buttons also load sets.**~~ **RETRACTED — see §8.6.** In a
  controlled re-run a step press in Set Overview produced **zero** LED events and
  no set change. The original observation followed an uncontrolled random walk
  and only the first of three presses changed anything, which is the signature of
  a load already in flight. The step row *is* lit in Set Overview and what it
  displays is not known, so treat a step there as unproven rather than safe.

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

## 8. Closing the named gaps

A finishing pass over everything §7 left open. Each item below is **closed** or
carries a **stated reason it cannot be**. Two earlier claims in this document
were wrong and are corrected here; they are called out rather than quietly
edited, because a reader who already acted on them needs to know.

### 8.1 Shift + Step 4, 12, 13, 15, 16 — closed

Each of the five was driven in **five contexts**: Note mode on a Drum Kit track,
Note mode on a melodic track, Session, Set Overview, and Note mode with the
**transport running**. 25 trials.

| Step | Verdict |
|---|---|
| **15** | **Double Loop — confirmed.** OLED `Loop doubled`, and the step row grew (`mapdiff {15: (126 → 112)}`). The belief carried from Schwung's `CLAUDE.md` is now measured. |
| **16** | **Quantize.** OLED `Clip <n>% Quantized`. Fires in Note mode on both track kinds and while running. |
| **4, 12, 13** | **No action in any of the five contexts.** LED event counts sat at the Shift-layer repaint baseline (58–100 events, identical to a step that does nothing) and no screen or step-map change occurred anywhere. |

The step-8/10 trap was specifically guarded against: those two were dead from Set
Overview and alive from a Drum Kit track, so all five were re-run from a Drum Kit
track first. **4, 12 and 13 are dead everywhere tested** — that is a closed cell,
not an unlit lamp.

*Caveat on the method:* in Session and Set Overview the "screen changed"
detector fires spuriously, because the before-frame is a transient card (`Session
Mode`, `Set Overview`) that expires during the trial. The real signal there is
that **all five leave the identical after-screen**, i.e. the default one.

### 8.2 Per-step automation, fully enumerated — closed

**The eight parameters are the selected device's own eight knob parameters —
the same row the knobs edit unheld.** They are not a separate automation-only
set. Measured on a Drum Kit track, holding a step that carries a note and
nudging each encoder in turn:

| Knob | Parameter | Example value |
|---|---|---|
| K1 | `Transpose` | `0 st` |
| K2 | `Start` | `0.0 %` |
| K3 | `Attack` | `1.10 ms` |
| K4 | `Hold` | `6?3 ms` |
| K5 | `Decay` | `??.? ms` |
| K6 | `Playback Effect` | `Stretch` *(an enum)* |
| K7 | `Stretch Factor` | `1.00` |
| K8 | `Grain Size` | `103 ms` |

**The names come from the DEVICE**, which is why the same gesture on a Schwung
chain slot shows generic `Schwung S2 K1` … `K8` — Move has no parameter names
for a device it does not know. A driver must not expect fixed names.

**It really is per-step**, proven by independence rather than by the label:
drive K1 up 20 detents on step 1, then read K1 on steps 2 and 3 → `0.00`, then
read step 1 again → the driven value. Repeated on a second track with
`Transpose`: same step `1 st`, other step `0 st`.

**Range:** K1 clamps at `0.00` at the bottom (48 down-detents stopped there);
no upper clamp was reached in 24 up-detents (~0.97 per detent). Upper bound not
established.

**The jog, in the same gesture, edits `Note Length`** — the hold opens directly
onto that screen.

#### The red is on the RINGS, and the step never turns red

- **A held step reads `d2 = 127`** on its own LED, and **the step's content value
  is unchanged afterwards** — still `122`. Checked immediately after setting a
  p-lock, and again after leaving the track and coming back. **Move does not
  recolour a step that carries per-step automation.** There is no red step.
- **While a step is held, every encoder ring turns RED** (`g = b = 0`): the
  idle rings were `(30,0,0)`-ish and the knob being driven went to
  `(106,0,0)` → `(107,0,0)`. Brightness is the value.

That last point is directly load-bearing: Schwung's p-lock is built on this
gesture, and **Move's own feedback for it is the ring colour, not the step.**

### 8.3 The Mute layer — my earlier claim was WRONG

§7.4 said "holding Mute turns all eight knob-ring LEDs dark". **That is
incorrect.** Holding Mute drives CCs 71–78 to a **binary per-track mask** of 0
and 127:

| Track | Mask (CC 71 → 78) |
|---|---|
| Track 1, Drum Kit | `0, 127, 127, 127, 127, 0, 127, 127` |
| Track 2, Schwung slot (generic K1–K8) | all `127` |
| Track 4, empty (no device, no clip) | all `0` |

**The mask means "this parameter has per-step automation somewhere in the
clip", and that is proven, not inferred.** Track 1 rests with bit 71
(`Transpose`) clear. Give `Transpose` a per-step value on one step, release,
re-read: **bit 71 flips 0 → 127**, and bit 76 (`Playback Effect`, untouched)
stays clear. One bit, one cause.

So the documented behaviour — *hold Mute and the rings report automation
status* — is real, and it is readable straight off the CC stream with no SysEx
decoding. The earlier wrong reading came from a broken filter in my own capture
script, not from the device.

This also explains the resting ring hues: the rings that are **red** are exactly
the ones the Mute mask sets to 127, and the **grey** ones are exactly the
zeros (`71=(142,142,142)` and `76=(30,30,30)` grey; `72..75,77,78` red at
varying brightness). Red ⇔ has automation; brightness ⇔ value.

### 8.4 Menu on an overlay — deterministic; the earlier anomaly is not reproducible

Re-run **3 times in each of 6 contexts, 18 trials, identical timing**:

| Context when Menu is pressed | CC 118 | Result |
|---|---|---|
| Tempo settings overlay | `0` ×3 | → Session |
| Scale settings overlay | `0` ×3 | → Session |
| Knob parameter overlay | `0` ×3 | → Session |
| Device carousel | `124` ×3 | → Note |
| Mode card still on screen | `124` ×3 | → Note |
| Plain track screen | `0` ×3 | → Session |

**Menu toggled the mode in all 18 trials.** The two 124 rows are toggles too —
those setups reach the carousel *via* a Menu tap, so the device was already in
Session.

So **§2.2's "a Menu tap while an overlay screen is up dismisses it and emits no
CC 118" does not reproduce.** The original observation came from a probe run
*after an uncontrolled random walk and four Back presses*, where the starting
context was unknown. The safe statement is: Menu toggles deterministically in
every context measured under control; **"Menu twice" is still not recommended as
a restate probe**, because a single unexplained observation of it failing exists
and the held-Menu and Shift-release probes are both better.

### 8.5 The knob-touch "burst" was my own measurement error

**Move continuously animates at least one button LED with nothing happening.**
A 1.2 s window with **no input at all** carries **20 LED events** — a stream of
`3B 10 n=40 rgb=(35,14,0) / (36,14,0) / (34,13,0) …`, i.e. CC 40 (Track 4)
being re-coloured with a slowly drifting orange.

Against that baseline a knob touch is nearly nothing: 49 events for touch-on
(28 of them the n=40 animation), 38 for touch-off, and 30 / 28 on two repeats.
The "120–137 event burst" reported in §7.9 was **the idle animation plus the
surrounding state**, not a touch feature.

**The lesson generalises: any event-count measurement on this device must
subtract an idle baseline.** What CC 40's animation actually represents is not
known.

### 8.6 Set Overview's steps — my earlier claim was WRONG

§7.7 said the 16 step buttons load sets. **In a controlled trial they do
nothing**: entering Set Overview cleanly and pressing step 1 produced **zero LED
events** and no set change (the screen moved only from the `Set Overview` card to
the set tile, which is the card expiring).

The earlier observation — set name changing from `Set 3` to `BNYX Demo 1`
coincident with a step press — came from a run that followed an uncontrolled
random walk, and **only the first of three step presses changed anything**, which
is the signature of a load that was already in flight. It is attributed to the
preceding state, not to the step.

What *is* measured about the Set Overview surface:

- **The pads are the set grid** (27 pad LED writes on entry, colour indices on
  channel 0, the loaded set marked on channel 9). **A pad press loads that
  set** — unchanged, and still the hazard.
- **The steps ARE lit** (42 step LED writes on entry, values 122/124/126/127), so
  the row displays *something*. What, and what a step press does in some other
  Set Overview state, is **not known**.

**For a driver the safety rule is unchanged and still conservative:** know the
pad mode before injecting a pad, and treat a step in Set Overview as unproven
rather than safe.

### 8.7 The held Note/Session preview — already scored, and it loses

§7.8 covers this: holding Menu flips the mode for the duration of the hold and
flips it back on release (`CC 118 → 0` on press, `→ 124` on release, full
repaint each way), so it *is* a real non-destructive preview. Scored head to
head over six random walks it got **4 / 6** against the Shift-release probe's
**6 / 6**, because it is blind to Set Overview — where it emits no CC 118 at
all, and where a wrong answer is the dangerous one. §2.3 stands.

### 8.8 A correction to §3.3: the "empty step" value is NOT fixed at 98

Decoding step value **112** turned up a bug in an earlier claim. Selecting each
drum pad in turn and re-reading the step row:

| Selected pad | Steps at 122 | Steps at 112 |
|---|---|---|
| 68 | 2, 4, 5, 9, 13 | all the rest |
| 69 | *(none)* | all 16 |
| 70 | 10 | all the rest |
| 71, 72 | *(none)* | all 16 |

So **122 follows the currently selected drum voice** — it marks "this step
carries a note *for the voice under your finger*", not "this step has any note".
And the *other* value is **not always 98**: it was 98 on track 2, **112** on
track 1, and **124** on a track with no clip at all. It behaves like a
per-track colour index.

**The reliable content test is `== 122`, not `122 vs 98`.** The original
verification in §3.3 stands (it was done on track 2, where empty really is 98)
but the rule stated there was over-general.

### 8.9 What is still open, and why

| Item | Why it is not closed |
|---|---|
| Upper bound of a per-step parameter's range | 24 up-detents never hit a clamp; needs a long sweep per parameter and the value is device-defined anyway |
| What CC 40's idle animation means | it runs with no input and no visible cause; no channel was found that explains it |
| What the Set Overview step row displays, and whether a step press ever acts | every probe there risks loading a set; ruled out as not worth the user's document |
| The Move-2.1.0 "red step" the manual describes | **actively looked for and not found** — a held step is 127, a p-locked step stays 122 across a track change. If it exists it is not on this firmware's step LEDs |
| Shift+Step 4, 12, 13 in a *sixth* context | five were tested; a context nobody has thought of cannot be ruled out, but the drum-track trap that caused the original miss was specifically covered |
| Audio tracks | none exist in the user's set; would need one created, which changes his document more than the answer is worth |

## 9. The last five — and CC 118 is a BUTTON

A finishing pass over the five items §8 left reachable-but-open. Four closed
outright, one closed as a measured negative. It also overturned the single
most-quoted claim in this document, found by a method that should have been run
on day one: **sweeping the whole CC space for controls nobody had identified.**

### 9.1 CC 118 is the SAMPLING button, not a mode indicator

Tapping every CC from 0 to 127 and watching for Move to light that same CC back
turned up exactly one unmapped control: **CC 118**. Pressing it puts `Press pad`
on the screen; pressing a pad then starts `Recording...`. Back cancels. Holding
it shows the same prompt and releasing cancels.

So the correlation §2.1 was built on is real but the mechanism is not what it
said:

> **CC 118's LED value is "the Sampling button is available here", and Sampling
> happens to be available in Note mode and not in Session.**

Confirmed in both directions: in Session mode a CC 118 press does **nothing at
all** (the screen never changes), which is exactly what a dark button should do.

**This does not break the localisation rule in §2.1–2.2 — the values and the
routes are unchanged — but it changes what a driver may safely infer.** CC 118
reads "Sampling available", so any future firmware that makes sampling available
somewhere else, or unavailable in Note mode, breaks the inference silently. The
§2.3 Shift-release probe does not depend on it and remains the recommended
method.

Sampling's destination is a **pad only**: `Sampling + Track button` cancels the
prompt and switches track, `Sampling + step` leaves the prompt up.

The sweep found nothing else. **Every CC that responds is now accounted for**,
which is as close to "the full set of buttons" as this method can get.

### 9.2 Audio tracks — a measured negative

**No gesture on the control surface creates an audio track**, and the user's
device has none to observe, so the audio-track column of §6 cannot be filled from
this instrument. Ten routes were driven and all failed:

| Route | Result |
|---|---|
| Press an empty-looking track, jog-click | opens the **preset browser** (and the jog *loads* presets as it scrolls) |
| Device carousel, walk to the end | no `+` entry; it clamps on the last device |
| Device carousel + Delete | nothing |
| Delete + Track button | nothing — **Delete does not delete a track** |
| Shift + Track menu | exactly **three** rows — `MIDI Out`, `MIDI In`, `Color` — all three open into value lists, none offers a track type |
| Sampling (CC 118) in Session | inert |
| Shift + Sampling | identical to Sampling |
| Shift + Capture | nothing |
| Sampling + Track button | cancels, switches track |
| Sampling + step | prompt stays up |

All four of the user's tracks carry instruments (`Drum Kit`, a Schwung slot,
`CPiano Rhodish`, `Mellow Bells`), and no track could be emptied from the
surface either.

**So the reason is a property of the device, not of the time available:** either
audio tracks are created off the surface (Move Manager, a file drop) or by a
gesture outside the ten above. Everything in this document that says "in Note
mode the steps do X" therefore remains scoped to **MIDI tracks**, and the
recipes in the appendix are written so that anyone who *has* an audio track can
finish that column in minutes.

*(Bonus mapping from the failed routes: the per-track menu's `MIDI In` list runs
`Off, Auto, Ch1, Ch2, …` and `Color` is a list of numbered colours. Track 4's
`MIDI In` was walked during this and put back to `Auto`.)*

### 9.3 Per-step parameter range — bounded, and I had simply not turned far enough

§8.2 reported no upper clamp in 24 detents. With 400:

| Detents | `Grain Size` |
|---|---|
| start | `103 ms` |
| +50 | `2?? ms` |
| **+100** | **`300 ms`** |
| +150 … +400 | `300 ms` — unchanged |
| −100 | `21.? ms` |
| **−200 … −500** | **`0.00 ms`** — unchanged |

**A per-step value is clamped to the device parameter's own range** (0–300 ms
for Grain Size), reached in ~100 detents and then immovable. Nothing about the
per-step layer is unbounded. The earlier "no clamp found" was an artefact of a
24-detent sweep, not a property of Move.

### 9.4 CC 40's idle animation — characterised, including the one thing that stops it

| State | CC 40 `3B 10` writes |
|---|---|
| Idle, track 1, Note | **13.5 /s** |
| Idle, track 2, Note | 11.7 /s |
| Idle, Session | 11.7 /s |
| **Transport RUNNING** | **0 /s — it stops completely** |

**§10.3 corrects this table: there are TWO idle animations, not one.** CC 43
(Track 1) pulses blue at ~37.6/s alongside CC 40's orange ~12.4/s; this section
counted only `n=40` because its filter looked for that. Both stop with the
transport. The real idle baseline is **~50 events/s**, not ~12.
| After stop | 10.7 /s, resumes |

It is a smooth orange pulse: `r` sweeps 32→48, `g` 13→19, `b` always 0, drifting
a step or two per message. It is **independent of the selected track and of
Note/Session**, and it is **gated on the transport**.

Practically: **~12 LED events per second arrive on this surface with no input at
all, and stop the moment playback starts.** Any event-count measurement must
subtract this baseline, and a measurement taken while stopped is not comparable
with one taken while running. It corrupted §7.9 of this document, where it was
reported as a knob-touch "burst".

What the pulse *means* is still unknown — CC 40 is the Track 4 button, but the
animation follows neither the selected track nor that track's content.

### 9.5 Set Overview's step row — it is an indicator, and a press does nothing

Read on entry, with no press needed:

```
step:  1    2    3    4    5..11  12   13   14   15   16
d2:   122  124  124  dark  124   126  dark 124  124  124
```

So the row is lit with the transient-chooser value 124, one step at 122, one at
126, and two dark. Alongside it the pads carry the set grid (colour indices on
channel 0, the loaded set additionally on channel 9 — here pad 99 = `122` + ch 9).

**Pressing step 1, step 8 and step 16 each produced 22–37 LED events over ~2.2 s
— which is the CC-40 idle animation and nothing else (§9.4: ~12/s × 2.2 s ≈ 26)
— and the loaded set did not change.** That is the third independent confirmation
of the §8.6 retraction, and this time the baseline is understood, so "22 events"
can be read confidently as "no response".

The Shift layer in Set Overview lights only `17, 18, 20, 24` — and step 16 is
dark, which is the §2.3 tell for "you are already in Set Overview".

What the 122 / 126 / dark positions encode is **answered in §13.2**: the row is
the **Shift shortcut layer**, drawn persistently, with step 1 at `122` because
step 1 *is* Set Overview and you are already in it. That is why no press acts on
it — the shortcuts need Shift held.

### 9.6 Shift + Step 4, 12, 13 — dead in eight contexts

Three further contexts on top of §8.1's five:

| Context | Steps 4 / 12 / 13 |
|---|---|
| A settings overlay (Tempo) on screen | no effect — the title stays `Tempo` |
| A clip playing | no effect |
| The Sampling `Press pad` prompt up | no effect — the prompt stays |

With the Drum Kit, melodic, Session, Set Overview and transport-running contexts
from §8.1 that is **eight**, including the two kinds of context that caught the
step-8/10 miss (a track type, and a modal state). These three steps are unused.

### 9.7 What remains, and why

| Item | Reason it is not closed |
|---|---|
| The whole audio-track column | **no audio track can be brought into existence from the surface** (§9.2), and none exists on the device |
| What CC 40's pulse represents | fully characterised but unattributed; it follows no track, mode or clip state that was varied |
| What the Set Overview step row encodes | it is an indicator that no press acts on (§9.5); nothing on the surface interrogates it |
| ~~The sampling flow past `Recording...`~~ | **CLOSED in §10.1** — the stoppers, the valid destinations, the armed surface and the file destination are all measured |
| A ninth context for Shift+Step 4/12/13 | eight were tried, spanning both trap categories; further contexts are unenumerable |

## 10. The Sampling flow, and where this map stops

### 10.1 Sampling, end to end

`CC 118` is the Sampling button (§9.1). The whole flow, driven:

```
CC 118  ──▶  ARMED: "Press pad" + a boxed MICROPHONE icon
              │
              ├─ jog turn  ──▶  a "Sampling" settings screen (§10.2)
              ├─ Back / Track button / Menu  ──▶  cancels
              └─ press one of the 16 drum pads
                        │
                        ▼
              RECORDING: "Recording..." + a live INPUT LEVEL METER
                        │
                        ├─ Back          ──▶ stops
                        ├─ the same pad  ──▶ stops
                        ├─ CC 118 again  ──▶ stops
                        └─ Play (CC 85)  ──▶ IGNORED, recording continues
```

**The armed surface.** All sixteen drum pads light `d2 = 65`, and the currently
selected drum pad additionally gets `126`. The step row is dark. Every knob-ring
CC (71–78) is driven to 0.

**Only the drum-rack pads are valid destinations.** Measured by arming and
pressing six pads across the grid:

| Pad | Result |
|---|---|
| 68, 76, 84, 92 | **records** |
| 72, 99 | **ignored** — the prompt stays up |

Lit-while-armed is exactly `68,69,70,71 / 76,77,78,79 / 84,85,86,87 / 92,93,94,95`
— **the left four columns of all four rows**, i.e. the drum rack's 16 pads as a
4×4 block on the left half of the grid. Pads outside it are inert in this mode.
That is also the first direct measurement of the drum rack's physical layout in
this document.

**What the other controls do while ARMED:** knob turn, step press and Play are
**ignored** (the prompt stays). The jog opens the settings screen. A **Track
button** and **Menu** both cancel the prompt and do their normal job.

**Play does not stop a recording.** Of the four stoppers tried, Back, the
destination pad and CC 118 all end it; Play is ignored and the take keeps
running. That asymmetry is worth knowing — a driver that "stops everything with
Play" will leave a sampler running.

**There is no auto-stop within 16 s** (watched in 2 s steps; the take was still
going when Back ended it).

**Where the sample lands:**

```
/data/UserData/UserLibrary/Recordings/<Set name> Rec <n>.wav
```

`<n>` increments per take and never reuses a number — takes 1…9 were produced
across this session. File sizes tracked the hold duration exactly (≈3 s takes
were ~630–700 KB, a ~7 s take 1.4 MB, an ~11 s take 2.0 MB), i.e. **the
recording runs for exactly as long as it is left running**, with no rounding to
a bar.

*(All nine test takes were deleted afterwards; the Recordings folder is empty.)*

### 10.2 The Sampling settings screen — reached, not named

A jog turn while armed replaces `Press pad` with a screen titled **`Sampling`**,
and a jog click opens a three-item row. Further jog turns do not change the
title, so the items are not enumerated by name.

The three items are **pictorial, with no text labels**: a small glyph that reads
as a save/target icon, a wide horizontal bar, and a boxed icon. The OLED carries
no words for them, so **they cannot be identified from the display alone** — this
is a limit of the readback channel, not of effort. A user looking at the screen
would recognise the icons; a program reading the framebuffer cannot.

### 10.3 CC 40's pulse — characterised, unattributed, and there are TWO of them

**Correction to §9.4:** it is not one LED. Counting per-CC rather than filtering
for `n=40` shows **two** idle animations running together:

| LED | Rate | Colour |
|---|---|---|
| **CC 43** (Track 1) | **~37.6 /s** | blue — `(r,r,255)` with `r` sweeping ~15→227 |
| **CC 40** (Track 4) | **~12.4 /s** | orange — `(32…48, 13…19, 0)` |

§9.4 only reported the slower one because the filter looked for `n=40`.

**Both stop completely while the transport is running** — zero ring writes of any
kind — and both resume when it stops. Confirmed over three start/stop cycles plus
one in Session, four clean transitions.

*A caution about that last one*: a Session trial initially looked like a
counter-example (pulses continuing with a clip "playing"), but the clip at that
pad had been deleted earlier in the survey, so the launch started nothing — and
the *next* Play press, which really did start the transport, silenced the pulses
as expected. The apparent contradiction was a stale assumption about the set's
contents, not about Move.

**Attribution failed.** Eight variations moved the rate by nothing beyond noise
(11.2–13.8 /s for CC 40 throughout): selected track = 1, 3 or 4; track 4 muted;
metronome on and off; Record armed; Sampling armed; Session vs Note. The two
animated LEDs are the Track 1 and Track 4 buttons, but the animation follows
neither selection nor mute nor clip content.

**This is where it stops.** The pulse is precisely characterised — rate, colour
range, both LEDs, and the one state that gates it — and its *meaning* is not
observable from the control surface. Recorded as characterised-but-unattributed.

**The practical consequence is the part that matters:** with no input at all this
surface emits **~50 LED events per second** while the transport is stopped and
**none** while it runs. Any event-count measurement must subtract that baseline,
and counts taken stopped are not comparable with counts taken running. It has
already corrupted one measurement in this document (§7.9's "knob-touch burst")
and one in §9.4.

## 11. The second channel — Move Manager, and the audio tracks that were there all along

**Move serves its own web app on port 80.** `http://move.local/` is Ableton's
**Move Manager** (Schwung's manager is a different thing on :7700). This document
had never considered it, and "everything reachable has been measured" in §10 was
therefore false: it meant *everything reachable from the control surface*. A
whole second control channel had not been named as untried — it had not been
thought of. That is a worse failure than a wrong measurement, and it produced the
two biggest corrections below.

### 11.1 Authenticating: the PIN is on Move's own screen

```
POST /api/v1/challenge            Content-Type: application/json, body {}
        ──▶ Move displays a six-digit PIN in large type on its OLED
POST /api/v1/challenge-response   {"secret":"<pin>"}
        ──▶ 200 + Set-Cookie: Ableton-Challenge-Response-Token=…  (Max-Age 2592000 = 30 days)
```

Two mechanics worth knowing:

- **The `Content-Type: application/json` header is load-bearing.** A plain
  `POST` with no body returns **400 with an empty body** — indistinguishable
  from "refused". Adding the header and `{}` returns 200 and puts the PIN up.
  An `Origin`/`Referer` pair does *not* help; the content type is the whole
  difference.
- **Wrong attempts are rate-limited and counted**: a wrong secret returns 401
  with `X-Retries-Left: 2`, and the app's own code handles a 429 with
  `Retry-After`. So this is not brute-forceable and a driver gets three tries.

The PIN is rendered in a large font that the glyph table in §0 does not cover; it
was read by rendering each digit's bitmap directly. **A `5` and a `6` differ only
in whether the bowl's left wall is closed above the base** — the first attempt
misread exactly that and burned a retry.

Unauthenticated, only `/api/v1/language` and `/api/v1/feature-flags/current`
answer; everything else is `401 {"error":"Unset credentials"}`.

### 11.2 What the channel exposes

Endpoint list lifted from the app bundle and then driven:

| Endpoint | Answer (measured) |
|---|---|
| `GET /api/v1/system/version` | `{"version":"2.1.0","branch":"move/release-v2.1.0","commit":"a6233f89a28a","commitDate":"2026-08-19","os":"AbletonOS v3.18","coreLibraryVersion":"0.59"}` |
| `GET /api/v1/is-move-running` | `{"isMoveRunning":true}` |
| `GET /api/v1/datetime` | `2026-09-14T21:29:30Z` |
| `GET /api/v1/system/update-channel` | `{"updateChannel":"move-stable"}` |
| `GET /api/v1/cloud-auth/status` | `{"status":"notAuthenticated"}` |
| `GET /api/v1/feature-flags/current` | `{"enableScreenReader":false,"enableVirtualMemoryLimit":true}` — and the schema shows those are the **only two flags**; neither has anything to do with audio tracks |
| `GET /api/v1/files/` | a JSON file listing of `UserLibrary` (`Sets`, `Recordings`, `Samples`, `Track Presets`, `Audio Effects`) |
| `GET /api/v1/screen-reader` | **an SSE stream of Move's screen-reader text** — see below |
| also present | `/api/v1/update`, `/update/reboot`, `/ssh`, `/syslog{,/current,/zip}`, `/perf/{start,stop,pop}`, `/render`, `/language`, `/legal/licenses`, `/cloud-auth/{start,complete,revoke}`, `/feature-flags/{next,reset-next,schema}` |

**It is a file and system manager, not a set editor.** Nothing in it creates or
edits tracks, clips or devices — which is why the audio-track answer did not come
from here either (§11.3). *(Refined in §12.2: whole sets **can** be listed,
uploaded, renamed and deleted as `.ablbundle` objects. What has no endpoint is
their contents.)*

**`/api/v1/system/version` independently confirms the firmware** read off the
OLED in §7 — 2.1.0, and it adds the build: commit `a6233f89a28a`, 2026-08-19,
AbletonOS v3.18.

**The screen-reader SSE would have replaced the whole OCR pipeline** — if it were
switched on. Connecting returns `data: {"type":"text","text":"Drum Kit"}`, i.e.
the current screen as *text*. But driving twelve screen changes (the Sampling
flow and its settings screen) produced **no further events**: announcements are
gated on Move's own screen-reader setting, which is off on this device. So the
endpoint exists, answers, and is silent in this configuration. **A driver that
can turn Move's screen reader on gets a text feed of the OLED for free**, and
would not need §0's glyph table at all.

### 11.3 Audio tracks — the measurement was right and the conclusion was wrong

§9.2 concluded "no audio track can be brought into existence from the surface,
**and the device has none to observe**". The first half still stands — ten
surface routes, all negative, and Move Manager adds no eleventh, because it has
no set-editing API.

**The second half was wrong. Three of the eight sets on the device already
contain audio tracks**, and I had never loaded them. Reading every
`Song.abl` directly (`tracks[].kind`):

| Set | Track kinds |
|---|---|
| Set 3, BNYX Demo 1, 2, 4 | `midi, midi, midi, midi` |
| **BNYX Demo 3** | **`audio, audio`**, midi, midi |
| **Jose Castillo** | **`audio, audio, audio`**, midi |
| **Alice Ivy** | **`audio, audio, audio`**, midi |
| **Heavy Mellow** | midi, **`audio, audio, audio`** |

So the track type is `kind: "audio"` vs `kind: "midi"` in the set file, the
device had audio tracks the whole time, and the column was reachable by loading
`BNYX Demo 3` from the Set Overview grid (pad 70). Which I then did.

### 11.4 An AUDIO track on the surface

Swept on `BNYX Demo 3`, tracks 1 and 2 (audio) against track 3 (MIDI) in the same
set, so the comparison is within one document.

| | **Audio track** | MIDI track (same set) |
|---|---|---|
| Track screen | icon row differs; the name row reads **`Audio Track`** | `Melodic Sampler` etc. |
| Step row | **every lit step is `126`** (T1: steps 1–4 and 9–16; T2: steps 1–8) | content values (`122` = note, `82` = empty on this track) |
| Pads | **inert — a pad press produces no LED event at all** | pad flashes `126`, settles `122` |
| Knobs | **no parameter overlay** — the screen does not change | `Transpose` etc. |
| Shift lamps | `16,17,18,20,21,22,24,29` — **none of the Note-only markers** | `…,25,26,29,30,31` |
| Mute automation mask | **nothing written at all** (CCs 71–78 silent) | all `0` |
| Hold step + jog | **`Empty Audio Clip`** | the clip's name |
| Hold step + knob | no parameter overlay | `Transpose` |
| Sampling (CC 118) | **available** — `Press pad` | available |

So on an audio track: **the pads, the knobs and per-step automation are all
absent**, the step row switches from a content map to a uniform `126`, and the
Shift layer loses its Note-only entries. Every claim in this document of the form
"in Note mode the steps do X" is confirmed to be **MIDI-track-only**.

**And CC 118 did not fire at all.** Toggling Note↔Session with Menu on all three
tracks of this set emitted **no CC 118 in either direction**, while the screen
changed to `Session Mode` normally. In `Set 3` the same toggle reliably emitted
`0` / `124`.

That is the **fourth** correction to the CC 118 story, and the decisive one:

> **CC 118 is not a usable mode signal. It is the Sampling button's availability
> lamp, it only transmits when that availability changes, and whether it changes
> across a mode toggle is SET-DEPENDENT.** It happened to track the mode in the
> one set this map was written against.

§2.3's Shift-**release** probe is unaffected — it reads the step row, which on an
audio track is still populated, so it still answers "Note". But §2.3's
*secondary* lamp test inherits a caveat: **the Note-only lamps are absent on an
audio track**, so the lamps must never be used as the primary mode test. They are
only for splitting Session from Set Overview, which is what §2.3 already says.

### 11.5 Set Overview's step row — varied, and it does not encode the loaded set

The row was read under two different loaded sets:

| Loaded set | Step row |
|---|---|
| BNYX Demo 3 | `1:122`, `2,3,5,6,7,9,14 : 124` |
| Set 3 | `1:122`, `2,3,5,6,7,8,9,10,11,14,15,16 : 124` |

**Step 1 is `122` and everything else is `124` in both.** The apparent difference
is only *which positions were re-transmitted* — Move writes an LED when it
changes, so a position missing from a capture is unchanged, not dark.

So the most likely hypothesis — that the row indicates which set is loaded — is
**disproved**: the pattern is identical across two different loaded sets (four,
by §13.2). **§13.2 then identifies what it actually is**: the Shift shortcut
layer, painted persistently. (What
*does* mark the loaded set is a **pad**: in Set 3 pad 99 carried the channel-9
marker, and with BNYX Demo 3 loaded no pad in the captured window did.) What the
step row encodes remains unknown, but the obvious candidate has now been tested
and rejected rather than left untried.

### 11.6 CC 40 / CC 43 — a second hypothesis class, also negative

§10.3 tried eight variations of the instrument's *musical* state. The external
class was tried too: with an authenticated Move Manager session **and** an open
SSE stream from this host, the rates were **CC 43 = 38.0/s, CC 40 = 14.6/s** —
indistinguishable from the idle baseline.

Nine variations, two hypothesis classes, nothing moves it except the transport.
**Recorded as characterised-but-unattributed and closed.**

### 11.7 The Sampling settings icons, described

The three items on the screen §10.2 reached, rendered from the framebuffer.
Ableton's published Sampling documentation is for ~1.5.x and these are on 2.1.0,
so they are described rather than named:

```
item 1  (7x13)          item 2  (52x10)        item 3  (23x22, boxed)
  .###.                 ####################     #######################
  .###.                 ####################     #.....................#
  .###.                 ####################     #........#####....#...#
  #####                 ####################     #......##.....##.#....#
  #...#                 ....................     #.....#.........#.....#
  #...#                 ....................     #....#........#..#....#
  #...#                 ####################     #....#......#....#....#
  #...#                 ####################     #....#..#.#...#..#....#
  #####                 ####################     #....#.###....##.#....#
  ..#..                 ####################     #......##.....##......#
  ..#..                                          #.....#.#.....#.......#
  ..#..                                          #....#................#
                                                 #######################
```

- **Item 1** is a small solid block above a wide bar above a short stem — it
  reads as a **microphone on a stand**, matching the boxed mic icon on the
  `Press pad` screen.
- **Item 2** is two long horizontal bars with a gap — a **level/threshold bar**
  or a slider track.
- **Item 3** is a boxed pictogram of **two crossing curved strokes**, most like a
  waveform or a routing/crossfade glyph.

Item 1's match to the arm screen's mic makes **input source** the natural reading
for it, and item 2's bar makes **a level or threshold** the natural reading for
the second — but neither was confirmed by changing one and observing an effect,
so both stay in Not known. The renders are here because an ASCII picture is more
use to the next person than the phrase "not nameable".

## 12. Move Manager's write side — enumerated, mostly not exercised

§11 drove the read side. This section maps the **whole** API, including the parts
that change the instrument, and states for each whether it was invoked. **Most
were deliberately not invoked**, and that is a scope decision, not a limit of the
device — the reasons are in §12.4.

### 12.1 How this was enumerated: the app ships its own source

Move serves a **source map** alongside the bundle:

```
GET /assets/index-CFUsWDQW.js.map        → 200, 6.4 MB, with sourcesContent
```

It contains the **original TypeScript of the API client**, so the endpoints,
verbs and payload shapes below are read out of Ableton's own source rather than
guessed from probing. Nineteen files under `packages/api-client/src/`, one per
API module (`FilesApi.ts`, `DataApi.ts`, `MoveUpdateApi.ts`, …).

**This costs the device nothing and is strictly better than probing** — it names
methods a probe would never find and payload shapes a probe could only guess.
Anyone extending this map should start here.

### 12.2 The complete API

`E` = exercised in this survey. `—` = enumerated only, never invoked.

| | Method + path | What it does | Expects |
|---|---|---|---|
| **E** | `POST /api/v1/challenge` | puts a 6-digit PIN on the OLED | **`Content-Type: application/json`**, body `{}` |
| **E** | `POST /api/v1/challenge-response` | exchanges the PIN for a 30-day cookie | `{"secret":"<pin>"}` |
| **E** | `GET /api/v1/system/version` | firmware + build | — |
| **E** | `GET /api/v1/is-move-running` | `{"isMoveRunning":bool}` | — |
| **E** | `GET /api/v1/datetime` | ISO-8601 clock | — |
| **E** | `GET /api/v1/language` | `{"languageCode":"en"}` — **answers unauthenticated** | — |
| **E** | `GET /api/v1/feature-flags/current` `/schema` | the two flags and their schema — **`current` answers unauthenticated** | — |
| **E** | `GET /api/v1/files/` and `/files/{path}` | directory listing of `UserLibrary` | — |
| **E** | `GET /api/v1/data/Sets` | every set: `objectId`, `name`, `size`, `lastModifiedDateTime`, `cloudState` | — |
| **E** | `OPTIONS /api/v1/files/{path}`, `/data/{bucket}` | capability discovery — see §12.3 | — |
| **E** | `GET /api/v1/screen-reader` | SSE stream of screen-reader text | — |
| **E** | `POST /api/v1/files/{path}` *(no body)* | **create a directory** | — |
| **E** | `DELETE /api/v1/files/{path}` | **delete a file or directory** | — |
| — | `PATCH /api/v1/files/{base}/{oldName}` | rename. The old path is **percent-encoded**, the new one is **not** | `{"path":"<base>/<newName>"}` |
| — | `POST /api/v1/files/{path}` *(multipart)* | upload a file | `FormData`; extensions and size capped per directory (§12.3) |
| — | `POST /api/v1/data/Sets` | upload a **set bundle** | `.ablbundle` |
| — | `DELETE /api/v1/data/Sets/{objectId}` | delete a whole set | — |
| — | `PATCH /api/v1/data/Sets/{objectId}` | rename a set | — |
| — | `PATCH /api/v1/language` | change UI language | `{"languageCode":…}` |
| — | `PATCH /api/v1/system/update-channel` | switch update channel | `{updateChannel}` |
| — | `GET /api/v1/update/{channel}/{version}/` | check for an update | — |
| — | `POST /api/v1/update` | **install a firmware update** | multipart |
| — | `POST /api/v1/update/reboot` | **reboot to finish an update** | — |
| — | `PATCH /api/v1/feature-flags/next` `/reset-next` | set / clear a flag for next boot | `{name, value}` |
| — | `POST /api/v1/ssh` | **add an authorised SSH public key** | `{sshKey}` |
| — | `GET /api/v1/syslog`, `/syslog/{id}`, `/current`, `/zip` | system logs | — |
| — | `POST /api/v1/perf/start` `/stop` `/pop` | performance recording | — |
| — | `POST /api/v1/render/{objectId}?format=…` | **render a set to audio**; `DELETE /api/v1/render/{id}` aborts | — |
| — | `POST /api/v1/cloud-auth/start` `/complete` `/revoke` | Ableton Cloud linking | — |
| — | `POST /api/v1/time` | set the system clock | ISO-8601 |

**There is no API that edits the CONTENTS of a set.** §11.2 said "no set-editing
API", which was too broad: sets can be **listed, uploaded, renamed and deleted**
as whole `.ablbundle` objects. What has no endpoint is tracks, clips, devices or
notes. That is why Move Manager offered no eleventh route to *creating* an audio
track (§9.2) — though uploading a bundle that already contains one would work,
and was not attempted.

### 12.3 `OPTIONS` is honoured, and it is the cheapest thing in this document

Only the `files` and `data` trees answer it; everything else 404s on OPTIONS.
Where it answers it returns real capability headers:

| Path | `Allow` | Extra headers |
|---|---|---|
| `/api/v1/files/` | `OPTIONS, GET` — **the root is read-only** | `Allowed-Audio-Conversion-File-Extensions: .aif, .aiff` |
| `/api/v1/files/Sets` | `OPTIONS, GET, PATCH, DELETE` — **no POST** | ditto |
| `/api/v1/files/Recordings` | `OPTIONS, GET, PATCH, DELETE` | ditto |
| `/api/v1/files/Samples` | `OPTIONS, GET, POST, PATCH, DELETE` | `Allowed-Post-File-Size: 100000000`, `Allowed-Post-File-Extensions: .wav, .wave, .aif, .aiff, .aifc` |
| `/api/v1/files/Track Presets` | `OPTIONS, GET, POST, PATCH, DELETE` | `Allowed-Post-File-Extensions: .ablpresetbundle` |
| `/api/v1/data/Sets` | `GET, POST, DELETE, PATCH, OPTIONS` | `Allowed-Post-File-Extensions: .ablbundle` |

So **per-directory write permissions and upload limits are discoverable without
writing anything** — a 100 MB cap on Samples, and each tree accepts only its own
bundle type.

### 12.4 The one write that was exercised, and why the rest were not

**Exercised** — a create/delete round-trip with a matching undo, on a throwaway
name, touching nothing of the user's:

```
POST   /api/v1/files/Samples/zz-probe   → 200, appears in the listing
PATCH  /api/v1/files/Samples/zz-probe   → 400 {"error":"… \"path\" property required by JSON body."}
DELETE /api/v1/files/Samples/zz-probe   → 200
GET    /api/v1/files/Samples            → clean; verified on disk, 0 entries left
```

The PATCH failure is itself the finding: **rename needs `{"path": "<base>/<newName>"}`**,
and the server says so. The source confirms the asymmetric encoding — old path
percent-encoded, new path not.

**Deliberately NOT exercised**, with the reason:

| Endpoint | Why not |
|---|---|
| `POST /api/v1/update`, `POST /api/v1/update/reboot` | **would change the firmware this entire document is pinned to (2.1.0) and can take the instrument out of service.** Not reversible from here and not authorised. |
| `PATCH /api/v1/system/update-channel` | switching to a beta channel invites exactly that. |
| `POST /api/v1/ssh` | installs an authorised key. **SSH is the channel Schwung and this survey both depend on**; anything touching it risks the access everything else needs. |
| `PATCH /api/v1/feature-flags/next` `/reset-next` | unknown blast radius, applied at next boot, and one of the two flags governs a virtual-memory limit on the Move app. |
| `POST /api/v1/data/Sets`, `DELETE`, `PATCH` on a set | whole-set upload/delete/rename against the user's eight sets. The user's "run wild" covered **clip, track and device contents**, not destroying his sets wholesale. |
| `POST /api/v1/files/{path}` multipart upload | same reasoning; the create/delete round-trip already proves the write path is live. |
| `POST /api/v1/render/{objectId}` | renders a set to audio — heavy, and its only abort is a DELETE against an id you must already hold. |
| `POST /api/v1/cloud-auth/start` `/complete` | links the device to an Ableton Cloud account. Not mine to link. |
| `POST /api/v1/time` | sets the system clock; nothing here needs it. |
| `POST /api/v1/perf/*`, `GET /syslog/*` | harmless but irrelevant; enumerated for completeness. |

**Every one of these is a scope decision, not a device limit.** They are all
reachable, the payloads are in §12.2, and a future pass with explicit permission
could drive any of them.

### 12.5 Two things to carry away

**The SSE screen-reader stream would replace this document's OCR pipeline.**
`GET /api/v1/screen-reader` returns Move's screen text as
`data: {"type":"text","text":"Drum Kit"}`. It is **gated on Move's own screen
reader, which is OFF on this device**, so it emits the current screen and then
stays silent. Anyone who can switch that on gets the OLED as text and needs none
of §0's glyph table.

**Authenticating issues a 30-day token and there is no revoke.**
`Set-Cookie: Ableton-Challenge-Response-Token=…; Max-Age=2592000; HttpOnly;
SameSite=Strict`. Nothing in the API revokes it — `/api/v1/cloud-auth/revoke` is
for Ableton Cloud, not for this session. **Two such tokens were issued during
this survey** (§11.1 and §12). The local cookie jars were deleted; the
device-side grant stands until it expires. A user who wants it gone should
assume a reboot or a firmware update is the only lever, and that was not tested.

## 13. The third channel — Move's own firmware image

`/opt/move/MoveOriginal` is Move's executable, 29,740,104 bytes, readable over
ssh. **Read only** — nothing here modified, moved or replaced it; its size and
mtime are unchanged (`Aug 19 13:03`).

Aimed at the two questions §10–§12 closed as unattributable. **One is now
solved; the other is genuinely closed across three named channels.**

### 13.1 What the image will and will not tell you

The binary is **stripped of `.symtab`** — 32 sections, `.dynsym` only (1,153
entries, essentially imports). Internal function names are gone.

What survives is **RTTI class names in `.rodata`**, and they are remarkably rich:
**675 distinct `ableton::move` classes**, plus assert strings that carry their
source paths (`products/move/MoveLib/src/…`). One single mangled symbol contains
**Move's entire view tree** — every view, delegate and LED delegate, nested in
construction order.

```bash
strings -n 6 /opt/move/MoveOriginal            # 22,991 strings
# class names:
grep -oE 'NS0_[0-9]+[A-Za-z]+' out | sed -E 's/^NS0_[0-9]+//' | sort -u
```

Source files named by asserts include `StepButtonColorUtils.cpp`,
`SongOverviewView.cpp`, `Animation.cpp`, `ColorStyle.hpp`, `Color.cpp`,
`DrumPadsView.cpp`, `MelodicPlayView.cpp`, `StepEditorView.cpp`,
`ScreenReaderSurface.cpp`, `Clipboard.cpp`, `SampleRecorder.cpp`.

### 13.2 SOLVED — the Set Overview step row is the Shift shortcut layer

Loaded **four different sets** and read the row each time:

| Loaded set | Step row |
|---|---|
| BNYX Demo 1 | `1:122`, `2,3,5,6,7,8,9,10,11,14,15,16 : 124`, **4, 12, 13 dark** |
| BNYX Demo 2 | identical |
| BNYX Demo 4 | identical |
| Set 3 | identical |

**Byte-identical across all four**, and the loaded set is marked correctly on a
**pad** each time (ch 9 on pad 68, 69, 71, 99 respectively). So it certainly does
not encode the set.

**Then compare it with the Shift lamp set.** The lit positions are
`1,2,3,5,6,7,8,9,10,11,14,15,16` and the dark ones are `4, 12, 13` — which is
**exactly** the Shift+Step map of §7.3/§8.1: twelve shortcuts plus step 8
(16 Pitches) on a drum-kit track, and steps 4, 12 and 13 unused *everywhere*
(§8.1 and §9.6 proved those three dead in eight contexts).

So the row is **the Shift shortcut layer, painted persistently in Set Overview**,
with one position distinguished: **step 1 shows `122` instead of `124` because
step 1 *is* Set Overview and you are already in it** — the same "the current
screen's own lamp differs" rule §2.3 uses to detect Set Overview.

The firmware agrees. Move names four step-row delegates —
`LoopModeStepsDelegate`, `StepEditorDelegate`, `AudioTrackLoopModeStepsDelegate`,
`AudioTrackStepsDelegate` — and **Song Overview (Move's internal name for Set
Overview) has none of them**: it has `SongOverviewView` and
`SongOverviewWheelViewDelegate`, a *wheel* (jog) delegate. Nothing in Song
Overview owns the step row, which is why a press does nothing (§9.5) and why the
row is invariant.

**That closes it, and it retires a gap rather than restating one.**

### 13.3 NOT SOLVED — CC 40 / CC 43's idle pulses, now closed across three channels

The image does not name its LED animations:

- **No `.symtab`.** Any `Animation.cpp` function names are stripped.
- **`Animation.cpp` exists** as an assert path, and `AnimatedColor` exists as one
  of four colour variants (`RgbPaletteColor`, `RgbColor`, `MonochromeColor`,
  `AnimatedColor`) — but `AnimatedColor` appears only as a *type parameter*.
  There is no animation-kind class, enum or table.
- **Of the 675 `ableton::move` classes, twelve are LED delegates** — and every
  one is a Shift-layer icon (`MainModeShiftStepIconLedDelegate`,
  `MetronomeButtonIconLedDelegate`, `NewClipButtonIconLedDelegate`,
  `FullVelocityButtonIconLedDelegate`, `QuantizeButtonIconLedDelegate`,
  `DoubleLoopButtonIconLedDelegate`, `LayoutsButtonLedDelegate`,
  `ArpeggiatorButtonIconLedDelegate`, `ArpeggiatorButtonLedDelegate`,
  `GrooveAmountLedDelegate`, `DialogModeIconViewLedDelegate`,
  `MainModeIconViewLedDelegate`). **There is no track-button LED delegate at
  all**, and nothing carrying the measured colour values.

So the honest statement is now three channels wide, each named:

> **CC 40 and CC 43's idle pulses are not attributable from the control surface
> (nine behavioural variations, §10.3 and §11.6), from the Move Manager HTTP API
> (§11–§12), or from the firmware image (§13).** They remain precisely
> characterised — ~12.4/s orange on CC 40, ~37.6/s blue on CC 43, both stopping
> dead while the transport runs — and unattributed.

That is a stronger claim than the one it replaces, because it says which doors
were tried. A fourth channel would be the DSP binaries in `/opt/move/Dsp/`, the
D-Bus interface, or a debug build — none of which was opened.

### 13.4 What the view tree gives back for free

The single view-tree symbol is a structural map of Move's UI, and it
**cross-checks §7.3's Shift+Step layer**: twelve button delegates, one per lit
lamp, in construction order — `SongOverviewModeButton`, `GrooveAmountButton`,
`LayoutsButton`, `ScaleButton`, `WorkflowSettingsButton`, `TempoButton`,
`MetronomeButton`, `ArpeggiatorButton`, `NewClipButton`, `FullVelocityButton`,
`QuantizeButton`, `DoubleLoopButton`.

Two corrections to §7.3 follow, both about **names**, not behaviour:

- **Step 11 is the ARPEGGIATOR, not "Note Repeat".** §7.3 read the screen as
  `C Repeat / Rate 1/1?`; the firmware has `ArpeggiatorButtonDelegate`,
  `ArpeggiatorRateDialog`, `ArpeggiatorSettingsDialog` and
  `ArpeggiatorStyleDialog`, and no note-repeat class at all.
- **Step 3 is Move's "Workflow Settings"** (`WorkflowSettingsDialog`) — §7.3
  described it by its contents (`Max Length / Quantize / Step Grid`) because the
  screen carries no title.

It also independently confirms **audio tracks are a first-class track type** —
`AudioTrackStepsDelegate`, `AudioTrackLoopModeStepsDelegate`,
`makeAudioTrackNoteModeStepsView`, `RecordButtonDelegateNoteModeAudioTrack`,
`SampleRecordingButtonDelegateNoteModeAudioTrack` versus their `…MidiTrack`
twins — which is exactly the split §11.4 measured on the surface.

And it names a great deal this survey never reached, each a pointer for anyone
continuing: `SampleSlicingDelegate`, `SampleEditMenuDelegate`,
`BouncingDelegate`, `CaptureDelegate`, `ChokeParameterDelegate`,
`RegionStartParameterView` / `RegionEndParameterView`, `LedBrightnessDelegate`,
`RefreshRateDelegate`, `MidiClockModeMenuDelegate`,
`UsbAudioOutputSourceDelegate`, `LinkSettingsDelegate`, `TrackColorsMenuDelegate`,
`SongColorsMenuDelegate`, `ResetMoveDialogDelegate`, `InputGainDelegate`,
`FixedMonitoringTrackDelegate`, `MaxRecordingLengthViewDelegate`.

**Reading the binary is the cheapest enumeration in this document** — 675 class
names for one `strings` run, no device state touched, nothing to clean up.

---

## Where this map stops

**Three channels were opened: the control surface (measured), Move Manager (read
side measured, write side enumerated from Ableton's own source rather than
tested), and the firmware image (read-only).** What remains is named, with its reason —
but note what §11 cost: the previous version of this sentence said "everything
reachable" while an entire second channel had not been *considered*. A channel
you have not thought of does not appear in a gap list. Treat the list below as
"what we know we do not know".

| Gap | Why it stops here |
|---|---|
| **Creating** an audio track | Ten surface routes (§9.2) all fail, and Move Manager has no endpoint that edits a set's contents (§12.2). **Observing one is done** (§11.4). One untried route remains: uploading an `.ablbundle` that already contains an audio track (§12.2) — enumerated, not attempted. |
| What CC 40 / CC 43's pulses represent | **Closed across three named channels**: the control surface (nine variations, §10.3, §11.6), the Move Manager API (§11–§12), and the firmware image (§13.3 — stripped `.symtab`, no animation class, no track-button LED delegate). Precisely characterised, not attributable. |
| ~~What the Set Overview step row encodes~~ | **SOLVED in §13.2** — it is the Shift shortcut layer painted persistently, with step 1 at `122` because you are already in Set Overview. Four sets, byte-identical; the firmware has no Song-Overview steps delegate. |
| What the three Sampling settings items DO | Reached and **rendered** (§11.7) — a mic, a bar, a boxed waveform — but changing one and observing an effect was not done. |
| A ninth context for Shift+Step 4/12/13 | Dead in eight (§8.1, §9.6) spanning both trap categories. Further contexts are unenumerable. |
| The upper bound of *every* per-step parameter | One measured to its clamp (§9.3, Grain Size 0–300 ms). The rest are device-defined. |
| Move Manager's write endpoints | **Enumerated in full from Ableton's own source map** (§12.2) and deliberately not invoked (§12.4): firmware update and reboot, update channel, ssh key install, feature flags, whole-set upload/delete/rename, render, cloud linking, clock. A **scope decision, not a device limit** — payloads are documented and any of them is reachable with explicit permission. One reversible create/rename/delete round-trip WAS exercised. |

That table is the ceiling. **Two of its rows are a scope decision rather than a
device limit** — Move Manager's write endpoints, and the `.ablbundle` upload
route to an audio track — and they say so, because "we chose not to" and "it
cannot be done" are different facts and only one of them is about Move. The
**Not known** section below is the longer running list of everything else that
was never nailed down; read both.

This document does not claim to be complete, and those two lists are the reason
it does not. Several of its own headline claims were overturned by later
measurement — CC 118 **three times**, the Mute layer, the Set Overview step row,
the "empty step is 98" rule, the idle-animation count, and "the device has no
audio track to observe" — **every one of them by running a thing that had been
asserted rather than measured.** Prefer a measurement to anything written here,
including this sentence.

One stale fact in this document is worth more than the map is: a claim written
from a device whose set has since changed will read as a device behaviour. When
something here disagrees with the instrument in front of you, the instrument is
right.

**And the honest summary of the whole exercise:** every conclusion that was
overturned had been *asserted from one observation and then built on*. The
method that worked — every time — was to vary the thing the claim depended on
and look again: a second set broke CC 118, a second track type broke half the
"in Note mode" rules, a second hypothesis class closed the idle pulse, counting
per-CC instead of filtering broke the animation count, and reading the sets off
disk broke "there is nothing to observe". None of those needed new tooling. They
needed one more variation.

---

## Not known

Untested. A driver must not assume any of it.

- **Does a jog/knob delta greater than 1 move more than one item?** Only `01`
  and `7F` were proved to register. A `03` and a `7D` were sent, but from a
  position where the list clamped, so they prove nothing. Likewise **whether
  repeated identical packets coalesce** — every repeat test was run against a
  clamped two-item list.
- **What every NOT-EXERCISED Move Manager endpoint actually does** (§12.4). They
  are enumerated with their payloads; none was invoked, by choice.
- **Whether uploading an `.ablbundle` containing an audio track is a route to
  creating one.** `POST /api/v1/data/Sets` accepts set bundles (§12.2); it was
  not attempted, so "no route creates an audio track" remains a statement about
  the **surface** plus the ten routes in §9.2.
- **How to revoke the 30-day Move Manager token.** Nothing in the API does it
  (§12.5); whether a reboot or update clears it was not tested.
- **When CC 118's lamp changes, and why it is set-dependent.** It is the
  Sampling button's availability lamp (§9.1) and it did not fire at all on the
  mode toggles of a second set (§11.4). What governs Sampling's availability per
  set was not established.
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
- **Shift + Step 4, 12, 13 in a NINTH context.** Dead in eight (§8.1, §9.6),
  spanning both categories that caught the step-8/10 miss — a track type and a
  modal state. Further contexts are unenumerable.
- **What the two idle pulses REPRESENT.** CC 43 blue at ~37.6/s, CC 40 orange at
  ~12.4/s, both gated on the transport being stopped (§10.3). Not attributable
  from **any of the three channels**: nine behavioural variations on the surface
  (§10.3, §11.6), the Manager API (§11–§12), and the firmware image (§13.3 —
  stripped symbols, no animation class, no track-button LED delegate). Untried
  fourth channels: the DSP binaries in `/opt/move/Dsp/`, and D-Bus.
- ~~What the Set Overview step row encodes~~ — **answered in §13.2**: it is the
  Shift shortcut layer, painted persistently.
- **What the three Sampling settings items DO.** Reached in §10.2 and
  **rendered** in §11.7 — a microphone, a level bar, a boxed waveform — but none
  was changed and observed, so the readings are descriptions, not functions.
  Move's screen-reader SSE (§11.2) would name them if Move's own screen reader
  were switched on.
- **CC 87 (Sampling).** Nothing at all on injection, tapped or held. Either it is
  not the Sampling button or an injected CC 87 is filtered before Move sees it;
  not distinguished.
- **How a per-step value is STORED.** §8.2 enumerates the parameters and proves
  per-step independence, but nothing was read back out of `Song.abl` to see the
  on-disk representation.
- **Whether the Set Overview pads have any structure worth naming** (banks,
  scrolling, ordering). Only "a pad loads the set under it" is established, and
  every further probe costs a set switch (§8.6).
- **The one unexplained Menu observation.** §8.4 re-ran it 18 times under
  control and Menu toggled every time; the single earlier "no CC 118" came from
  an uncontrolled state after a random walk and **could not be reproduced**. It
  is recorded rather than dismissed, and it is why "Menu twice" is still not
  recommended as a probe.
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
- **CREATING an audio track.** Ten surface routes (§9.2) and Move Manager
  (§11.2 — it has no set-editing API) all fail. *Observing* one is no longer a
  gap: §11.4 sweeps two, loaded from a factory set. The claim that "the device
  has none to observe" was **wrong** — three of its eight sets contain audio
  tracks and none of them had been loaded.
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
 },
 {
  "action": "read_automation_mask",
  "packets": "0BB0587F s1200 0BB05800",
  "note": "hold Mute; CCs 71-78 answer with a per-track mask",
  "observe": "127 = that knob's parameter has per-step automation somewhere in the clip, 0 = it does not. Proven by flipping one bit on demand (section 8.3). No SysEx decoding needed.",
  "state": "unchanged"
 },
 {
  "action": "double_loop",
  "packets": "0BB0317F s80 09901E77 s120 09801E00 s80 0BB03100",
  "observe": "OLED 'Loop doubled'; the step row grows",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "quantize_clip",
  "packets": "0BB0317F s80 09901F77 s120 09801F00 s80 0BB03100",
  "observe": "OLED 'Clip <n>% Quantized'",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "read_step_content",
  "packets": "0BB0317F s350 0BB03100",
  "note": "the Shift-release repaint doubles as an instant clip readback",
  "observe": "step Note-On d2 == 122 means that step carries a note FOR THE SELECTED VOICE; any other value is empty (the empty value is a per-track colour index: 98, 112 and 124 all observed). Never test 122-vs-98.",
  "state": "unchanged"
 },
 {
  "action": "sampling",
  "packets": "0BB0767F s110 0BB07600 s1200 0990<pad>50 s110 0980<pad>00",
  "note": "CC 118 is the Sampling button (found by sweeping every CC). Destination is a PAD only.",
  "observe": "OLED 'Press pad' + a microphone icon, then 'Recording...' + a live input meter. Only the 16 drum-rack pads (68-71/76-79/84-87/92-95) are valid destinations; others are ignored. Back, the destination pad, or CC 118 again all STOP it - Play (CC 85) is IGNORED and the take keeps running. No auto-stop within 16 s.",
  "state": "note_mode",
  "destructive": true,
  "result": "/data/UserData/UserLibrary/Recordings/<Set name> Rec <n>.wav, n increments per take",
  "while_armed": "knob/step/Play ignored; jog opens a 'Sampling' settings screen; Track button and Menu cancel"
 },
 {
  "action": "per_step_range_probe",
  "packets": "0990<step>50 s400 <100x 0BB0<cc>01> s800 0980<step>00",
  "note": "a per-step value is clamped to the device parameter's own range",
  "observe": "measured on Grain Size: clamps at 300 ms after ~100 detents and at 0.00 ms going down; 300 further detents move nothing",
  "state": "note_mode",
  "destructive": true
 },
 {
  "action": "idle_led_baseline",
  "packets": "",
  "note": "subtract this before reading any event count",
  "observe": "with NO input Move writes CC 40's RGB ~12 times/s (orange, r 32-48). It STOPS entirely while the transport runs, so counts taken stopped and running are not comparable.",
  "state": "unchanged"
 },
 {
  "action": "idle_led_baseline_v2",
  "packets": "",
  "note": "supersedes idle_led_baseline - there are TWO animations",
  "observe": "with NO input, CC 43 pulses blue ~37.6/s and CC 40 orange ~12.4/s, ~50 events/s together. BOTH stop completely while the transport runs. Subtract this before reading any event count, and never compare a count taken stopped with one taken running.",
  "state": "unchanged"
 },
 {
  "action": "move_manager_authenticate",
  "packets": "HTTP, not MIDI",
  "note": "Move Manager is Ableton's own web app on port 80 - a SECOND control channel, distinct from Schwung's manager on 7700",
  "observe": "POST /api/v1/challenge with Content-Type: application/json and body {} -> 200 and a six-digit PIN appears on Move's OLED in a large font. POST /api/v1/challenge-response {\"secret\":\"<pin>\"} -> 200 + Set-Cookie Ableton-Challenge-Response-Token (Max-Age 30 days). A plain POST with no JSON content type returns 400 with an EMPTY body. A wrong secret is 401 with X-Retries-Left; three tries then 429.",
  "state": "authenticated for 30 days"
 },
 {
  "action": "move_manager_read",
  "packets": "HTTP GET with that cookie",
  "observe": "/api/v1/system/version (firmware + build), /is-move-running, /datetime, /system/update-channel, /cloud-auth/status, /feature-flags/{current,schema}, /files/ (a JSON listing of UserLibrary), /screen-reader (SSE of the screen-reader TEXT - silent unless Move's own screen reader is on). There is NO set/track/clip editing API.",
  "state": "unchanged"
 },
 {
  "action": "load_a_set_with_audio_tracks",
  "packets": "0BB0317F s80 09901077 s120 09801000 s80 0BB03100 s1800 0BB0337F s90 0BB03300 s1200 0990<pad>50 s110 0980<pad>00",
  "note": "track kind is tracks[].kind == 'audio' | 'midi' in Song.abl. On this device BNYX Demo 3, Jose Castillo, Alice Ivy and Heavy Mellow carry audio tracks; Set 3 and BNYX Demo 1/2/4 do not.",
  "observe": "the set name on the OLED changes; allow ~3 s",
  "state": "that set loaded",
  "destructive": true
 },
 {
  "action": "audio_track_surface",
  "packets": "n/a - observations",
  "observe": "On an AUDIO track: pads are INERT (no LED at all), knobs open no parameter overlay, there is no per-step automation, the Mute automation mask is not transmitted, the step row is uniformly 126, the Shift layer loses its Note-only lamps (25/26/30/31), hold-step+jog reads 'Empty Audio Clip', and Sampling is still available. CC 118 did NOT fire on mode toggles in that set at all.",
  "state": "unchanged"
 },
 {
  "action": "move_manager_enumerate_api",
  "packets": "GET /assets/index-<hash>.js.map",
  "note": "the app ships a 6.4 MB source map WITH sourcesContent - the original TypeScript of the API client, ~19 files under packages/api-client/src/",
  "observe": "every endpoint, verb and payload shape, read without touching the device. Strictly better than probing.",
  "state": "unchanged"
 },
 {
  "action": "move_manager_options",
  "packets": "OPTIONS /api/v1/files/<path> | /api/v1/data/<bucket>",
  "note": "only the files and data trees answer OPTIONS; everything else 404s on it",
  "observe": "Allow: per-directory verbs, plus Allowed-Post-File-Extensions and Allowed-Post-File-Size (100000000 on Samples). Per-directory write permission is discoverable WITHOUT writing.",
  "state": "unchanged"
 },
 {
  "action": "move_manager_file_write",
  "packets": "POST /api/v1/files/<path> (no body) ; DELETE /api/v1/files/<path>",
  "note": "the only write exercised - create and delete a throwaway directory, fully undone",
  "observe": "POST 200 creates a directory; DELETE 200 removes it. RENAME is PATCH /api/v1/files/<base>/<oldNamePercentEncoded> with {\"path\":\"<base>/<newName>\"} - the old path is percent-encoded and the new one is NOT.",
  "state": "unchanged if you delete what you create",
  "destructive": true
 },
 {
  "action": "read_firmware_class_names",
  "packets": "strings -n 6 /opt/move/MoveOriginal",
  "note": "READ ONLY. The binary is stripped of .symtab, but RTTI class names survive in .rodata - 675 distinct ableton::move classes, plus one mangled symbol containing Move's ENTIRE view tree in construction order.",
  "observe": "grep -oE 'NS0_[0-9]+[A-Za-z]+' | sed -E 's/^NS0_[0-9]+//' | sort -u. Also assert strings carrying source paths (products/move/MoveLib/src/...). Cheapest enumeration in this document; touches no device state.",
  "state": "unchanged"
 },
 {
  "action": "set_overview_step_row",
  "packets": "0BB0317F s80 09901077 s120 09801000 s80 0BB03100",
  "note": "the row is the SHIFT SHORTCUT LAYER painted persistently, not a Set Overview indicator",
  "observe": "step 1 = 122 (you are in Set Overview), the other shortcut steps = 124, steps 4/12/13 dark because those three shortcuts do not exist. Byte-identical across four loaded sets. The LOADED SET is marked on a PAD, channel 9.",
  "state": "set_overview"
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
Sampling 118=0x76 ('Press pad' -> pad -> records)   CC 87: no observed effect
Steps 1-16 = notes 16-31 = 0x10..0x1F
Pads = notes 68-99 = 0x44..0x63
```
