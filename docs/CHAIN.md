# Signal Chain — the module contract and the chain host

Split out of `CLAUDE.md`, which keeps the pipeline diagram and points here.

Covers what a chainable module must publish (`ui_hierarchy`, `chain_params`),
the chain host's file layout, and the shape-edit verbs in `chain_reorder.c`.

### Shadow UI Parameter Hierarchy

Modules expose `ui_hierarchy` (menu structure + knob mappings) via get_param:

```json
{
  "modes": null,
  "levels": {
    "root": {
      "label": "SF2", "list_param": "preset", "count_param": "preset_count", "name_param": "preset_name",
      "knobs": ["octave_transpose", "gain"],
      "params": [
        {"key": "octave_transpose", "label": "Octave"},
        {"key": "gain", "label": "Gain"},
        {"level": "soundfont", "label": "Choose Soundfont"}
      ]
    },
    "soundfont": {"label": "Soundfont", "items_param": "soundfont_list", "select_param": "soundfont_index"}
  }
}
```

- `knobs`: array of param-key **strings** mapped to physical knobs 1–8
- `params` items: string (param key), `{key, label}` (editable), or `{level, label}` (navigation)
- Preset levels use `list_param`/`count_param`/`name_param`; selection levels use `items_param`/`select_param`
- **Use `key`, not `param`**, for editable entries. Metadata comes from `chain_params`.

### Chain Parameters

`chain_params` (get_param JSON array) is **required** for Shadow UI to know step sizes, ranges, enum options:

```c
"[{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01},"
 "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\",\"options\":[\"LP\",\"HP\",\"BP\"]}]"
```

Types: `float` (min/max/step), `int` (min/max), `enum` (options). Optional: `default`, `unit`, `display_format`.

### Knob destinations — one knob, several parameters

Each of a slot's eight knobs drives up to **four** parameters (`MAX_KNOB_DESTS`),
and each destination can be limited to a **window**: a slice of that parameter's
own range.

```
knob 1  ->  synth: cutoff     0 .. 100%
            fx1: mix         20 ..  80%
            fx2: drive      100 ..   0%     (inverted)
```

`lo`/`hi` are **fractions of the destination parameter's own range**, never
values in its units. That is what makes a window portable: re-point a
destination at a parameter measured in Hz, dB or list positions and the window
still means the same thing. `lo > hi` is an inverted destination — turn the knob
up, that parameter goes down — and needs no extra machinery, because the
interpolation simply runs backwards. An `enum` destination takes a **sub-range**
of its option list, and its last option is reachable rather than one short.

#### One destination keeps its parameter's feel

> **The line is "several destinations", not "has a range".**

- **One destination**, ranged or not, is the path that has always shipped plus a
  clamp into its window. It keeps that parameter's own step size, its own
  acceleration and its own enum behaviour.
- **Several destinations** have no single parameter to be, so the knob's own
  0..1 **position** becomes the thing being turned, and each destination follows
  it through its own window.

This asymmetry is arithmetic rather than taste, and it is worth stating because
it looks like an inconsistency worth "simplifying" away. An 8-option enum spans
7 units. A position moving by `KNOB_STEP_FLOAT` (0.0015) shifts that enum by
0.0105 per detent — `4 -> 4.0105 -> (int) 4`, the same value — so it needs about
95 detents per option instead of one. A parameter that shares a knob with others
pays that by necessity. A parameter that does not must never be made to, which
is why giving a single destination a window does **not** move it onto the
position path. `tests/host/test_chain_knob_turn.c` measures both cases
side by side.

A knob's position is seeded from its first destination the moment it gains a
second, so nothing jumps at that moment. It is never re-derived from a
destination afterwards: doing so between detents would snap the position back to
that destination's own quantisation grid, and a slow turn on a coarse
destination would never advance.

#### Editing

Set-param keys, all per slot. `N` is the knob 1-8 and `M` the destination 1-4:

