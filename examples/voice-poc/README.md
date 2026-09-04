# Voice POC

A module that declares its performance surface, for verifying the layout and
voice contract end to end. It ships no DSP: the whole declaration is a static
`ui_hierarchy` in `module.json`, which the chain host caches and serves — the
path most module authors will use.

## What it demonstrates

| Thing | Where |
|---|---|
| `layout: "drums"` — declared, not inferred | top of `ui_hierarchy` |
| Sibling voices, each its own page | `kick` (36), `snare` (38), `hat` (42) |
| A page that is **not** a voice | `reverb` — navigable, declares no `note`, sounds nothing |
| An optional free-string `role` | `kick` / `snare` / `hat` |
| A template rack with declared names | `pads` — 4 instances, `child_note_base: 60`, `child_names` |
| Module-owned focus (sibling shape) | `focus_param: "cur_voice"`, whose value is a **level name** |

The one shape it does **not** cover is a rack declared at `root` itself, which
is what mrdrums does. That case is pinned instead by a regression test built
from the real captured contract, in `tests/host/test_voices.sh` — a POC written
by the same person as the code can share the code's blind spot, and that one
already did once.

## Verify off-device

```bash
node -e '
const m = JSON.parse(require("fs").readFileSync("examples/voice-poc/module.json", "utf8"));
const h = m.capabilities.ui_hierarchy;
import("./src/shared/param_pages/voices.mjs").then(V => {
  console.log("pad_layout:", V.padLayoutOf(h));
  for (const v of V.voicesOf(h)) console.log(" ", v.index, v.level, v.name, "note=" + v.note);
});'
```

Expected: `pad_layout: drums`, then seven voices — kick/snare/hat at 36/38/42, then
the four pads at 60–63 carrying their declared names — and **no Reverb**. If
Reverb appears, the page-versus-voice rule is broken.

Render the pages and look at them:

```bash
node tools/param-pages/preview_knob_card.mjs voice-poc
```

## Verify on device

Load it into a chain slot and open its knob grid:

- The instance picker shows **Tom Lo / Tom Hi / Rim / Clap**, not "Pad 1 … Pad 4"
- Playing note 38 moves the grid to **Snare**; playing 61 moves it to **Tom Hi**
- Writing `cur_voice` moves the grid, and picking from the list writes it — they
  are the same write, so they cannot disagree
- **No pad LED changes colour.** Move owns the pads; the follow path reads and
  navigates and never writes MIDI out

To exercise the `last_note` fallback instead of `focus_param`, remove
`focus_param` from the copy you load. The grid should then follow played notes
through `synth:last_note`. With `focus_param` present, `last_note` must not be
read at all — exactly one input is live per module.
