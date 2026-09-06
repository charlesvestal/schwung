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

**`:effective` ON AN UNMODULATED KEY REACHES THE PLUGIN.** The chain owns the
suffix and answers from its own table while a source is routed. When none is,
it used to strip `:effective` and ask the plugin for the PLAIN key — which is
right only if the chain is the sole thing that can move a parameter, and it is
not. A synth driving its own value (MonkSynth sweeps its vowel from pad
pressure) serves `<key>:effective` itself, and that answer was being thrown
away: the suffix consumed, the plain key asked, the knob's value returned. Every
picture of that parameter sat still while the sound moved.

It now asks the plugin **by name first** and falls back to the plain key only on
a miss, so a module that has never heard of the suffix is unaffected. An empty
answer counts as a miss — an unserved key comes back as an empty buffer, and
taking that for a value would blank the reading. See `"live": true` in
`docs/MODULES.md` for the UI half: it is what makes the host ask often enough
for the answer to be worth serving.

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

`tick()` also continues while an otherwise silent chain slot is parked by the
audio idle gate: the shim's `mod:tick` runs it alongside the LFOs, so a
time-driven MIDI FX keeps generating with no audio render behind it. If it
**delivers a message to the synth**, that same block wakes and renders, so the
generated note is heard now rather than on the next idle probe up to ~0.5 s
later. The tick is marked as already advanced, so the wake-up render does not
advance MIDI FX or LFO time a second time.

Delivery, not emission, is the trigger — and the distinction is not academic.
A slot carrying MIDI FX with **no synth loaded** (Pre mode, driving Move's own
track instrument) is silent by construction and therefore permanently idle, so
waking it on "a MIDI FX emitted" would un-park it on every generating frame to
render a synth that is not there: the idle gate switched off for exactly the
slot that can never need it.

The handshake is three calls and it is order-dependent — `mod:tick`, then one
`chain_take_midi_tick_wake`, then the render. `take` is one-shot and clears the
double-tick guard when the answer is no, because in that case no render is
coming to consume it; ask twice and the second answer erases the first. The
transitions are pure and live in `chain_idle_tick.h` so `tests/host` can drive
them (`test_chain_idle_tick.c`); the wiring is pinned by
`test_idle_midi_tick_wake.sh`.

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

### Buses: a module declares which voices it can render APART

A splittable module publishes **`split_voices`** from `get_param` and exports
**`move_plugin_render_split`**. Neither is required; a module that does neither
is rendered exactly as before, and the shadow UI offers no bus affordance at all
(see `docs/SHADOW_UI.md`, "Slot buses").

```json
[{"id":"kick","label":"Kick"},{"id":"snare","label":"Snare"}]
```

```c
/* Exported alongside move_plugin_init_v2. NOT a field on plugin_api_v2_t. */
void move_plugin_render_split(void *instance, int16_t *const *voice_out,
                              int n_voices, int16_t *main_out, int frames);
```

**`split_voices` is FLAT AND ORDERED, and entry *i* is buffer *i*.** The
bus→voice map has to be resolved in C on the SPI callback, and `chain_json.c`'s
helpers are flat key scans that **cannot walk `ui_hierarchy`'s `levels` in
order** — the same constraint that makes `synth:last_note` report a note rather
than a voice index. So the module publishes a flat array and the host never
tries to walk its hierarchy for this. `split_voices_parse.h` does the scan; it
has no escape handling and no brace tracking, so an id containing `\"`
truncates and a *nested* `"id"` key is harvested as a phantom voice.

**A rejected entry is a HOLE at its own index, never a compaction.** An id that
is empty, or too long for `SPLIT_VOICE_ID_LEN`, is stored as an empty string and
still counted. Dropping it and shifting everything up would be worse than the
truncation the rejection exists to prevent: a truncated id orphans one bus,
visibly, while a shifted index silently re-points **every voice behind it** to
the wrong render buffer, with nothing on screen to say so. An empty stored id
never matches a lookup (`bus_voice_index`), so a hole resolves to Main like any
unassigned voice.

**`render_split` is a dlsym'd symbol, never a field on `plugin_api_v2_t`.**
Appending to that struct is what boot-looped a device via breakbeat's header
drift (see `CLAUDE.md`, "`host_api_v1_t` ends in a run of NULLs"): a module
cannot extend the ABI from its side, and a guarded read of a field we do not
have tests memory belonging to somebody else. A dlsym'd symbol is
absent-or-present with no offset to get wrong. `v2_load_synth` resolves it on
the synth handle and clears it on unload — the pointer is resolved against a
handle that is about to be `dlclose`d.