| Key | Value | Meaning |
| --- | --- | --- |
| `knob_N_set` | `"target:param"` | Collapse this knob to **one whole-range** destination. Unchanged meaning. |
| `knob_N_clear` | `"1"` | Remove the mapping. |
| `knob_N_adjust` | `"+N"` / `"-N"` | Turn by one detent in that direction. ⚠ The value is an accumulated COUNT, and only its sign is used — honouring the count would change the feel of every existing chain knob. |
| `knob_N_dest_M_set` | `"target:param"` | Point destination M, **keeping its window**. `M` one past the end appends. |
| `knob_N_dest_M_range` | `"lo:hi"` | Its window, as fractions. **Applies immediately.** |
| `knob_N_dest_M_clear` | `"1"` | Remove destination M. Removing the last clears the knob. |
| `knob_N_position` | `"0.0".."1.0"` | Put the knob at a position; every destination follows. |

⚠ `knob_N_set` **collapses** the knob, which is right for assigning an empty one
and wrong for re-pointing the first destination of one that drives several — that
is what `knob_N_dest_1_set` is for.

A window **applies as it is set**, rather than on the next turn of the knob. A
window is adjusted by ear, and one you cannot hear until you turn the knob again
is one you are setting blind. On a multi-destination knob that re-derives the
destination from the current position; on a single one it clamps the parameter
into the new window, which can move it — deliberately, for the same reason.

Readbacks: `knob_N_dests` answers the whole list as JSON in one read,
`knob_N_dest_count` and `knob_N_position` the scalars. `knob_N_target` and
`knob_N_param` keep answering the **first** destination, so anything that knew
about knobs before destinations existed still gets a sensible answer.
`knob_N_name` answers `cutoff +2` for a multi-destination knob and
`knob_N_value` its position as a percentage.

#### A knob write is a base, not a fight with an LFO

`knob_forward_value` routes a modulated parameter through the modulation bus
rather than writing past it — a knob turn is an edit of the **resting** value,
exactly as an edit from the parameter's own page is (see the four forms above).
Without that the next LFO tick recomputes from the stale base and erases the
turn within milliseconds, and the knob reads as dead.

### Reading a modulated parameter — four forms, one rule

While a chain-mod source (slot LFO, etc.) drives `<prefix>:<key>`, the overlay
holds two numbers: the **base** (what the user set — what `set_param` writes)
and the **effective** value (base + contributions — what the overlay keeps
writing into the plugin so the modulation is audible). The read forms
(`chain_mod.c`, dispatched from every `get_param` branch in `chain_host.c`):

```
<key>              BASE while actively modulated; otherwise the plugin's value
<key>:base         BASE (explicit; falls back to the plugin value if unmodulated)
<key>:effective    driven base+mod value (the dot on the arc; same fallback)
<key>:modulated    "1" / "0"
```

**A plain read answers with the base** — read-after-write must return what was
written, or every mod-unaware UI (module web UIs, anything polling plain keys)
shows the LFO's number and the knob reads as dead (#276). A UI that wants the
driven value asks for `:effective` by name; the plugin itself is not the place
to ask, since it holds whatever effective value the overlay last wrote.

### `synth:last_note` — the chain host's own key, and why it is a NOTE

**`synth:last_note`** — the MIDI note last played *into* the synth,
post-MIDI-FX, or `-1`.

**It is a DIAGNOSTIC. Nothing navigates on it**, and `test_voice_follow.sh`
asserts the knob grid never even reads it. It was briefly the third input to
"which voice is focused", for a module declaring neither `child_index_param`
nor `focus_param`, and that was wrong: **a sequencer plays notes**, so a
running pattern changed the editor's page on every hit in the bar. A pad press
and a clip cannot be told apart here either — both reach the synth through
Move's MIDI_OUT echo, tagged the same. The focus is the module's to declare
(see `docs/MODULES.md`, "Declaring your performance surface"); a sequencer
asking what last sounded is the legitimate use of this key.

It is reset to `-1` on instance create and on every synth load, so a
note left over from the previous module can never name a voice in a list that
no longer exists.

**Two paths feed the synth and both record it**, through one inline helper,
`chain_record_synth_note` in `chain_midi.c`. `v2_on_midi` carries the notes a
MIDI FX transformed in its `process_midi`; `v2_tick_midi_fx` carries the notes
it emitted from its `tick()` instead — **which is exactly what an arpeggiator
does**, swallowing the held note and emitting its pattern on the clock.
Instrumenting only the first means `last_note` never updates at all with an arp
in the slot, and the grid follows a pad nobody played. The same split already
bit the MIDI trace, for the same reason.

