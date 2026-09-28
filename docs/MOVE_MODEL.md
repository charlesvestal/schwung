# Move's live song model — read out of its own memory

**Measured 2026-09-28 on Move 2.1.0 (build-id `fc05b06d…`, commit `a6233f89a28a`).**
Reader: `src/host/move_model.{h,c}`. Probe kit: `tools/move-model/*.py` (run on
the device as root).

## Why this exists

Automation lanes need to know, *at the moment of a gesture*, which clip is
being edited, how long it is, where its loop sits, which page of it the step
editor shows, and where playback is inside it. The first lanes build
reconstructed that from four lossy signals — session-pad LEDs (Session view
only), step-strip pixels (a one-bar clip is ambiguous), `Song.abl` (**~10 s
stale**, nothing at all for a new clip) and note fingerprints (absent until a
clip has notes) — and was still wrong in the flow it exists for. It was removed
from main in #528.

None of that is necessary. **The shim runs inside MoveOriginal, and Move's set
lives in MoveOriginal's heap as a flip document.** Everything the lanes need is
there, current to the edit.

## What Move's model is

`flip` — Ohm Force's (now Ableton's) C++ data-model framework, public at
`github.com/ohmtech/flip-public`. The binary's own strings name the source
tree: `shared/live-model/FlipModelLib/src/FClipRegion.cpp` — Live's shared
model. The classes Schwung cares about:

```
live.Song          mTransport mTracks mScenes ... mStepEditorResolution (Enum)
live.Transport     mTempo(Parameter) mTimeSignature mLoop mTransportControlMessage ...
live.Track         mComponents(Collection) mTrackType mLabel mIsSelected ...
live.Clips         mClipSlots(Array) mPlayingState ...        <- a Track component
live.PlayingState  mMode(Enum) mPlayingClipSlot(ObjectRef) mSessionClipStartTime(Float)
live.ClipSlot      mClip(Collection: 0 or 1 SessionClip) mHasStop
live.SessionClip   mClip(Clip) mClipEnvelopes
live.Clip          mLabel mClipRegion mTimeSignature mContent(Collection) mpGroove mIsEnabled
live.ClipRegion    mStart mEnd mLoop
live.Loop          mStart mEnd mIsEnabled
live.MidiClipContent  mStepEditorScrollPosition mNotes(Blob) ...
```

`tools/move-model/allcls.py` dumps all 93 registered classes with members and
offsets (`classes.json`), and `fdump.py` walks the whole live tree into a flat
`path -> value` JSON (~370 keys, 46 ms in Python). `fdiff.py a.json b.json`
between two gestures is how every fact below was found.

## How the reader resolves it — BY NAME

flip keeps a runtime registry: one `flip::ClassBase` per class, carrying the
class name and a vector of members, each `{type, std::string name,
Representative{vptr, stub}}` (48 bytes). The stub is a two- or three-
instruction accessor — `add x0, x0, #off; ret` or `mov w8, #off; add x0, x0,
x8; ret` — so **decoding the stub yields the member's byte offset**. The reader:

1. finds each `ClassBase` in one pass over the first 64 MB of the heap (a
   `{vptr, name}` pair, both in the image, name matching);
2. resolves every member it needs by name, following `super` (e.g.
   `live.Track.mComponents` lives on `live.Components`);
3. finds the few C++ vtables it tests against via RTTI (`typeinfo` name →
   `typeinfo` → primary vtable, offset-to-top 0);
4. finds the `FSong` by scanning the heap for its vptr, validated by a sane
   tempo.

flip value slots (measured): a `Type` ends at +0x64; `Bool` value at **+0x64**,
`Float`/`Int`/`Enum` at **+0x68**, an `ObjectRef`'s target object id at
**+0x80**, an object's own id at **+0x28**. Containers are libc++ `std::map`s:
node `{left, right, parent, color}` then the key (KeyFloat for Array,
KeyRandom for Collection — different sizes), then `ObjectWrapper{vptr, T*}`.
The element is the word after the first image pointer that follows the key's
own vptr.

**The step-grid enum is read the same way**: `flip::EnumClass
"StepEditorResolution"` carries `{name, value}` pairs — `0=1/8t 1=1/16 2=1/16t
3=1/32 4=1/32t 5=1/64` — and the reader parses the names, so no table is
copied into Schwung.