**`main_out` is the audio that belongs to NO voice.** A drum bus, a mix
compressor, a global filter, an internal reverb return — any master section has
audio that is not attributable to one voice, and in split mode there is no
single `out` for it to land in. It is the *same* pointer an unassigned voice is
handed, so it is usually reachable through `voice_out[]` as well; it is passed
explicitly because **it is not reachable that way when every voice is on a
bus** — assign all 32 pads of a rack and no entry points at main. A module with
no master section ignores the argument. `frames` stays last, as in
`render_block(inst, out, frames)`.

**It ACCUMULATES, and `voice_out[]` entries ALIAS.** `v2_render_block` clears
the main buffer and the *distinct* bus buffers named by `bus_mix_active_mask`,
then hands `voice_out[i]` to the module for voice *i*. Two voices assigned to
one bus get **the same pointer**, so their sum happens inside the module's own
render with no mixing pass of ours; a voice on no bus gets the main buffer, so
the sparse case costs nothing at all. Four consequences a module author owns:

- **Accumulate, never overwrite** — the opposite of `render_block`. Overwriting
  makes two voices on one bus into "whichever wrote last".
- **Never `memset` a destination.** The chain has already cleared main and the
  bus buffers before the call, and the buffers alias, so zeroing one is zeroing
  another voice's audio for that frame. This is the exact carry-over mistake a
  module ported from a single-`out` render is set up to make: its old entry
  point almost certainly cleared its own output first.
- **Never write more than `frames` frames** into any `voice_out[]` entry. These
  point at shared bus buffers, so an overrun is another bus's audio, not your
  own tail. Nothing checks this.
- **Both entry points must be state-compatible.** The host switches between
  `render_split` and `render_block` **at runtime, per frame**, on whether any
  voice is currently assigned to a bus — assigning one voice on the shadow UI
  flips the module's active entry point mid-stream with no reload. Same voice
  allocator, same envelope / LFO / phase state, or the flip is audible.

`bus_mix.h` holds that arithmetic, header-only so `tests/host` can run it
natively: `v2_render_block` lives in a translation unit that cannot be built on
the dev machine, which is how arithmetic like this ships untested.

**Two threads own a bus, and the split is the design** (`chain_bus.c`). The RT
side (the SPI callback) owns the **request** — which module, which voices, the
send levels — and never allocates, opens a file or `dlopen`s. The **worker**
(`chain_bus_worker_fn`, SCHED_OTHER on cores 0–2) owns the **realisation** —
the bus buffer, the `dlopen`, the `create_instance`, the ~1.1 MB param table.
`get_param` answers from the *request* side, so the UI never reads a field the
worker is writing and "what is loaded" is positional and immediate rather than
lagging a load. Two gates join them: `buf` (release/acquire) and a **sequence
number**, not a flag — the boolean it replaced could be resurrected by a
preempted worker, putting `process_block` on the audio thread in a race with the
next reconcile's `destroy_instance` and `dlclose`.

**That worker is why the threading contract is now true-with-one-exception**;
see `src/host/plugin_api_v1.h` and rule 4 of `docs/REALTIME_SAFETY.md`, and keep
all three in step.

### Per-voice sends: a superset over the bus send, not a replacement

The aliasing above is what makes a bus free, and it is also what makes a bus
**coarse**. Two voices in one bus are handed one pointer and are already summed
by the time the chain sees the buffer, so they cannot be scaled differently: a
slot's per-bus send levels are `SLOT_BUSES × BUS_MIX_SENDS` = **16**, with every
voice belonging to exactly one bus. The reference consumer, `schwung-dr32`, carries
**64** — a `send_db[0]`/`send_db[1]` on each of 32 pads. It could not drop its
internal sends and adopt the platform feature without losing capability, which
is the test of whether the feature is sufficient.

So a voice may carry its own two levels, and **nothing existing changes
meaning**:

| | taken from | when |
|---|---|---|
| **per-BUS send** (unchanged) | the bus buffer, **post-insert**, post-fader | after the bus's chain runs |
| **per-VOICE send** (new) | the voice's **own** audio, **pre-insert**, post-fader | straight out of `render_split` |

### The slot send: the drain point after the slot FX

A bus send and a voice send both need the module to publish `split_voices`, and
one module in the fleet does. **The slot send needs nothing of the module**: the
whole slot feeds Send A and Send B, which is what makes the global send buses
reachable on an ordinary synth at all — and is the classic reason a console has
them, one reverb shared by four slots instead of four instances inside them.

It could not exist while `chain_drain_sends` was the only tap. That runs
immediately after `render_block`, and under the same-frame-FX mode the device
always runs, `render_block` returns the **raw synth** (the `external_fx_mode`
early return) while the slot's own 8 FX run later, in the shim. There is no
post-FX buffer at that point, so a slot send taken there would have been
pre-FX while every bus send was post-insert — two meanings behind one control.
It shipped once as exactly that and was inert; the level was written, saved,
restored and read by nobody.