Note-offs are ignored: a released pad is still the pad you are editing. The
record is an int store on the SPI callback and nothing else — no allocation, no
parsing, no logging.

**It reports a note and not a voice index on purpose.** Resolving the index
needs the canonical voice order — `root`'s nav links, then unlinked voice
levels, then child instances — and `chain_json.c`'s helpers are flat key scans
that cannot walk `levels` in order. A C implementation would therefore be a
*second* copy of that order sitting beside `voices.mjs`. That is the shape that
gave the metronome and `recall_quantize` the same off-by-one, and here it would
fail silently as "the grid follows the wrong pad". One fact, one
implementation: the note → voice lookup happens wherever the voice list already
lives (`src/shared/param_pages/voices.mjs` for the grid, a sequencer's own
parse for itself).

### Chain Architecture

Chain host (`modules/chain/dsp/chain_host.c` — lifecycle/set+get_param/render; helpers split into `chain_{json,params,mod,midi,patch,reorder}.c`, shared decls in `chain_internal.h`) dlopens sub-plugins, forwards MIDI to sound generator, routes audio through FX. Patches in `/data/UserData/schwung/patches/*.json`. Built-in MIDI FX: chord, arp (up/down/up_down/random). Built-in audio FX: freeverb. MIDI sources can provide `ui_chain.js` for fullscreen chain UI.

### Chain shape edits are a PERMUTATION, never a reload

Adding, removing or reordering a position used to be expressed as a run of
`<id>:module` writes, and each of those unloads the position and dlopen()s a
fresh instance — so inserting at the head rebuilt every module behind it and
removing a mid-chain FX rebuilt everything downstream. A running arp lost its
phase; a reverb lost its tail. Three set_param verbs replace that, **1-based to
match the ids**:

```
fx:insert = "1"     midi_fx:insert = "1"    open an empty position, shift the rest along
fx:remove = "3"     midi_fx:remove = "2"    unload that position and close the gap
fx:move   = "1>3"   midi_fx:move   = "3>1"  rotate the span between two positions
```

`chain_reorder.c` shifts every per-position array together (`chain_permute.h`)
and re-aims the three tables that name a position by string — modulation targets,
the two LFOs, the knob mappings. Instances keep running, so **nothing is
carried**: state, modulation base and routing are still the originals.

**Two kinds of per-position array, and the difference is a crash.** A VALUE
array is vacated by zeroing its bytes (`PERM_FIELD`). An OWNED-BUFFER array
holds a pointer to a block allocated once per position by
`chain_alloc_position_storage` and **never null** — `fx_params`,
`midi_fx_params`, and the two `ui_hierarchy` caches (`PERM_OWNED`). Those are
**rotated**: the vacated position gets the buffer displaced off the end of the
shift and its *contents* are cleared. Zeroing the pointer instead left a NULL
that `v2_load_midi_fx_slot` parsed a param table through — SIGSEGV on the SPI
callback, loading a MIDI FX in front of an existing one — and leaked the
allocation the shift overwrote.

`tests/host/test_chain_permute.sh` pins both: a new
`[MAX_AUDIO_FX]`/`[MAX_MIDI_FX]` member not in a collector fails, and the
owned/value split is derived from `chain_alloc_position_storage` rather than
trusted. `tests/host/test_chain_midi_fx_slot.sh` drives the crashing sequence
against a real `chain_instance_t` with the real loader.

Insert only opens the hole — the caller follows with the ordinary
`<id>:module` write. Both chain walks skip a hole per position, so the frame in
between renders correctly.

**Thread safety is free**: parameter requests are serviced from
`shim_pre_transfer` on the SPI audio thread, after `shadow_mix_audio`, and
nothing else touches a chain instance — a permutation cannot interleave with a
render. (That same property is what lets module loading `dlopen()` from this
thread, which *is* a pre-existing realtime violation.)

In the shadow UI, `writeChainShape` emits these verbs. It replaced
`writeChainOrder`, whose state / modulation-base / LFO-remap carries are all
deleted. `clearLfoRoutingForComponent` stays: a picker **swap** genuinely does
destroy and create a module.

The two `+` boxes add **where they are drawn** — the MIDI one at the head of the
chain (index 0), the audio one appended. Backing out of a `+` picker, or picking
`None` in one, writes nothing at all.
