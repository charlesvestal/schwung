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

### Automation lanes — a clip's knob moves, recorded and played back

A **lane** records a parameter against the clip playing on a Move track and
plays it back every loop. One lane is `(track, clip slot, target, param)` — the
same `(target, param)` address the knob grid, the mod bus and the E16 surface
already use — plus a *fingerprint* of the clip it was recorded against. Its
content is an ordered list of breakpoints, each `(phase in beats from the
clip's loop start, value in the parameter's own units)`.

The pure model is `src/host/lane_store.{h,c}` (runnable from `tests/host/`); the
`lanes:state` document is `src/host/lane_serial.{h,c}`; the glue that knows
about `chain_instance_t`, parameter types and the mod bus is
`src/modules/chain/dsp/chain_lanes.c`. `lane_tick` runs once per block from
`render_block` **and** from the `mod:tick` branch on a silent slot — the shim
skips `render_block` on a silent slot for 171 frames in 172, and a lane must
keep playing through silence.

#### It is ABSOLUTE, and that is why `chain_mod` grew an override class

The lane *is* the value; the knob is the base underneath it. So
`chain_mod_recompute_effective` computes

```
effective = clamp((override ? override : base) + Σ offset contributions)
```

One source per target may be flagged `is_override`. **LFOs still sum on top** of
whatever the lane plays, exactly as they sum on top of the knob, and clearing a
lane goes through the ordinary clear path (`chain_mod_emit_override` with
`enabled = 0`), so the parameter **returns to the knob** with a forced write
rather than sticking wherever the lane stopped. Nothing about write throttling,
the change epsilon or base tracking on `set_param` is re-implemented: a lane is
just another source.

**An unarmed knob turn must not be inaudible.** Under an absolute lane a turn
moves the base, which the override masks, and the encoder reads as broken. So an
unarmed turn **punches through until the loop comes round** — per
`(target, param)`, expiring when the phase wraps past where it started, so it
survives a tempo change and needs no timer. An *armed* turn cancels any open
punch on that lane, or the point just recorded would sit silent for a loop.

#### VERIFIED END TO END ON HARDWARE (2026-09-13)