`chain_drain_main_send` is the second tap, and the shim calls it from the **mix
pass** instead, at the three sites where a slot's finished audio exists:
`shadow_slot_fx_deferred[s]` on the normal path, the inline legacy branch, and
`fx_buf` under `rebuild_from_la` (where the slot is Move's track plus the synth
through the same chain — inseparable by construction, the same reason a stem is
a slot). The audio is passed IN, because the chain does not hold it.

The timing works because `send_accum[]` is **cleared in the render pass**, which
runs post-ioctl, *after* the mix pass: clear → per-bus and per-voice drains →
ioctl → the next mix pass's slot drains → the send-bus loop consumes. Both taps
describe the same block and neither is consumed twice.

Post-fader like every other send here — the shim passes
`shadow_effective_volume(s) * fade.gain`, which is 0 for a muted or soloed-out
slot. The level is `buses:main_send<N>`, the key `chain_bus.c` already read and
persisted; it is edited in **Slot Settings** (a `Sends` page on the knob grid,
two rows on both settings lists), never on the bus Send Mixer, because a slot
with no buses never sees that screen.

The three **sum** into the same `send_accum[]`. A voice with all-zero per-voice
sends **costs nothing and still aliases** into its bus buffer exactly as before
— you pay only for what you use.

**The partition rule is: a voice is solo-buffered iff any of its per-voice send
levels is above zero.** It lives in `bus_mix.h` (`bus_mix_solo_mask`,
`bus_mix_build_table_split`) beside the routing it modifies, because that header
is what `tests/host` can compile and run — `test_bus_mix.c` asserts the sparse
case is **pointer-for-pointer identical** to the pre-sends build and that not
one pool slot is touched, which is the property that keeps buses cheap. A
solo-buffered voice renders into `chain_instance_t::voice_send_buf[i]`, its
sends are taken from that in `chain_drain_sends`, and its audio is then
accumulated into the destination it would have had — so it still reaches its
bus and still goes through that bus's inserts. `bus_mix_active_mask` is
deliberately unchanged by the partition for exactly that reason.

**The pool is 16 KB inline on the instance and does NOT go through the bus
worker.** Everything else in this feature does, because it has to be
*allocated*; this does not — the array is part of the instance, live for exactly
as long as the instance is, so there is nothing to publish, no release/acquire
pair and no lifetime question. It is not on the callback's stack either.

**The tap is chain-side. The module is never told sends exist** — it is handed
a pointer and accumulates into it, the same contract as before.

Two things the drain does that are not obvious from the arithmetic. It runs in
`chain_drain_sends` rather than in `v2_render_block` because **`accum` does not
exist there**: the shim owns the global send buses and passes them in after the
render, together with the slot's volume, so that is the only point at which a
per-voice send can be both taken from the voice's own audio and scaled by the
fader. And `voice_send_mask` is cleared on **every** path out of
`v2_render_block`, bypass included — "has a level" is not "was rendered this
frame", and draining on the level alone would send a voice's final 128 frames
forever.

### THE MODULE OWNS A VOICE'S SEND LEVEL

Three tiers of send exist and each is owned where the thing it sends lives:

| Tier | Owner | Where the control lives |
|---|---|---|
| **Voice** | the **MODULE** | its own pages |
| **Bus** | the host | the bus's row on the Send Mixer |
| **Slot** | the host | Slot Settings |

The host briefly owned the first tier too — an id-keyed config, a fader per
voice on the Send Mixer, a `buses:voice<V>:send<M>` route and a `voice_sends`
array in the slot document. dr32 already published per-pad `send1`/`send2`
knobs on its own `pads` level, beside pan and cutoff, so **the same concept
appeared twice, in two places, meaning different things.** All four of the
host's halves are deleted. The audio machinery — the solo partition, the
fold-back, `chain_drain_sends` — is untouched; only the source of the numbers
moved.

**A module declares a key TEMPLATE beside its voices:**

```json
"split_voices":      [{"id":"pad1","label":"Kick"}, …],
"voice_send_params": ["{id}_send1", "{id}_send2"]
```

Both are served through `get_param`. `{id}` is substituted **verbatim** with
the voice id, so the host reads `pad1_send1`, `pad1_send2`, … straight off the
module's own parameter surface.

- **Array position is the send index.** `[0]` is Send A, `[1]` is Send B. A
  shorter array declares fewer sends — a real answer. Longer than
  `BUS_MIX_SENDS` is an **error and the whole declaration is refused**, never a
  silent truncation: a module that believes it declared three sends while the
  host kept two has a control writing into nothing.