## What each question is answered by

| Question | Field | Measured behaviour |
|---|---|---|
| Which track is being edited | `Track.mIsSelected` | follows a Track press immediately |
| Which clip | that track's `PlayingState` | `mode 1` + `mPlayingClipSlot` = the playing clip; with the transport stopped, the **selected** one (this is what `Song.abl` calls `isPlaying`) |
| — an empty slot picked | `mode 2`, ref null | a step press then creates the clip there, and `mode 1` points at it, in the same tick |
| — a clip that has never played | `mode 0` | |
| Clip length / loop / start marker | `ClipRegion.mStart/mEnd`, `.mLoop.mStart/mEnd/mIsEnabled` | Double Loop showed `0..4 -> 0..8` instantly; the file took 10 s |
| Which page | `MidiClipContent.mStepEditorScrollPosition` (beats, per clip) | followed Track switches and arrow presses immediately |
| Step length | `Song.mStepEditorResolution` via the enum | all six values tracked live through the Step Grid dialog |
| Clip identity | the SessionClip's flip object id | unique per clip for the life of the loaded set; a new clip gets a new id |
| When the clip started | `PlayingState.mSessionClipStartTime` (beats) | **the quantised launch boundary, exactly**: pressed at 4.76 → `8.0`; pressed at 13.5 → `16.0`. Every Play resets all to `0.0` |
| Track mute / solo / volume / pan | `Track.mTrackMixerDevice` → `AudioMixerParameters.mSpeakerOn/mSolo/mVolume/mPan` | instant; see below |
| Transport run state + position | `Transport.mTransportControlMessage` +0xb0 (0/1) and +0x158 (double beats) | **build-pinned**, see below. ~20 ms update. Resets to 0 on every Play, holds on Stop |

**Clip position** is then `region_start + (song_beats − start_beats)`, wrapped
into `[loop_start, loop_end)` once past `loop_end` (`mm_clip_position()`).

**A held step's clip time** is `scroll + button × step_beats`, where the button
is the hardware step note (16–31) — the one input that stays a button read,
because it is a finger, not an inference. On a triplet grid every fourth button
is dead (12 steps per page).

### "Move has no song position"

Correct in the UI sense — there is no arrangement. `song_beats` is the
transport clock: beats since Play, restarting at 0 on every Play. It is what
clips launch against, which is exactly why launch times are expressed in it.

### The queue window — the model and the step editor agree

While playing, launch clip B on a track playing A: `PlayingState` changes **at
the launch boundary** (song 7.997, start 8.0), not at the press. Move's step
editor does the same: in the queue window, re-entering Note mode sent no step
LEDs for B's notes; from the boundary the playhead restores painted B's
pattern. So "what the editor shows" and "what `PlayingState` says" are the same
fact even mid-queue. The *queued* clip itself is not in the document (it is
engine state); nothing in the lanes needs it.

### The Song object survives set loads

Loading another set from Set Overview replaced the contents **in place** — same
`FSong` address, new tempo, new clips. The reader re-validates the vptr every
tick and re-finds the object if it ever fails (`refind=` in the log); across
two set loads it never had to.

## What Schwung does with it — mute/solo and set alignment

`src/host/move_model_sync.{h,c}` is the consumer, a listener on the reader
thread.

**Mute / solo follow Move's mixer directly.** `Track.mTrackMixerDevice` holds
a `live.AudioMixerParameters` with `mSpeakerOn` (1 = audible) and `mSolo`
(`solo-cue`) — Mute+Track flipped `speakerOn` 1→0→1 and Shift+Mute+Track
`mSolo` 0→1→0, instantly. On a NEW DOCUMENT (a set load, or the first snapshot
after boot) all four slots take them as LEVELS; after that only EDGES are
applied, so Schwung's own slot-mute controls (slot settings, E16, CC map) still
hold between Move gestures, as #540 specified. Measured on hardware: slot
mute/unmute within ~200 ms of the gesture, and Move's exclusive solo handed
T3 → T4 → none edge for edge.

