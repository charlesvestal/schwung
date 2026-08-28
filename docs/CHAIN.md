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