- **An entry with no `{id}` is refused too**, for the same reason — it would be
  one key for every voice, i.e. every pad writing the same level, which looks
  like a working feature.
- **A module declaring nothing gets no per-voice sends.** That is the correct
  fallback and not a gap: put the voice in a bus and ride the bus's send.
- **The host never adjusts the id it was given.** If a module's `split_voices`
  ids do not address its own params when substituted, that is the module's
  contract to fix — a host that corrected an off-by-one would be unpredictable
  for every other module.

**THE RANGE IS THE MODULE'S, AND IS NEVER ASSUMED.** A send level is 0..127
(`BUS_MIX_SEND_LEVEL_MAX`, 127 exactly unity); a module's own parameter is
whatever it says it is — dr32's is dB over −70..+6. The host looks the
parameter's declared range up in the module's own `chain_params` (the same
metadata the knob grid draws the control from) and maps:

| `unit` | law |
|---|---|
| `dB` | `127 × 10^(v/20)`, clamped — **0 dB is exactly unity** |
| anything else | `127 × (v − min) / (max − min)` |

In both, a value **at or below the declared minimum is exactly 0**, so the
control's own off position is off whatever floor the module chose. **When the
metadata cannot be found the host REFUSES that send** rather than picking a
scale — assuming 0..127 mis-scales a 0..1 module by two orders of magnitude,
assuming 0..1 mutes a 0..127 one, and both are silent. The lookup tries the
focus-addressed spelling first (`{id}_send1` → `send1`, which is how a
per-voice param is actually authored in a `ui_hierarchy`, once on the child
level, because 32 voices × N params would not fit `chain_params`) and then the
fully substituted key.

All of that is in **`src/host/voice_send_source.h`** — header-only for the same
reason `bus_mix.h` is, so `tests/host/test_voice_send_source.sh` compiles and
RUNS it; `chain_bus.c` is a translation unit the dev machine cannot build.

**`voice_send[]` is a CACHE, not configuration.** Nothing about it is saved: the
levels ride in the synth's own opaque `state` blob, which the slot document
already carries, so a reload restores them by the same route every other one of
the module's parameters takes. It is refreshed two ways, and the split is
deliberate:

- `chain_voice_sends_poll` runs on the render path, **before** the solo mask
  (which is computed from the cache), and asks the module for
  `VOICE_SEND_POLL_PER_FRAME` = 4 keys a frame — a 32-pad rack sweeps in 16
  frames (~0.36 s). A module's `get_param` is on the SPI callback, so the full
  64 calls cannot be a per-frame cost. This is the **ground truth**, catching
  what no write of ours went past: a state restore, a preset load, a value the
  module moved itself. Those are followed by silence.
- `chain_voice_sends_touch` sees a `synth:` write whose key ends like one of the
  declared templates and arms a **full sweep next frame**, so a knob turn is
  heard immediately. It never sets a level from the written value — the module
  may clamp, and the key may be a focus alias (`pad_send1`) naming a pad the
  host cannot resolve. It says "ask again now", not "the answer is".

**A key the module does not serve leaves the level ALONE.** An unserved read is
not an answer of zero, and letting one become a level is how a send silently
mutes. `chain_voice_sends_load` is the one place the cache is cleared, on the
one event that changes the voice list — a synth load or unload — because a level
cached against the previous module's render index would be a send on whatever
voice now holds that index.

Worst case, all 32 voices carrying a distinct level, the added per-frame work is
a 16 KB clear, a 16 KB fold-back and 32 × 2 scaled adds. The sparse case is a
32-bit mask scan.

### The bus↔send seam, and whose design it is

A bus's audio is summed into the slot's main output **before** the slot's own
eight audio FX, so a slot compressor sees the whole kit; the two **global send**
levels are taken from the post-insert bus buffer at the same point, and drained
by the shim, where the per-voice levels above are added to the same
accumulators. The sends themselves — the topology, the post-fader rule, the
return levels, the feedback-safe A→B and the generic FX-bus picker — are
documented in `docs/SHADOW_UI.md`.

**Design credit: PR #121 by legsmechanical**, which designed and
device-verified the global-send half of this feature: two post-fader send buses
hosted as `master_fx_slot_t`, a `send_accum[]` in the shim, return levels, the
A→B ordering that makes feedback impossible by construction, shared presets, and
one picker over all three FX buses. That branch is unmergeable — its merge-base
is 2026-03-04, `main` is 1696 commits ahead and the branch carries 864 of its
own — so this is a re-implementation of its design on current `main`, not an
independent invention of it. `send_fx_key.h` carries the same credit at the
point the keys are parsed.