It replaces two inferences, which remain ONLY as the fallback on a firmware
the model cannot resolve (`move_model_sync_active()` gates them): the D-Bus
`"<name> muted"` text paired with a gesture window (`mute_follow.h`), and the
Song.abl read at set load — which is only ever the last save. The SPI-thread
optimistic toggle (Mute+Track, which also wrote the state file on the SPI
callback) is gone under the model, and the per-set chain config no longer
overrides the mixer when a set loads.

**Set changes land in ~10 ms, not ~3 s.** A set load replaces the document:
the new tracks are inserted before the old are removed, over ~180 ms, and
Move rewrites `Settings.json`'s `currentSongIndex` within ~12 ms of the swap
completing. The model's edge (a new hash of the four track ids) runs the
identity poll at once, and the SPI-side consume went from every 500 frames to
every 16. Measured: press → document edge 0.35 s (Move's own load) → SET_CHANGED
7 ms later → Schwung switched and aligned 0.6 s after that (its slot reload).

**Alignment gates autosave.** `shadow_control_t.move_doc_gen` is the document
Move has loaded; `set_doc_gen` is the one Schwung's per-set state belongs to.
While they differ, `shadow_ui` refuses periodic autosave — the old detection
window was exactly how state landed in the outgoing set's folder. A reload of
the SAME set (a new document, same name — which the index poll never saw)
aligns in C once a read is known to postdate Move's rewrite (≥ 300 ms after
the edge); a different set aligns when the UI acknowledges its SET_CHANGED
with the `set_aligned` key. The poll republishes while misaligned — without
that, a boot never aligned, found on hardware.

**A new, unsaved set keeps its state.** Move writes a new set's folder only on
its first save, so until then it runs under a synthetic `__pending-*` id —
and whatever was configured there used to be orphaned when the real UUID
appeared (eight such folders on one device). Now, if the model shows it is
the SAME document (no set load in between), the pending folder's whole
contents move to the real UUID. Verified with a positive-control file: slots,
sends, snapshot and the marker all arrived; the pending folder was removed.
Pending ids also carry a per-boot token now: the sequence restarted at 1 every
boot, so `__pending-26-1` named a different unsaved set each session and a new
one silently loaded an old one's leftovers.

## Automation lanes on the model

`shadow_slot_clip_phase()` and `shadow_lanes_step_phase()`
(`shadow_chain_mgmt.c`) answer from the model; the ~365-line LED / step-strip /
Song.abl resolver they replaced is gone. The chain seam
(`chain_set_clip_phase`) is unchanged.

- **Clip** = the track's `PlayingState` clip; **loop** from its region;
  **phase** = `mm_clip_position(clip, start_beats, now)` in CLIP time, where
  `now` is the shim's per-block interpolated MIDI-clock position (the model's
  own beat clock refreshes every ~20 ms, too coarse to drive a parameter).
- **Fingerprint** `{loop_start, loop_len, notes_len, notes_hash}` — the content
  half is a hash of Move's notes blob, so it is always valid: there is no
  "blind take" and no adoption any more. A take lands on the clip it was made
  on, at the moment it is made.
- **A held step** is `scroll + step × step_beats` (triplets skip the dead
  fourth button); refused past the clip's end, pending with no current clip.
