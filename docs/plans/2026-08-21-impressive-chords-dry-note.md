# impressive-chords: the dry note, read at last

Handoff §2.3 left this as *"the one genuinely unexplained thing"*, with the note
that nobody had read `process_midi` yet. It has now been read, along with every
route from a pad to a sound on the Schwung side.

**The chain host is exonerated a second time, and this time exhaustively. The
leading explanation is that the extra note is not leaking through Schwung at all
— it is Move's own track, playing the pad, on the cable-0 path the chain never
sees.** One 20-second experiment discriminates it (§3). Do that before writing
any code.

## 1. What `ic_process_midi` actually does

`_community_clone/schwung-impressive-chords/src/dsp/impressive_chords.c:451`.
It is a `midi_fx_api_v1_t` (`move_midi_fx_init`), so it runs through
`v2_process_midi_fx`.

For a Note On it does exactly one thing: `return trigger_chord(...)`. There is no
path in which it copies the input note into its output. The only passthrough in
the whole function is the final

```c
// Pass through for everything else
memcpy(out_msgs[0], in_msg, in_len);
```

which is unreachable for note events — every note status is consumed by the two
branches above it. Clock (`0xF8`) returns 0 or a retrigger. So **the plugin never
emits the dry note.**

Nor does it emit it indirectly. `trigger_chord` builds every voice from
`chord->notes[idx] + transpose`, a preset table indexed by
`chord_idx = note - base_note`; the input pitch is used only as a table index and
as the key of `active_notes[]`. And the plugin calls no host MIDI callback at all
— confirmed fleet-wide in `2026-08-21-create-instance-thread-spike.md`, and by
grep here: `impressive_chords.c` contains no `midi_send_*` / `midi_inject_to_move`
call site.

Two `return 0` paths are worth knowing because they are the *opposite* of a leak
— out-of-range notes go silent, not through:

```c
int chord_idx = note - inst->base_note;
if (chord_idx < 0 || chord_idx >= 48) return 0;      // pad below base_note, or 48+ above
if (!g_presets || ... ) return 0;                     // presets failed to load
```

## 2. What the host does with a zero-output stage

`v2_process_midi_fx` (`src/modules/chain/dsp/chain_midi.c:363`) accumulates
`next_count` only from what stages return, and the final copy loop is bounded by
`current_count`. A stage that returns 0 leaves `current_count == 0` and the
function returns 0. The caller (`:810`) then runs a loop `for (i = 0; i < 0; i++)`
to the synth. **There is no leniency, no fallback, no "if the FX produced nothing,
pass the original".** This is the second confirmation; the first is recorded in
the handoff.

I also checked every other route from `v2_on_midi` to the synth. There are five
`synth_plugin_v2->on_midi` call sites in the chain host; four are the MIDI-FX
output loop, the tick-generated loop, a mod-reset, and the CC path. The fifth,
`inst_send_note_to_synth` (`chain_midi.c:636`), takes a raw `msg` and would be a
perfect suspect — **but it has no callers.** It is dead code and should be deleted
(see §4).

## 3. The hypothesis, and the experiment that settles it

Pressing a pad puts the note on cable 0, where **two** things happen: the chain
slot receives it (and swallows it, per §2), and **Move's firmware receives it and
plays the selected track's own instrument**. Schwung cannot swallow the second —
that is the whole reason `midi_fx_pre_mode`'s inject path carries a "root-match
skip" comment about Move's pad path already having triggered the note.

Why this shows up on impressive-chords and not on the built-in `chord`: the
built-in emits the played note as the chord's root, so a doubled pad note is
inaudible as a separate event. impressive-chords maps the pad through
`note - base_note` into a preset table, so its chord has **no pitch relationship
to the pad at all** — and the pad note becomes glaringly, obviously wrong. Same
mechanism, only one of them audible. That asymmetry is what has made this look
like a plugin bug for weeks.

**The discriminator** — takes about twenty seconds on the device:

1. Play a pad with impressive-chords loaded and listen to the extra note.
2. Does its pitch track the **pad** (chromatically, one semitone per pad) or the
   **chord's base**? Pad ⇒ it is the cable-0 route, not the chain.
3. Mute Move's own track (Mute + Track), or select an empty Move track, and play
   again. If the extra note disappears while the chord remains, the diagnosis is
   confirmed and there is nothing to fix in either repo — it becomes a
   documentation and defaults question.

If instead the extra note survives a muted Move track, the hypothesis is dead and
the next step is instrumentation, not more reading: `spi_midi_log` on the slot's
recv channel, which will name the route directly. **Do not accept a third
plausible mechanism without that trace** — handoff §3 records that two confident
wrong diagnoses were produced here before the bisect discipline was applied, and
this document is a hypothesis, not a result.

## 4. Two things to fix regardless

**Report to mestela** (independent of the above, and already known):
`ic_get_param` returns the literal string `"[]"` for `chain_params` when
`chain_params.json` is missing from the install path
(`impressive_chords.c:931`). A degenerate contract is worse than no contract —
`"[]"` reads as "this module declares no parameters", which the shadow UI is now
careful about but should not have to be. It should ship the `chain_params.json`
or `return -1`.

**Delete `inst_send_note_to_synth`** (`src/modules/chain/dsp/chain_midi.c:636`).
It is unreferenced, and it is precisely the shape of the bug being hunted here —
a raw-`msg`-to-synth call that bypasses the FX chain. Leaving it in the file
costs a future investigator the same hour it cost this one.
