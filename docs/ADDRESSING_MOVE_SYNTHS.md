# Addressing Move Synths from Tools, Overtake Modules, and Chain MIDI FX

Implementation guide for sending MIDI to Move's native track instruments
(or to Schwung shadow-chain slot synths) from a module you're writing.

> For the historical design journey of getting here, including the echo
> race and failed approaches, see [MIDI_INJECTION.md](MIDI_INJECTION.md).
> This doc covers the shipped APIs.

## The one routing rule

```
Module's output channel = Move track's MIDI In channel
                         (or Schwung slot's Receive Channel)
```

Move tracks default to `track N ↔ channel N` but the user can reassign
either side. Whatever the receiving side is listening on, send on that
channel. Channels 1–4 reach Move's four tracks in the default config;
channels 5–16 have no native Move track in the default mapping but can
still reach a Schwung slot whose `recv` is set to match.

## Two paths exist

| Path | Use when | Limitation |
|------|----------|------------|
| **Chain MIDI FX with Schw+Move** | Your MIDI FX transforms a pad-driven stream (chord, scale-quantizer, harmonizer) and you want Move's track to play the transformed result additively | Single-pad continuous generators (arp on a single held pitch) can't be reliably disambiguated from pad releases — the held pitch stays silent on Move native. Works cleanly when FX output pitches differ from the held pad. |
| **Tool / overtake with direct inject** | Your module generates MIDI on its own clock (step sequencer, euclidean generator, arpeggiator-as-tool, drum machine) — no pad-echo conflict | None specific to injection. Standard rate limits still apply. |

Both paths share the same underlying shim infrastructure
(`/schwung-midi-inject` SHM + one drain) and the same rate limits.

## Writing a generator tool

Reference implementation: `src/modules/tools/seq-test/` (~400 lines
total) — a 4-step arp that validates the whole path end-to-end.

### 1. `module.json`

```json
{
    "id": "your-tool",
    "name": "Your Tool",
    "version": "0.1.0",
    "author": "You",
    "description": "…",
    "component_type": "tool",
    "dsp": "dsp.so",
    "tool_config": {
        "interactive": true,
        "skip_file_browser": true,
        "overtake": true
    },
    "capabilities": {
        "midi_out": true,
        "suspend_keeps_js": true,
        "button_passthrough": [85]
    }
}
```

Key choices:

- `component_type: "tool"` — accessed from Tools menu.
- `tool_config.overtake: true` — takes the full UI while active.
- `capabilities.suspend_keeps_js: true` — Back suspends the UI but the
  DSP keeps ticking; Shift+Back fully exits. Essential for a sequencer
  that should keep playing while the user browses Move.