- **Deletions and copies** come from diffing the model (`move_model_sync.c` →
  the worker's clip-event channels): a deleted clip ORPHANS its lanes at once
  (it waited for Move's save before — long enough for a clip made in the same
  slot to inherit them); a clip that arrives with the same notes and geometry
  as one on its track is a COPY, and its lanes are copied.
- **Orphans re-attach on Undo** — Move's Undo restores the same clip object
  (measured: same flip id), its fingerprint matches, and the lane plays again.
  They are **not written to disk**: undo history does not survive a reload,
  and a written orphan came back looking live, so a new clip in that slot
  inherited the dead clip's automation.

Measured on hardware (hank on Set 5, T2, 117 BPM):
- p-lock on a clip made one second earlier, stopped: accepted (`ok`), bound to
  the new clip, and heard on step 5 every loop (0.9 for phase 1.00–1.25, base
  elsewhere);
- the model phase against Move's own step playhead: **143/143 LED events in the
  right step**, offset +1 or +2 clock pulses (the LED's latency), including a
  clip launched mid-playback at beat 8;
- a recorded knob sweep (45 points) played back at median error 0.002 over two
  loops;
- delete → `orph=1` at once; Undo → re-attached and driving; Copy → the
  duplicate got a 45-point copy of the lane.

`move_model_on` also writes `phase_model.log`: one line per step-playhead LED
with the model's position and the offset `d` from the lit step's start.

## Automation follows Move's edits

The rule: Schwung automation behaves as part of Move's clips, pages and steps.
Whatever Move does to one of those, the automation on it does too -- and it
must never be possible to see the two disagree.

### What Move does to its OWN automation (the oracle)

Move's per-step automation lives in the document too
(`SessionClip.mClipEnvelopes` → `ClipEnvelope.mAutomation{mpParameter,
mBreakpoints}`, breakpoints a blob of big-endian `(time, value)` doubles). So
every rule below was measured by driving the gesture and watching what Move did
to its OWN envelopes and notes (with per-note ids, see "The notes blob"):

| Move edit | notes | Move's automation → ours |
|---|---|---|
| step paste (Copy, A, B) | per PITCH: a source note replaces a same-pitch note, else is added (melodic); drum tracks copy only the SELECTED voice | the destination step's automation is **replaced** by the source step's -- every parameter, including ones only the destination had |
| page paste (Loop + Copy, pages) | same, per page | same, per page |
| a source step with no notes | nothing | nothing -- automation-only steps count as empty |
| Delete + step | removed | **kept** |
| Double Loop (Shift + step 15) | duplicated (new ids) | duplicated |
| Undo / Redo | exact previous note ids | exact previous automation |
| clip delete / Undo | the SAME clip object returns | ours comes back with it |
| clip duplicate (Copy in Note view) | new clip, same content | copied |

Move's own step lock is a one-step RECTANGLE when pasted (the original can span
to the next note: `(0,36)(0,41)(1,41)(1,36)`), the same shape as a Schwung
p-lock.

### How it is mirrored -- intent from the buttons, proof from the model

`edit_gesture.c` (RT) reads Copy / Loop / Shift / step / Undo off the hardware
buffer and reports INTENT -- pairing presses exactly as Move does (the source
survives releasing Copy; Shift + Undo is Redo; Shift + step 15 is Double Loop)
-- with positions taken at the moment of each press, from the model: a step is
`scroll + step × grid`, a page is `page × 16 × grid` (12 on a triplet grid).

`edit_follow.c` (model thread) issues a lane verb only when the MODEL confirms
Move made that edit:
- a **paste** when the notes that appeared in the destination span (ids new
  since the clip's previous state) are exact copies of source-span notes --
  pitch, relative start, length, velocity. Anything else is declined: Move
  refusing an "empty" source, a range selection, an armed source Move had
  cleared, a note the user played. The gesture model's unmeasured corners can
  therefore only produce an intent that is not confirmed -- a no-op;
- a **Double Loop** when the loop really doubled (geometry, since Move doubles a
  clip with no notes too);
- **Undo/Redo** when the clip returns EXACTLY to its state (`mm_clip_state_hash`:
  notes + Move's envelopes + loop geometry) before/after a mirrored edit;
- clip **delete → stash** under its id, **same id back → unstash**, **new id with
  a sibling's content → copy** -- from identity alone, no intent needed.

The chain verbs (`lanes:paste_span`, `lanes:journal undo|redo`, `lanes:stash`,
`lanes:unstash`, `host/lane_edit.c`) journal what they replace, so undo and redo
restore it exactly. Commands cross to the SPI callback through a lock-free ring.

The edited clip is probed EVERY tick (its notes and envelope blobs re-hashed,
a few hundred bytes): a paste onto an occupied step replaces a note without
resizing Move's notes vector, so no structural guard moves.

### Verified on hardware (Set 5, hank on T2, a throwaway clip)

step paste to an empty step, onto a step holding only a Schwung lock (cleared,
as Move does), Undo, Redo; Double Loop + Undo + Redo; page paste on a
three-page clip + Undo; clip delete (stashed, nothing orphaned) + Undo (the same
clip id, both lanes back); duplicate (both lanes copied); a paste Move declined
(nothing changed). Every one matched Move's notes, and nothing was mirrored that
Move did not do.

### A drum paste is VOICE-scoped

Move pastes only the selected voice's notes on a drum track, so only that
voice's automation may follow -- copying the whole step would put a snare's
lock on a step that received only a kick, and overwrite the destination's own
snare lock with it.

- **The voices come from the notes, not the buttons.** `edit_follow` appends the
  distinct pitches of the notes that APPEARED to the command
  (`lanes:paste_span ... v=36`). It does not need to know which voice is
  selected, and on a melodic track it names pitches the chain simply ignores.
- **Which parameter is whose comes from the module's declaration**, through
  `voices.mjs` and `child_key.mjs` (`src/shared/lane_voice_map.mjs`), never a
  C copy. A pad spread over several child levels (dr32: Sample, Shape, Mix,
  where only Sample declares notes) is merged by the shared
  `child_index_param`. A key EVERY voice lists (`ui_current_pad`, `link`) is
  the track's, and a voice paste leaves it -- and every lane on a non-voice
  parameter or an FX -- where it is.
- **The map is keyed to the synth by NAME on the chain side.** The UI asks
  `lanes:voice_map_need` (one read per ~45 ticks, one slot per pass) and reads
  the hierarchy only when a synth has no map -- once per load. A push naming a
  synth that is no longer loaded is refused, a stale map never scopes, and a
  failed hierarchy read pushes nothing rather than "not a rack".
- No map, an empty map, or a map too big to carry (>16 KB, whole or not at
  all) is a **whole-step** paste. Double Loop is always whole.
- `lanes:paste_scoped` reports whether the last paste was scoped.

## One Undo for Move's edits and Schwung's

Move's Undo undid Move's last edit, and a take or p-lock made in Schwung is not
one -- so Undo after recording automation undid the NOTE edit before it and
left the automation. There is one instrument, so there is one history.

**Move's side is read, not modelled.** Move's undo stack is flip's own
`History<HistoryStoreMemory>` (the strings name `mTransactionHub.mHistory`):
a libc++ `std::list<Transaction>` plus a redo iterator. The reader finds it
once by its two vptrs (History at +0, its store at +0x10) and each tick reads
the last-undo and first-redo NODES with their transaction numbers
(`mm_history_read`; node+0x10 is vptr-checked as a `flip::Transaction` every
time). List nodes never move, so a node plus its number IS a step's identity.
Every change of that pair is exactly one of: Move UNDO (the new first-redo is
the old last-undo), Move REDO (the reverse), or a NEW step -- which covers
every kind of Move edit, including device knobs and anything else this reader
never models.

**Schwung's side is journaled by the chain** (`lane_journal_diff`): a take is
everything recorded between Record going solid and going out, a p-lock and
each clear verb are one step each, all stored as whole lanes before/after in
`lanes_sjournal` (ids with the high bit) and announced once through the
dlsym'd `chain_take_lane_edit`. An empty take announces nothing. A
Move-mirrored edit (paste, its undo, a stash) landing mid-take commits the take
first, so undoing the take can never revert Move's paste.

**The decision** (`undo_timeline.c`): each Schwung edit is ANCHORED to Move's
last-undo step when it was made. An Undo press is Schwung's iff the latest
live Schwung edit is anchored at Move's CURRENT last-undo -- nothing of Move's
came after it. Then the shim swallows the press AND its release
(`midi_in_swallow`, latched) and the slot gets `lanes:journal undo <jid>`.
Otherwise the press reaches Move untouched. Shift+Undo mirrors it for Redo. A
new edit on either side ends the other's redo branch where it can (ours; Move's
own stays Move's).

**A take recorded while Move recorded notes is ONE step.** If Move pushed a new
step between arm and 600 ms after the take ended, the take is LINKED to that
step: the press goes to Move, and the take follows Move's undo and redo of it
(observed from the stack, no press needed). Re-linking follows Move squashing
its recording into a fresh top node.

Fails closed: an unreadable stack (`hist_valid = 0`, unknown firmware, object
not found) claims no press, so Undo is exactly Move's as before. The chain keeps
8 own-edits per slot, and the timeline never claims one older than that.
Not hardware-verified yet -- the History candidates are logged to
`move_model_status.txt`, and `move_model.json` carries `history`.

### Known limits

- **Undo depth for Schwung's own edits is 8 per slot** (the chain's journal);
  older ones fall out of the button's reach, and Slot Settings' one-level Undo
  is still there. An edit touching more than 16 lanes (clearing a busy slot)
  is not journaled at all, so the button passes it to Move.
- **Cross-track clip copies** are not mirrored (a different slot, usually a
  different module).
- **Page copy** is Loop held + Copy + page, then RELEASE Copy before touching
  the destination page. Holding Copy through both presses is not the gesture
  (an earlier note here called page copy a no-op on two pages from exactly
  that mistake).
- Injected test presses reach Schwung's decoders only with
  `inject_as_hardware` set (offset 108) -- see `docs/DIAGNOSTICS.md`.

## Not RT, and cheap

The reader is its own SCHED_OTHER thread on cores 0–2, created from shim init.
The SPI callback may call `move_model_get()` — a seqlock copy, no syscalls.

**Every read is `process_vm_readv` on our own pid**, so a stale pointer returns
EFAULT instead of raising SIGSEGV inside MoveOriginal. Move mutates the
document on its main thread (tid == pid; the SPI ioctl is on `Audio Main/SPI`),
so a walk can race an edit: it is taken twice and published only when both
agree (`torn=` counts the discards — one, during a clip creation, in a full
session).

**Cost, measured by the thread's own `CLOCK_THREAD_CPUTIME_ID`:**

| version | cost |
|---|---|
| full walk twice per tick, 50 Hz | **~14%** of a core |
| recorded read plan, one iovec per field | 3.5% |
| plan coalesced into spans (450 fields → 95 iovecs) | **1.6%** |

The full walk records every leaf it read *and every structural word it relied
on* (container begin/size, element ids, tested vptrs) as a guard. A tick
replays that plan as one batched `process_vm_readv`; any guard that moved means
the shape changed and the next tick walks again. A full walk also runs every
10 s regardless. Halving the rate halves the cost if it ever matters.

## Firmware updates

- **Re-resolves on its own:** member and class offsets (by name), vtables (by
  RTTI name), the step-grid enum (by name). Field reorders, new members and
  object-size changes all land.
- **Depends on flip / libc++ / the compiler:** value slots inside flip's basic
  types, the `ClassBase` and `Member` layout, `std::map` node shape, the
  accessor-stub shapes. They change only if Ableton changes flip or the
  toolchain.
- **Pinned to one build:** the transport run flag and beat clock (a Move
  message struct, not a flip member). Gated on MoveOriginal's GNU build-id; on
  any other build `clock_valid = 0` and phase must come from the MIDI-clock
  pulse counter the shim already keeps.
- **Fails clean:** a missing class or member name leaves `valid = 0` and says
  which one in `move_model_status.txt`. It never reports a guessed value.
- Checkable before installing an update: decrypt the new image with
  `move-firmware-watch` and confirm the class and member names are still in
  `strings MoveOriginal`.

## Diagnostics

```
touch /data/UserData/schwung/move_model_on     # arm (writes json every 0.5 s -- disarm after)
cat   /data/UserData/schwung/move_model.json   # the current snapshot, incl. clip_pos per track
tail  /data/UserData/schwung/move_model.log    # one line per change, with torn/refind/walks/plan
cat   /data/UserData/schwung/move_model_status.txt   # ALWAYS written: resolve result, cpu while armed
```

`move_model_status.txt` exists because `unified_log` is best-effort (it drops
on mutex contention and is off unless armed) — the first deploy failed RTTI
resolution and said nothing anywhere. The cause was a chunked image scan
straddling the unmapped gap between ELF segments, which is exactly where
`.data.rel.ro` (the typeinfo) begins; scans now walk mapped segments, never the
image span.

## Not yet known

- **The notes blob.** `MidiClipContent.mNotes` is a flip Blob; small clips
  decode as 40-byte big-endian records `{i32 note, pad, f64 start, f64 dur,
  f32 vel, f32 offvel, i64 id}`, but larger ones do not — likely a different
  (compressed or chunked) encoding. Lanes do not need notes (identity is the
  object id), so it is left.
- **Audio clips' content** (`AudioClipContent`) — scroll is reported as `-1`.
- **Persistence across reloads.** Object ids are per load; a lane saved to disk
  still needs a position + fingerprint key, and the model is what makes that
  fingerprint cheap to take at any moment rather than ~10 s later.