Driven through the param channel with `schwung-testd`, against
`bouba-kiki` in slot 0 — which is how recording and p-locks were verified
without a hand on the device: a lane records from a **param write**, and an
injected knob CC cannot produce one (the drain writes Move's mailbox, not
Schwung's param channel).

**A p-lock, with the transport STOPPED:**

```
SET_PARAM lanes:plock synth pinch 9.0 0.9   ->  lanes:plocked = 1
GET_PARAM lanes:state                       ->  V 2
                                                L synth pinch 0 2 8 12 3 50 1
                                                P 9 0.899999976 1
```

Keyed to (track 0, slot 2) = T1 s3, the fingerprint matching the file's clip
(loop 8..20, 3 notes, first note 50), one point, `hold = 1`.

**And it reaches the synth, not just the mod bus.** `:effective` and
`:modulated` prove only that an override is registered — this project has been
burned by exactly that (`:effective` is the bus's own table). The independent
witness is the plugin's own state blob:

```
synth:pinch            0.47   (the user's knob, untouched)
synth:pinch:effective  0.9
synth:pinch:modulated  1      <- the on-screen lane-driven mark reads this
synth:state            {"pinch":0.9, ...}   <- the synth itself
```

`lanes:clear` then returned all four to 0.47 / 0 — the release reaches the
plugin too.

**Recording a pass**, armed by injecting Move's own Record button (`lanes:armed`
went 0 → 1, LED `SOLID`), five writes ~0.7 s apart:

```
P 9.5416666666666679 0.100000001
P 11.583333333333332 0.25
P 13.625 0.400000006
P 15.625 0.550000012
P 17.583333333333336 0.699999988
```

**Those phases are CLIP time** — 9.54, not 1.54 — which is the clip-time change
proving itself on the device rather than in a fixture. Playback interpolated
along the curve (0.13 → 0.37 → 0.61 → 0.70, holding past the last point).

Two things this also settled, both previously listed as unverified: the
`:modulated` mark exists and reads 1, and the clip gained **no notes** from
arming Record (`[50, 50, 60]`, loop 8..20 unchanged).

#### Step p-locks: the arithmetic is done, the gesture is not

A p-lock is **hold a step, turn a knob** — set a value *on* that step. Two
pieces of it exist and are tested; the input plumbing is not written.

- **A point can be a RECTANGLE.** `lane_point_t.hold` says "this value stands
  until the next point" instead of ramping into it, because that is what a
  p-lock is: under plain interpolation two neighbouring p-locks glide into each
  other and sound like automation rather than a sequencer. It is **free** —
  `{double, float}` is 12 bytes padded to 16 — and it is in the format now
  precisely so nothing has to be migrated later. `stepped` (the parameter's
  type) and `hold` (the point's own shape) are independent, and **the LEFT
  point of a segment decides it**. A write within `LANE_MIN_POINT_BEATS`
  replaces the shape along with the value, so recording a sweep over an old
  p-lock produces a slope and p-locking over a recorded point produces a
  rectangle. Serialized as an optional third field on the `P` line, absent
  meaning 0, so an ordinary sweep's document is byte-identical to before.
- **A held step's phase is `step_plock.h`**, the inverse of the mapping the
  device scored 26/26:
  `phase = ((bar - 1) * steps_per_bar + index) * step_resolution`.
- **The displayed bar comes from the STRIP, not from Move's announcement.**
  `bold_segment` is the thickened bar, read off the screen; `shadow_editor_bar`
  needs the screen reader running — measured with it off, it stayed 0 through
  repeated arrow presses, so an oracle built on it is absent exactly when
  nobody has turned that on. This is what settles what the design doc called
  "the least certain part of Project 1".
- **A MULTI-PAGE BAR IS REFUSED, not guessed.** A bar fits the 16 step buttons
  only while `steps_per_bar <= 16`: true for 4/4 at 1/16 (Move: "the entire bar
  can be accessed at once"), false for 1/32, and false for **11/8 at 1/16**,
  which pages 16 + 6 at the default grid. Move shows the page number on the
  display and we do not read it, so the answer is `MULTI_PAGE` — a p-lock one
  page out is a value on the wrong sixteenth, silently. Bar 0 is refused too:
  a one-bar loop draws no thickening, so `bold_segment` can be 0, and that must
  not quietly mean bar 1.
- **What is missing is the gesture.** A held step must be **swallowed from
  Move** while the shadow UI is up (both edges, latched — `midi_in_swallow`),
  or the same press edits the clip's notes; and the knob turn has to arrive
  through Schwung's own param write, which an injected CC cannot do. So it is
  unverifiable by the harness and waits for a hand.

#### Recording on a clip Move has not saved yet

The hole: make a clip, press Play, try to record automation — refused. `T1 -`,
`loop_len 0.00`, `has_phase false`. The clip reaches `Song.abl` about **10 s**
later (measured), and until then there is no length, so no phase, so nothing
records.

It closes with the two facts arriving from two places at two times:

1. **The length, now, from Move's own screen.** `shadow_slot_clip_phase` falls
   back to `step_strip_segments_for_track()` when the file has no entry for the
   live clip: `loop_len = segments × quarters_per_bar`, `loop_start` **assumed
   0**. Both limitations are real — bar resolution, and an origin the strip
   cannot show — and honest for a clip just made, whose loop is a whole number
   of bars starting at bar 1.
2. **The identity and the true origin, later, from the file.** When the clip
   appears, its notes identify it and its `loop.start` places it. A lane
   recorded blind is **adopted**: every point is shifted by the real
   `loop_start` and the fingerprint is stamped, in one step, with the number
   that just arrived rather than a guess (`lane_adopt_fingerprint`).

- **`fp_valid == 0 with a valid phase` IS the provisional signal**, and it
  needs no new argument on a seam that cannot safely take one (`dlsym`, the
  breakbeat drift). It is a state that could not otherwise occur: the
  fingerprint is filled in *before* the anchor is even checked.
- **Adoption is scoped to THIS SESSION's blind takes** (`origin_pending`, never
  serialized). An absent fingerprint on disk and a blind take are the same
  bytes and must not be the same decision — adopting the loaded one would bind
  a lane to whatever clip later occupied its position and play it. The cost is
  a reboot inside the 10 s window: that take stays at its assumed origin, goes
  stale, and is silent until re-recorded.
- **It refuses** an already-identified lane (structurally — the fingerprint is
  no longer absent, so it is idempotent and cannot be hijacked), an absent
  incoming fingerprint (a no-op that would still clear the pending state), and
  a non-finite or negative `loop_start`. Every refusal leaves the lane exactly
  as it was, still adoptable: a bad answer now must not cost the chance of a
  good one later.
- **A live pass's `rec_last_phase` moves with its points**, or the next write
  erases a span the gesture never swept.
- **A blind take PLAYS while it is unidentified.** That is not a hole in the
  staleness rule: the lane is at the position that is playing and nothing else
  can be there. Staleness is for a position holding a *different* clip, and
  establishing that needs a fingerprint.
- **No strip reading, no answer**, and no anchor, no answer. The fallback is a
  reading, not a guess.
- **THE PROVISIONAL LENGTH IS ±1 BAR, and that is bounded by WHEN it matters.**
  The strip's count can exceed the loop by one (Move draws the next bar it
  offers you), so a blind take's length can be a bar out. A length is only used
  at the **wrap**: inside a single pass the phase is monotonic and correct
  whatever the length is, and a clip younger than ~10 s at, say, 120 BPM has
  usually not completed one pass. So a blind take recorded in the first pass is
  right; a longer one can wrap early or late, and the honest remedy is to
  record it again once the clip is in the file. Adoption fixes the ORIGIN, not
  a length the points were already computed against.

#### A point is CLIP TIME, and the loop is a WINDOW over it

Measured in Move's own file (2026-09-12), a clip whose `region`/`loop` is
`8.0 .. 20.0`:

```
"region": { "start": 8.0, "end": 20.0, "loop": { "start": 8.0, "end": 20.0 } }
notes:    startTime 0.0, 9.5, 16.5      <- absolute from the CLIP's start
```

The note at 0.0 sits **outside** the loop and does not play. So notes are
absolute clip time and the loop is a window over them — and a lane stored in
that same coordinate makes *"the automation lines up with the notes"*
definitional rather than something the host maintains.

**Loop-relative storage was the first design, and it was wrong in two ways.** A
sweep recorded one beat into that loop was stored as `1.0` instead of `9.0`, so
opening the loop out to the whole clip replayed it at beat 1 — two bars early,
on different notes. And a step p-lock has the same problem in reverse: *"bar 3,
step 5"* cannot be turned into a loop-relative phase at all without knowing
where the loop begins. The counter-argument — that `loop_start` is unobservable
for a clip Move has not saved yet — holds only for the save latency, which is
**10 s measured, not the ~35 s long assumed**.

- **The unit is the QUARTER NOTE, and that is what makes it signature-proof.**
  Changing the set to **11/8** changed not one number in `Song.abl`: the same
  clip stayed `8..20`. An 11/8 bar is 5.5 quarters, so that 12-quarter loop is
  ~2.18 bars. The signature is therefore needed **only to convert bars**, which
  is the strip reader's problem alone (`quarters per bar = upper * 4 / lower`).
  It lives in the file **per clip** *and* song-wide — and for a brand-new clip
  the song-level one is available even though the clip is not.
- **Points outside the window are dormant, never deleted** — the same rule as a
  shrunk clip, generalised from a prefix to a window. A point *below*
  `loop_start` is dormant too, which a prefix test `[0, loop_len)` got wrong.
- **A recording pass wraps at the WINDOW.** The swept span is
  `(prev, loop_start + loop_len)` then `[loop_start, phase)`. Erasing from 0
  instead would delete automation on the bars *before* the loop — material the
  gesture never touched, invisible from inside the loop, and audible the moment
  the loop is opened out.
- **`lane_pass_travel` checks window membership BEFORE direction.** A `prev`
  outside the current loop cannot have been swept from inside it, and that is
  just as true walking forward: prev 2.0 to phase 8.2 on a loop of 8..20 reads
  as a tidy 6.2 beats, with only the gap threshold downstream stopping it from
  erasing two bars.
- **The seam did NOT grow an argument.** `chain_set_clip_phase` is dlsym'd, so
  adding a parameter is the one change that cannot be made safely — a chain
  `.so` and a shim disagreeing about a signature is the breakbeat header drift
  that boot-looped a device, and the callee would read an uninitialised
  register as a loop start. The window's start is taken from **`fp[0]`**, the
  fingerprint's geometry half, already pushed in the same call from the same
  parse. A valid phase implies `fp_valid`, so the window is never unknown while
  the phase is known.
- **The document is `V 2`, and a `V 1` one is REFUSED rather than migrated.**
  That is a fact about this feature's history, not a policy: the format never
  left this branch, so the only v1 documents in existence are the author's own
  tests. The version is compared for **equality** so the refusal is loud — the
  two coordinates are indistinguishable per point, so a tolerated v1 would
  place every breakpoint wrong while looking healthy. If a migration is ever
  needed it is exact: each lane's header line carries the `loop_start` it was
  recorded against.

#### Time-addressed, with no length of its own

Clip length is mutable from Move's step editor and extending a clip by adding a
note past the end is routine, so:

- breakpoints are stored **unbounded**; the lane has no length, only the clip's;
- playback wraps at the clip's **current** `loop_len`;
- **nothing is ever rescaled.** Stretching a lane to a new length turns a filter
  sweep into a different filter sweep — the musically wrong answer even though
  it is the tidy-looking one;
- evaluation considers **only points below `loop_len`**, and holds at both ends
  rather than interpolating across the wrap. A dormant point past the end is
  **retained** but cannot bend the audible curve — and in particular cannot do
  so through wrap-around interpolation, which is how a hidden point would
  otherwise become audible while appearing nowhere on screen.

Growing a clip reveals what was recorded there; shrinking it hides the tail;
neither loses data. Interpolation is linear for floats and **stepped for int and
enum**, from the module's own `chain_param_info_t` rather than from anything the
lane stored — a parameter that changed from float to enum must not keep ramping
across its options.

#### Move's clips carry no identity, so a lane can only be keyed to a POSITION

A clip in `Song.abl` has `name` (usually `""`), `color`, `region`, `grooveId`,
`stepEditorScrollPosition`, `notes` and `envelopes` — **no id, no uuid**. A lane
therefore cannot be bound to "this clip". It is bound to a grid position plus
evidence about what was there when it was recorded:

```
key         = (set, track, clip slot, target, param)
fingerprint = (loop_start, loop_len, note count, first note)   at record time
compared    = (note count, first note)                         only
```

**Only the content half is compared.** The note count catches a copy of a
same-length clip; the first note catches a same-length same-density different
clip. **Neither loop field is**, and that is the same decision twice: a clip
that grew is the same clip, and so is a clip whose loop area the user dragged.
Going stale on either is **silent** — the automation simply stops, with no
gesture short of re-recording the pass that brings it back — and a moved loop
costs the lane nothing, because phases are stored **loop-relative**
(`shadow_slot_clip_phase` subtracts `loop_start`). Clip-relative storage was
considered and rejected: `loop_start` is observable by nothing for a clip just
made and then edited, so re-origining would have to guess and would put every
value a bar out while looking healthy. Both loop fields are recorded for
**diagnostics only**.

On a mismatch the lane is **stale: retained, silent, and never guessed at.** A
clip copied into a slot that once held automation does not inherit it. A match
clears both `stale` and `orphaned` — the clip coming back is an undo, and there
is no gesture in the UI that would otherwise un-strand a lane. A *mismatch* only
ever sets `stale`, because `orphaned` is a statement about the clip's
**existence** and only the worker's before/after parse can make it. No
fingerprint at all is a third answer and marks nothing either way; the absent
fingerprint is `{note_count: 0, first_note: -1}`, never all-zero, because note 0
is a real note number and, with no loop field compared, a zeroed `first_note`
plus a zeroed count would match the first clip the lane ever met.

A **deleted clip orphans its lanes; it does not delete them.** Move saves
`Song.abl` about 35 s after an edit, so "absent from the file" is a statement
about the last save and not about the user's intent. Pruning is only ever an
explicit `Clear Lanes`.

(`envelopes[]` in `Song.abl` is **Move's own** clip automation. We never read or
write it. Do not reuse the word "envelope" for a lane.)

#### Phase is DLSYM'd into the chain, never a `host_api_v1_t` field

`chain_set_clip_phase(instance, valid, phase_beats, loop_len, track, clip_slot,
fp_valid, fp[4])` and `chain_set_clip_deleted(instance, track, slot)` are
default-visibility exports of `dsp.so`, resolved in `shadow_chain_mgmt.c` and
called from the shim's per-slot loop. **Not** fields on the host struct: the
front of its `reserved` tail is **+120**, the offset a shipped breakbeat build
over-reads and calls as `get_project_bpm()`, so a live pointer there boot-loops
the device (`CLAUDE.md`, `src/host/plugin_api_v1.h`). The fingerprint crosses as
four doubles so the shim never has to agree with `lane_store.h`'s layout.

**Unknown phase refuses, and is never phase 0.** Phase has three answers — a
number, "nothing is playing", and "I could not tell" — and the third reaches the
chain as a refusal. It is stored as **NaN**, not as the caller's zeroed local:
`0.0` is a legal phase (the loop start), so a reader that forgot the
`clip_phase_valid` gate would otherwise play every lane's first breakpoint
forever, in silence. With no phase, `lane_tick` releases everything it drives —
once, which is what `driving` is for — and ends every recording pass.

#### Recording: a pass ERASES the span it sweeps

The arm is **Move's own Record button**, decoded from its LED on cable 0
(`src/host/rec_arm.h`) and pushed to each slot as `lanes:armed` **on change
only**. Three rules, measured on hardware 2026-09-12:

- Record is **CC 86**. Not 118 — `schwung-spi` documents 118 as the same
  physical button as Sample, and 118 never appeared in the arm sequence.
- The **animation is carried in the channel nibble** (0x06–0x0F) and the value
  byte is the colour it animates to. Any animation channel means *flashing* —
  armed, or counting in — and records nothing, so no rate measurement and no
  colour comparison is needed.
- **Static alone does not mean recording.** The resting state is static and
  non-zero too (122 and 124 were both observed). The discriminator is **full
  brightness**, `d2 == 127`, read as a brightness rather than as a palette
  index. And it is evaluated **once per frame, from the last CC 86 in it**:
  Move writes the base colour statically and *then* applies the animation, so a
  burst contains `static 127` immediately followed by `blink`, and acting on
  each message in turn reports one frame of RECORDING every time a count-in
  starts. One frame is enough to record a breakpoint.

While armed *and* the phase is known, a write to a parameter that resolves
through `find_param_by_key` creates a lane implicitly and records a breakpoint
at the phase sampled **on the callback at the moment of the write** — a UI frame
is ~23 ms and a knob sweep is faster than that, so frame-time phase would
quantize a sweep into steps. A key too long for `lane_t`'s fields is **refused,
not truncated** (two over-length keys would collide onto one stored string), and
a full store records nothing rather than pretending to.

**Playback cannot record itself**, structurally rather than by a flag:
`chain_mod_set_param_string` writes the sub-plugin's `set_param` directly and
never re-enters `v2_set_param`, which is the only caller of the recorder. If
that ever stops being true a lane compounds its own curve every loop, silently
and worse every bar; the unit test asserts it.

**A second pass over an existing lane erases the span it swept.** That is
`lane_record_point`, and it is *not* `LANE_MIN_POINT_BEATS`. The thinning window
is ~5 ms at 120 BPM — below a knob detent's spacing — and for a while it was
claimed to do punch's job as well. It cannot: a second pass's writes land tens
of milliseconds from the first pass's, so they miss the window entirely and the
two curves **interleaved** (20 → 90 → 40 → 91 → 60). The user heard it as
"super jumpiness". Replacing a pass is a *swept region*, not a point window:
everything strictly between the previous write of **this** pass and this one is
deleted. The first write of a pass erases nothing, and neither does a gap wider
than `LANE_PASS_GAP_BEATS` (one beat — a knob detent stream is an order of
magnitude inside that, so a whole beat with no write is a hand that stopped). A
wrapped sweep is one gesture: the swept span is `(prev, loop_len)` plus
`[0, phase)`. Every pass therefore needs an **end** — `lane_record_end_all` on
disarm, and `lane_record_end` when a lane stops being the one playing — or the
first write of the next take erases back to wherever the last one happened to
stop.

#### The `lanes:` param surface, behind ONE dispatch

| Key | Direction | Meaning |
|---|---|---|
| `lanes:state` | get / set | The whole store as one opaque document. `0` bytes means *this slot has no automation*; `-1` means the host's buffer was too small, which the UI must read as a **failed** read and not as an empty one. A set is **all or nothing** — a malformed document leaves the store exactly as it was. |
| `lanes:armed` | get / set | Move's Record button, pushed by the shim on change. Readable because the UI has no other source for it. Disarming releases nothing and clears nothing — a take must keep driving its parameter the moment Record goes out. |
| `lanes:clear` | set | Throw this slot's automation away. Releases first, then resets. Guarded on a non-zero value so a stray `=0` cannot destroy a set's automation. |
| `lanes:cleared` | get | How many lanes the last clear threw away. Written unconditionally, so a second press answers `0` rather than repeating the first take's number. |
| `lanes:phase_valid` | get | **Why** a recording was refused. `0` is *unknown*, not phase zero. |

All of them arrive through **one** branch in `v2_set_param` / `v2_get_param`
that forwards the key past `lanes:` to `lane_param_set` / `lane_param_get`.
That is not tidiness: `chain_host.c` is pinned under 2900 lines by
`tests/host/test_chain_host_file_split.sh` and was sitting two under it, so a
per-key ladder there would have made the next lane key a choice between the pin
and the feature. An unknown subkey returns `-1` — the dispatch swallows the
whole prefix, so there is nothing left to fall through to and it must say so
rather than answer `""` and be believed.

`stale`, `orphaned`, `driving` and the punch and pass fields are **runtime, not
content**, and are not in the document. `lane_tick` recomputes the first two
from the live clip every block, and only a *match* clears `stale` — a persisted
`stale` would strand a lane whose clip is present with no gesture anywhere that
un-strands it.

Lanes ride with the **set**: `set_state/<uuid>/lanes_<i>.json`, written by the
existing autosave (see `docs/SHADOW_UI.md`). A clip position means nothing in
another set. There is no second serializer.

#### Move's own screen carries the length, and we read it rather than model it

Make a clip in Move's step editor, press Play, try to record automation:
refused. `T1 -`, `loop_len 0.00`, `has_phase false`. The clip is not in
`Song.abl` yet — Move saves about **35 s** after an edit — so there is no
length; with no length there is no phase; with no phase, recording refuses
rather than guessing. That is the hole, and Move's step-editor screen carries
both missing facts.

Measured on hardware 2026-09-12, a 5-bar loop (`src/host/step_strip.c`):

```
row 59       (1-23) (26-49) (52-74) (77-100) (103-126)   5 segments = 5 bars
rows 58-60   thicker over one segment                    the DISPLAYED bar
playhead     a 1 px INTERRUPTION in the strip, plus a stub at rows 55-57/61-63
```

- **Segments are 23–24 px with 2 px gaps**, and the 1 px playhead against a
  2 px gap is what makes the two separable: a hole splits the strip into bars
  only when it is **at least 2 wide**, so the playhead cannot inflate the bar
  count. A playhead sitting *on* a boundary widens that gap to 3 — still one
  boundary, and its column is then named by the stub alone.
- **It is page-independent**, which is what makes it better than the step LEDs:
  measured drawn at bars 3 and 4 while bar **5** was the displayed one. The LED
  playhead is visible only while the displayed page IS the playing page, so on
  a long clip it is dark for 15 bars in 16.
- **It does not say where the loop BEGINS** — confirmed by eye. Which costs
  nothing, because lane phases are **loop-relative**.
- **Opportunistic, not a clock.** The bar count does not change while you
  record, so a reading from ten seconds ago is as good as a live one; that is
  what keeps this to one cached fact per track rather than a second position
  pipeline. Phase stays with the existing anchor machinery.
- **The decode runs where the frame COMPLETES** (the SPI callback, off
  `pin_accumulate_slice`'s new completion return), because the frame is only
  whole at that instant and because the selected track must be read *then* —
  the editor shows one track, and pairing the reading with whatever is selected
  200 ms later attributes a bar count to the wrong clip.
- **Do NOT build a model of Move's sequencer UI.** Read Move's answer off the
  screen; never track its modes, pages or loop points. Every time this work
  drifted that way it produced a bug.
- **A SEGMENT IS A BAR, ROUNDED UP — so the strip answers a RANGE.** Move's
  manual, on this strip: *"Each line represents a bar… A thick line specifies
  that the bar is selected and part of the loop… A plus icon signifies that the
  bar is outside of the loop."* Measured on an 11/8 set (5.5 quarters/bar),
  against the file:

  | clip | file | bars (ceil) | 16-step pages | strip drew |
  |---|---|---|---|---|
  | T2 | 16 q | 2.91 → **3** | **4** | **3** |
  | T1 | 12 q | 2.18 → 3 | 3 | 3 |
  | T4 | 4 q | 0.73 → 1 | 1 | 1 (thin) |

  T2 is the only clip that separates the two models, and it says bars. So
  `quarters ≈ segments × quarters_per_bar`, as a **ceiling**: 3 segments under
  11/8 means (11.0, 16.5], exact only when the loop is a whole number of bars
  — which a clip Move created in the current signature is. Anything needing
  better than bar resolution must wait for the file.
  **Two wrong answers preceded this**, both from coincidences: `bars × 4` (a
  4/4 assumption), then "a segment is a 16-step page", which came from T1
  agreeing *exactly* through pages — 12 quarters is 3 pages **and** 3
  bars-rounded-up. One coincidence, believed twice; T2 is what broke it.
- **A ONE-BAR LOOP DRAWS A THIN LINE WITH NO THICKENING**, straight from the
  manual — *"if a loop contains only one bar, a thin line is displayed
  instead"* — and the displayed-bar gate therefore refused **every new clip**,
  which is the case the reader exists for. The 1-bar T4 clip came back as gate
  6 twice before the manual explained it. It is accepted now and **flagged**
  (`single_thin`), because it is the one shape indistinguishable from an
  unrelated full-width line; with two or more segments the thickening is still
  required. Surveyed by driving Move through Menu, Loop Mode and the screen
  Back lands on: row 59 empty on all three. Three screens is not proof, which
  is what the flag is for.
- **The step grid runs 1/8t to 1/64, so `stepEditorResolution` must parse a
  TRIPLET suffix.** The old `sscanf("\"%d/%d\"")` accepted `"1/8t"` as a
  straight eighth — its two `%d`s succeed and the trailing literal quote fails
  without changing the return count — a silent 50% error in every step index.
  Per the manual the grid divides a **bar**, and above 1/16 a bar spans several
  pages of step buttons.
- **Move's lit step is BAR-relative, then PAGE-wrapped** — and this is the
  mapping a step p-lock needs:

  ```
  idx = (step_in_CLIP mod steps_per_bar) mod 16      steps_per_bar = qpb / res
  ```

  The manual says the grid divides a **bar**, and that above 1/16 a bar spans
  several pages of step buttons — an 11/8 bar at 1/16 is 22 steps, so it pages
  **16 + 6** even at the default grid. Measured: 16 sightings on the 11/8 set
  where Move's index was always our loop-relative step **+10**, arriving in
  bursts of six with a ~43-step gap. The loop starts at quarter 8.0 = 32 steps,
  `32 mod 22 = 10` — it begins ten steps into bar 2, so only steps 10–15 of
  that bar's *first* page are ever displayed while it plays. The model
  reproduced all 16 offline, and on hardware the check went from 0/16 to
  **26/26, 100%, zero misses**.
  **Three attempts, each missing one term:** a bare `% 16` (no bar), then
  `% steps_per_bar` with no page wrap *and* a loop-relative step — which was
  worse than either, taking the page column from 16/16 to 0/18. In 4/4 at 1/16
  all three coincide, which is why a 4/4 device reads 99.3% whatever this line
  says. Grids coarser than 1/16 (a 4/4 bar is 8 steps at 1/8) are **not
  measured**: the model predicts an index in 0–7 there, and nothing has
  checked it.
- **VALIDATED ON HARDWARE 2026-09-12.** With the editor open the reading
  followed the selection across three tracks — 4, 5 and 3 bars — the decoded
  displayed bar agreed twice with Move's *independently announced* "Bar N",
  and the playhead swept 1→126 and wrapped while the displayed bar stayed put
  (the page-independence claim, and one of the two committed fixtures is that
  case in Move's own pixels: playhead in bar 2, bar 3 bold). Session and Set
  Overview refused with "nothing on the row".
- **A frame can be TORN, and the cache waits for a second reading because of
  it.** The accumulator stitches six slices, so a frame can straddle two of
  Move's screen updates. Paging and switching tracks each produced a *one-off*
  refusal on a different gate (not-full-width, non-uniform, no-displayed-bar)
  — which is what a half-drawn strip looks like, and refusing is the safe
  direction. But a torn frame could in principle read uniform and WRONG, and a
  wrong bar count is a wrong loop length, so `g_bars[]` commits only after
  `STEP_STRIP_CONFIRM` consecutive readings agree (~33 ms at 30 fps). An
  invalid frame breaks the run without clearing the cache.
  **The refusing frame itself was NOT captured**: the dump trigger writes the
  *next* complete frame, so by the time it landed the screen had moved on —
  the file is a healthy editor screen, not the refusal. Capturing the frame a
  decode rejected needs the copy taken inside the decode.
- **The gates are the risky half, so this is a DIAGNOSTIC first.** The geometry
  is measured; the rejection gates (full-width span, uniform segments, the
  displayed-bar thickening) are reasoned, and a false positive is a *wrong loop
  length*. So `clip_state.json`'s `step_strip` block and the manager's
  `/clip-state` panel report it and **nothing depends on it**: the hardware
  pass must see `valid` with the right bars on the editor and a named `reject`
  on Move's other screens before the loop-length fallback is wired in. The
  displayed-bar gate is load-bearing for the one-bar case — a 1-bar loop is a
  single solid 126 px run with no gaps for the uniformity test to measure, so
  without it any full-width line would read as a one-bar clip.

#### The budgets are small, and knowingly too small

`LANE_MAX` is **32** per slot — 8 clip slots × 4 parameters, because the key
spans clips *and* parameters — and `LANE_POINTS_MAX` is **64** per lane
regardless of loop length. The memory is irrelevant (a `lane_t` is ~1.1 KB and
`lane_store_t` sits on `chain_instance_t`, **not** inside `patch_info_t`, which
is a stack local on the SPI callback). **What caps it is the param contract:**
`lanes:state` is served as one param value, so `LANE_SERIAL_MAX_BYTES` must stay
under `SHADOW_PARAM_VALUE_LEN`, which a `_Static_assert` in `lane_serial.c`
enforces. At 32 lanes the worst-case document is ~104 KB against a 128 KB
ceiling, and ~40 lanes is the hard wall — so do not raise `LANE_MAX` without
reading that assert.

64 points is coarse for a long clip and 32 lanes is few for a kit. Scaling it —
a **shared point pool** so a dense lane can borrow from an empty one, a point
**density per beat** rather than per lane, and a **per-clip transfer key** so the
document is fetched a clip at a time instead of whole — is designed and
**deliberately deferred to a follow-up**. None of it exists today. A full lane
degrades resolution rather than dropping the gesture (the write replaces its
nearest point and counts a `full_hits`), because a lost write mid-sweep is a
hole the user can neither see nor fix.
