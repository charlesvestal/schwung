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

### Macro Knobs

Each of a slot's 8 chain knobs (`knob_mapping_t`, CC 71–78) is normally a 1:1
mapping (`knob_N_set` = `"target:param"`). It can instead be promoted to a
**Macro** (`knob_N_to_macro`): the knob's own position (0..1, not any single
param's value) fans out to up to `MAX_MACRO_TARGETS` (4) module params, each
with an independent signed **depth** (-1.0..+1.0, same semantics as an LFO's
depth) — turning the knob moves every configured target, possibly in opposite
directions and by different amounts. A knob is either direct or a macro, never
both; promoting/demoting clears the other mode's fields.

Configuration set_param keys, all auto-vivifying the knob's mapping as a macro:

```
macro_N_set_name       = "WARM"           macro's display name (wire-level only — see below)
macro_N_row_R:target   = "fx1"            R = 0..3, clears this row's old contribution first
macro_N_row_R:target_param = "cutoff"
macro_N_row_R:depth    = "0.6"            -1.0..+1.0
macro_N_row_R:clear    = "1"
```

Read: `knob_N_is_macro`, `macro_N_config` (one JSON blob: name, value, all 4
rows — the Shadow UI's single read on entering the Macro Editor).

**Macros are unnamed in the Shadow UI.** `macro_N_set_name` and the `name`
field in `macro_N_config`/persistence still exist at the wire/host level (a
macro row's `cc`/`is_macro` group in `slot_N.json` still carries a `name`
key — see the persistence shape below), but nothing in `shadow_ui.js` calls
`macro_N_set_name` or displays a per-macro name; every macro knob in the
Knobs list reads simply as `Macro` (`getKnobAssignmentLabel`). Inside the
Macro Editor, each of up to 4 rows shows one target param per row — its
`short_name` (falling back to `label`, then the raw key — the same fallback
the knob grid itself uses via `buildMetaIndex`) on the label side, its depth
as a percentage on the value side. Click toggles jog-editing that row's
depth in place; Shift+Click opens the target picker for that row instead.

**Changing a knob's type is Shift+Click on its row in the Knobs list**
(`enterKnobEditor`'s select handler), not a click on the row itself (which
still opens the Macro Editor for a macro, or the target picker for a direct
knob). Shift+Click opens **Knob Options** — a one-entry menu (room for more
later) whose only entry, **Knob Type**, is a Simple/Macro picker. Selecting
the type the knob is already in is a no-op; selecting the other one requires
a Yes/No confirmation, since either direction discards state a plain undo
can't recover — a macro's configured rows, or a direct assignment's target.
Confirming applies the same host verbs the promotion/demotion always used —
`knob_N_to_macro` to promote (landing in the Macro Editor, same courtesy the
old picker-driven promotion gave), `knob_N_set` with an **empty** value to
demote (landing back on the Knobs list, "direct, unassigned"). The knob
target picker's old `[Make Macro]` entry is gone — Knob Options is the one
place a type change happens, so it is the one place that gets the
confirmation.

**Implementation is almost entirely reuse of the runtime modulation bus**
(`chain_mod.c`), which already does non-destructive, range-scaled, N-**sources**-
per-target contribution math for LFOs. A macro is the mirror case — 1 source,
N **targets** — built by calling `chain_mod_emit_value` once per configured row
with a shared `source_id` (keyed by the knob's CC number, not its array index,
so `knob_N_clear`'s array-compaction can't hand a stale source to a shifted-in
mapping) and `signal = 2*pos-1, bipolar=0`, which recovers a clean unipolar
`contribution = pos * depth * range_span` per target. `chain_macro_apply()` in
`chain_mod.c` is the single place that does this; it is called from the CC
71-78/102-109 dispatch in `chain_midi.c`, from `knob_N_adjust` (Shift+Knob) and
the `macro_N_row_R:*` writes in `chain_host.c`, and once per macro after patch
load in `chain_patch.c` (macros are event-driven, not recomputed every render
block like LFOs, so a reload must re-push the saved position's effect).

**Persistence is a flat, repeated-row shape, not a nested array.** The
`knob_mappings` JSON (inside `slot_N.json`'s `chain` object) is parsed by a
hand-rolled scanner in `chain_patch.c` that finds the first `}`/`]`  with no
bracket-depth tracking — a real `"targets":[...]` array would silently
truncate after its own first `]`. A macro instead serializes **one row per
populated target**, all sharing `cc`/`is_macro`/`name`/`value`, tagged
`macro_row`:

```json
{"cc":75,"is_macro":1,"name":"WARM","value":0.42,"macro_row":0,"target":"synth","param":"cutoff","depth":0.60}
{"cc":75,"is_macro":1,"name":"WARM","value":0.42,"macro_row":2,"target":"fx1","param":"wet","depth":-0.30}
```

The parser merges rows sharing a `cc` **and** `is_macro:1` into one
`patch->knob_mappings[]` entry rather than one JSON row per array slot — so
`knob_mapping_count` keeps meaning "how many of the 8 physical knobs have any
mapping," unaffected by how many targets a macro among them happens to have. A
macro with zero configured rows still emits one header-only row so
`is_macro`/`name` survive a reload.

A chain-shape edit (`fx:insert`/`remove`/`move`) retargets a macro's rows the
same way it already retargets `mod_targets[]`/LFOs/direct knob mappings —
`chain_perm_retarget_all` in `chain_reorder.c` has a fourth loop for
`knob_mapping_t.macro_targets[]`, nested inside the existing knob-mappings
loop. Skipping it would desync a macro row's stored target from the position
`mod_targets[]` (the *live* contribution) already renamed — the two must move
together, or the next knob turn re-applies to the wrong param.

The Shadow UI's Macro Editor (`enterMacroEditor`, `VIEWS.MACRO_EDITOR`) reuses
the LFO target picker's `LFO_TARGET_COMPONENT`/`GROUP`/`PARAM` views verbatim
via `makeSlotMacroTargetCtx` — a context object shaped like `makeSlotLfoCtx`
(both share the extracted `collectChainTargetComponents` helper), since the
`macro_N_row_R:target`/`:target_param` sub-keys mirror `lfoN:target`/
`:target_param` exactly. Master FX has no macro knobs — macros are a
slot-scoped concept (the receive-channel physical knobs), and Master FX
addresses its LFOs at IPC slot 0 by an unrelated convention.

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
and re-aims the four tables that name a position by string — modulation targets,
the two LFOs, the knob mappings, and (nested inside the knob mappings) each
macro knob's rows. Instances keep running, so **nothing is
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