- `capabilities.button_passthrough: [85]` — the Play button still
  reaches Move (so Move's transport stays in sync) while your module
  also handles its own play/stop.

### 2. DSP plugin (`dsp/your-tool.c`)

Implement the v2 plugin API. Capture the host pointer in
`move_plugin_init_v2` and stash it on each instance:

```c
#include "host/plugin_api_v1.h"

static const host_api_v1_t *g_host = NULL;

typedef struct {
    const host_api_v1_t *host;
    /* …your state… */
} inst_t;

static void* create_instance(const char *dir, const char *json) {
    inst_t *s = calloc(1, sizeof(*s));
    s->host = g_host;           /* picked up from init */
    /* …init state… */
    return s;
}

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}
```

### 3. Injecting MIDI to Move

Build a 4-byte USB-MIDI packet with **cable 2** (external USB) and call
`host->midi_inject_to_move()`:

```c
static void emit_note_on(inst_t *s, uint8_t channel, uint8_t note, uint8_t vel) {
    if (!s->host || !s->host->midi_inject_to_move) return;
    uint8_t pkt[4] = {
        (2 << 4) | 0x09,              /* cable 2, CIN=note-on */
        0x90 | (channel & 0x0F),      /* note-on status + channel */
        note, vel
    };
    s->host->midi_inject_to_move(pkt, 4);
}

static void emit_note_off(inst_t *s, uint8_t channel, uint8_t note) {
    uint8_t pkt[4] = {
        (2 << 4) | 0x08,              /* cable 2, CIN=note-off */
        0x80 | (channel & 0x0F),
        note, 0
    };
    s->host->midi_inject_to_move(pkt, 4);
}
```

**CIN must match the status type:**

| Message type | Status high nibble | CIN |
|--------------|-------------------|-----|
| Note-off     | 0x80              | 0x08 |
| Note-on      | 0x90              | 0x09 |
| Poly AT      | 0xA0              | 0x0A |
| CC           | 0xB0              | 0x0B |
| Program      | 0xC0              | 0x0C |
| Channel AT   | 0xD0              | 0x0D |
| Pitch bend   | 0xE0              | 0x0E |

**Cable must be 2** for standard MIDI semantics. Cable 0 is Move's
internal prefix protocol (pads 68–99, steps 16–31, track CCs 40–43 —
see `docs/SPI_PROTOCOL.md`) — injecting pitched notes there won't reach
track instruments.

### 4. Timing from `render_block`

`render_block` fires every 128-sample audio block (~2.9 ms at 44.1
kHz). That's your tick for clocking note events. Query BPM from the
host if your module wants to follow project tempo:

```c
static void render_block(void *instance, int16_t *out, int frames) {
    inst_t *s = (inst_t *)instance;
    memset(out, 0, frames * 2 * sizeof(int16_t));  /* silent audio */
    if (!s->running) return;

    s->samples_to_next -= frames;
    if (s->samples_to_next > 0) return;

    /* Step fires */
    if (s->host && s->host->get_bpm) s->bpm = s->host->get_bpm();
    emit_note_off(s, s->channel, s->last_note);
    uint8_t next_note = compute_next_note(s);
    emit_note_on(s, s->channel, next_note, 100);
    s->last_note = next_note;
    s->samples_to_next += compute_step_samples(s);
}
```

### 5. UI (`ui.js`)

Standard Schwung JS module surface:
`globalThis.init`, `.tick`, `.onMidiMessageInternal`, `.onMidiMessageExternal`, `.onUnload`.

Talk to your DSP via `host_module_set_param(key, val)` and
`host_module_get_param(key)` — the same pattern chain modules use. The
UI is just a controller for the DSP's state.

For the `suspend_keeps_js` lifecycle: `init` is called once on first
entry AND on every resume. Read the DSP's current state in `init` so
your UI reflects what's actually running. Example from seq-test:

```javascript
globalThis.init = function() {
    running = parseInt(getDspParam("running")) || 0;
    channel = parseInt(getDspParam("channel")) || 0;
    step    = parseInt(getDspParam("step")) || 0;
    needsRedraw = true;
    draw();
};
```

## Writing a chain MIDI FX that hits Move (Schw+Move)

If your MIDI FX reacts to pad/keyboard input and produces transformed
output (chord, scale-quantizer, arpeggiator, etc.), it's a chain MIDI
FX, not a tool. Nothing special needed in the module itself — the slot
handles everything:

1. User loads your MIDI FX on a slot
2. User toggles **MIDI FX = Schw+Move** in the slot's settings
3. Chain host injects every one of your MIDI FX's output messages into
   Move's MIDI_IN on the slot's forward channel

Your MIDI FX module's `process_midi` / `tick` callbacks behave exactly
the same in Post and Pre mode — the only difference is whether the host
also injects to Move behind the scenes.

**Do make sure your MIDI FX preserves the channel** on output messages
(see `src/modules/midi_fx/arp/dsp/arp.c` for the pattern) — hardcoding
channel 0 would cause your output to always route to Move track 1
regardless of what the user intends. Inherit from the input's channel.

## Limits and guarantees

### Rate limit

The shim drains the inject SHM at **8 packets per SPI tick** (~2.9 ms).
A burst of 64 packets (the SHM ring's capacity) takes 8 ticks to drain
(~24 ms). If you write faster than this sustained, packets get dropped
silently.

In practice: a 16-step sequencer at 1/16 notes at 120 BPM emits 8
note-on/off pairs per second (16 pkts/s) — well under the limit. A
dense drum machine with 8 voices at 1/32 at 240 BPM tops out around
256 pkts/s (~0.74 pkts/tick) — still fine.

### Defer after cable-0 activity

The drain holds SHM contents for 2 frames (~6 ms) after any cable-0
event appears in MIDI_IN (pad press, button, knob touch). This is
unavoidable — injecting cable-2 events in the same tick as live
cable-0 hardware input races Move's firmware and has been observed to
crash the shim. In practice the ~6 ms delay is inaudible.

### No echo filter needed for pure generators

Move echoes every injected event on MIDI_OUT cable 2 — but a generator
tool doesn't respond to MIDI_OUT cable 2 (it's not a chain slot), so
the echo has nowhere to loop back to. You can ignore echoes entirely.

If your tool *does* listen to MIDI input (say, a sequencer that
watches for external-MIDI triggers), filter echoes yourself by
tracking which notes you've recently emitted. The chain's
`pre_mode_is_echo` in `src/modules/chain/dsp/chain_host.c` shows the
pattern (refcount bumped on inject, checked on incoming).

### NULL-safety

On non-shadow host builds, `host->midi_inject_to_move` is NULL. Always
guard:

```c
if (!s->host || !s->host->midi_inject_to_move) return;
```

## Testing

`src/modules/tools/seq-test/` is the canonical end-to-end test. It's
excluded from analytics census (`INTERNAL_IDS` in both `schwung_host.c`
and `shadow_ui.js`) but ships in the build so you can run it on-device
to verify the path when iterating on this area.

Enable the unified log to see `MIDI inject: drained N/N pkts` lines
confirming packets reach MIDI_IN:

```bash
ssh ableton@move.local 'touch /data/UserData/schwung/debug_log_on'
ssh ableton@move.local 'tail -f /data/UserData/schwung/debug.log'
```

Silent drains + no sound usually means either:

- **Channel mismatch** — receiving side isn't listening on the channel
  you're sending. Verify the Move track's MIDI In setting or the slot's
  Receive Channel.
- **`midi_inject_to_move` is NULL** — the host wasn't a shadow-mode
  Schwung host (check your test environment) or the overtake
  `host_api` wasn't wired for injection (fixed for all overtake DSPs
  as of `3e307a66`; but custom host setups may need to wire
  `shadow_chain_midi_inject` themselves).
