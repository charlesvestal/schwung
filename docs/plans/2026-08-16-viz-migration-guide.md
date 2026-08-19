# Declaring `viz` in a module — migration guide

**For:** a Claude Code session working through one module repo at a time.
**Contract:** `docs/MODULES.md` → "Parameter visualisations (`viz`)".
**Why:** Schwung's knob pages draw parameter *groups* as pictures — an ADSR as an
envelope, cutoff+resonance as a filter curve. A module that declares its groups
gets the right picture. A module that says nothing gets whatever a name-and-range
detector guesses, which is usually right and sometimes embarrassing.

46 of the 104 catalog modules are in `charlesvestal/` repos. This guide is for
working through those.

---

## Before you start

**Read the module, not just its parameter table.** The single most damaging thing
you can do here is declare a group that is not real — four params that happen to
be called attack/decay/sustain/release but drive something else, three "gain"
params that are crossover frequencies. A wrong picture is worse than no picture,
because a plain knob is honest and a wrong envelope lies.

If you are unsure whether a group is real, **leave it undeclared**. The detector
will make the same guess you would have, and it will be visible as a guess.

---

## 1. Find what the module declares

Two shapes are common:

**A `param_def_t` table** (`src/dsp/param_helper.h` + a table in the plugin):

```c
static const param_def_t moog_params[] = {
    { "cutoff",    "Cutoff",    PARAM_TYPE_FLOAT, IDX_CUTOFF, 0.0f, 1.0f },
    { "resonance", "Resonance", PARAM_TYPE_FLOAT, IDX_RESO,   0.0f, 1.0f },
    ...
};
```

`param_def_t` already carries the viz fields (schwung #216), so there is no
plumbing left to write — copy the current `src/host/param_helper.h` over the
module's stale copy and fill in three fields per declared param:

```c
{ "cutoff",    "Cutoff",    PARAM_TYPE_FLOAT, IDX_CUTOFF, 0.0f, 1.0f, "filter", "cutoff",    NULL },
{ "resonance", "Resonance", PARAM_TYPE_FLOAT, IDX_RESO,   0.0f, 1.0f, "filter", "resonance", NULL },
```

`viz_group` / `viz_role` / `viz_kind`, in that order. Leave all three NULL — or
just omit them — for a param you are not declaring, which is most of them.
`viz_kind` is usually NULL too: the host derives the kind from the roles, and
you only set it for a single-param graphic (`"fader"`, `"waveform"`,
`"switch"`) or to `PARAM_VIZ_NONE` to suppress a detector.

Two things to check when you copy the header in:

- **The copies drift.** Six module repos carry their own `param_helper.h`, and
  they are not all at the same revision. Diff before you copy, in case the
  module has local edits worth keeping.
- **Positional initialisers now warn.** Adding fields to `param_def_t` means
  entries that stop short trigger `-Wmissing-field-initializers` under
  `-Wextra`. Harmless unless the module builds with `-Werror`, in which case
  spell out the trailing NULLs.

Declare it in the table, never in a separate lookup keyed by param name. Braids
originally did the latter and it is the one thing about braids not to copy: two
tables keyed by the same strings drift apart silently, because renaming a key
detaches the graphic without any compile error.

**A hand-written JSON string** in `get_param`:

```c
if (strcmp(key, "chain_params") == 0) {
    return snprintf(buf, len, "[{\"key\":\"cutoff\",\"name\":\"Cutoff\",...}]");
}
```

Here you edit the literal directly. Watch the buffer length — several modules
size this buffer exactly, and adding `viz` objects will overflow it silently.
**Check the buffer size and grow it.**

A module can be both: braids keeps a hand-assembled loop, because `param_def_t`
has no room for the percentage unit its params carry, but still declares viz in
the table and calls `param_helper_viz_json()` for the field. Prefer that over
pasting JSON literals — one place knows the field's shape, and the declaration
stays next to the param. The entry margin that loop reserves must clear the
longest entry it can now emit; `PARAM_HELPER_ENTRY_MARGIN` (256) is sized for
that, and braids' longest is 151.

Start by dumping what the module currently declares:

```bash
node tools/param-pages/validate.mjs <module-id>        # findings, incl. inferred groups
node tools/param-pages/preview.mjs <module-id> --all   # every page, as it looks now
```

**Both read the checked-in fleet capture, not your device.** So they show the
module as it was when `tests/fixtures/module-contracts.json` was captured, which
is the right baseline for step 1 but useless for step 4 — see Verify.

---

## 2. Decide the groups

Work from the module's own documentation and DSP, not from the key names. For
each candidate group ask:

- **Is it one thing?** An ADSR that drives the amplifier is a group. "Attack" on
  the amp and "Decay" on the filter are two groups, and grouping them draws a
  nonsense envelope.
- **Are the roles right?** A 2-stage AD is `attack` + `decay`, not
  `attack` + `release`. The picture differs.
- **Is an EQ band actually a gain?** `eq` roles are band **gains** — bipolar,
  boost/cut. A crossover frequency or a per-band Q is not a band; declare
  `viz: false` on those if a detector keeps picking them up.

Name groups after what they drive: `amp`, `filter_env`, `osc1_lfo`. The id never
appears on screen; it only has to be unique within the module.

---

## 3. Declare

Add `viz` to each member. Adjacent declaration matters — the page planner seats a
group on one row, and a group split across two pages cannot be drawn:

```json
{ "key": "amp_a", "name": "Attack",  "viz": { "group": "amp", "role": "attack"  } },
{ "key": "amp_d", "name": "Decay",   "viz": { "group": "amp", "role": "decay"   } },
{ "key": "amp_s", "name": "Sustain", "viz": { "group": "amp", "role": "sustain" } },
{ "key": "amp_r", "name": "Release", "viz": { "group": "amp", "role": "release" } }
```

Single-param graphics need no group:

```json
{ "key": "osc_wave", "type": "enum", "options": ["Saw","Square","Tri"], "viz": { "kind": "waveform" } },
{ "key": "volume",   "type": "float", "viz": { "kind": "fader"  } },
{ "key": "bypass",   "type": "toggle", "viz": { "kind": "switch" } }
```

And to stop a detector guessing wrong:

```json
{ "key": "low_xo", "name": "Low Crossover", "viz": false }
```

---

## 4. Verify

`viz` lives in the DSP, so it only appears once the module is rebuilt and the
host reads it from the running plugin. There is no way to check it from source
alone.

```bash
cd <module-repo> && ./scripts/build.sh && ./scripts/install.sh
```

Then **look at the module on the device** — that is the verification. Turn to
the level you changed and check the graphic is there and reads correctly.

`validate.mjs` cannot confirm this for you. It reads the checked-in fleet
capture (`tests/fixtures/module-contracts.json`, captured 2026-07-15), so a
module you have just migrated still reports its groups as `viz-inferred`
until that capture is refreshed — braids did for the whole of its first
release. The findings are still worth reading; just do not treat them as a
statement about the running module.

Refreshing the capture means a full device run of
`tools/param-pages/dump_contracts_device.js`, which loads every module into a
probe slot in turn. It is intrusive, its device half is marked unverified on
hardware, and it will change the `param_pages_viz` snapshots. Worth doing
periodically for the fleet, not per module.

What you want to see on the device:

- the graphic you declared, on the level you declared it
- roles in the right places — an ADSR whose sustain moves the wrong segment is
  a wrong declaration, not a rendering bug
- nothing that was a working knob before has become undrawable

**If the buffer overflowed**, `chain_params` comes back truncated and the module
loses params entirely — they simply stop appearing on the pages. Count the knobs
against what step 1 recorded.

---

## 5. Ship it

Normal module release: bump `src/module.json`, commit, tag, push, add release
notes. Then bump `min_host_version` in `module-catalog.json` **only if** the
module now needs a host that understands `viz` — a host that does not will ignore
the field, so in most cases no bump is needed.

---

## Working order

Do the modules where the picture is most obviously right first, so mistakes are
easy to spot:

1. **Synths with a plain ADSR** — obxd, hera, moog, braids. One group each,
   unmistakable.
2. **Anything with cutoff + resonance** — the filter curve is the second most
   common graphic.
3. **Modules with waveform enums** — `viz: {kind: "waveform"}` on the shape param.
4. **Samplers** — sf2, sfz, minijv: `sample` + `position`.
5. **EQ-ish effects last.** They are the easiest to get wrong and the hardest to
   notice: check the range is bipolar before calling something a band gain.

---

## What "done" looks like

Not "every param has a `viz`". Most params are plain knobs and should stay that
way. Done is:

- every group the module really has is declared
- nothing is declared that is not a real group
- the detector is left to handle nothing important

The measure to watch is in `validate.mjs`: how much of a module's graphics come
from declarations versus inference. If the fleet is still mostly inference in a
year, the declaration path failed and the detectors became a maintenance
treadmill instead of a fallback.

That measure is only as current as the capture behind it. `validate.mjs` prints
the capture's date on every run; if it is months old, a falling `viz-inferred`
count means nothing, because the modules migrated since are still being read at
their pre-migration state. Refresh the capture before drawing any conclusion
from the trend.

---

## Migration tracker (`charlesvestal/` modules only)

46 of 104 catalog modules live in `charlesvestal/` repos; the other 58 (community
+ third-party) are out of scope for this pass — inference covers them. Ordered
per "Working order" above; update the row (and commit the module repo) each time
one ships.

### 1. Plain-ADSR synths

| Module | Repo | Status |
|---|---|---|
| braids | schwung-braids | **done, shipped v0.2.6** — amp/filter_env groups + filter cutoff/resonance, declared in the param table (f72c0ec, bf5c9df) |
| obxd | schwung-obxd | not started |
| hera | schwung-hera | not started |
| moog | schwung-moog | not started |

### 2. Cutoff + resonance

| Module | Repo | Status |
|---|---|---|
| dexed | schwung-dx7 | not started |
| sf2 | schwung-sf2 | not started |
| sfz | schwung-sfz | not started |
| minijv | schwung-jv880 | not started |
| surge | schwung-surge | not started |
| osirus | schwung-virus | not started |
| 303 | schwung-303 | not started |
| chordism | schwung-chordism | not started |
| nusaw | schwung-nusaw | not started |
| hush1 | schwung-hush1 | not started |

### 3. Waveform enums

| Module | Repo | Status |
|---|---|---|
| chiptune | schwung-chiptune | not started |
| webstream | schwung-webstream | not started |
| radiogarden | schwung-radiogarden | not started |

### 4. Samplers

| Module | Repo | Status |
|---|---|---|
| mrsample | schwung-mrsample | not started |
| rex | schwung-rex | not started |
| samplerobot | schwung-autosample | not started |
| waveform-editor | schwung-waveform-editor | not started |
| stretch | schwung-stretch | not started |
| stems | schwung-stems | not started |
| tb3po | schwung-tb3po | not started |

### 5. EQ-ish effects (last — easiest to get wrong)

| Module | Repo | Status |
|---|---|---|
| cloudseed | schwung-cloudseed | not started |
| midiverb | schwung-midiverb | not started |
| tapescam | schwung-tapescam | not started |
| psxverb | schwung-psxverb | not started |
| mverb | schwung-mverb | not started |
| tapedelay | schwung-space-delay | not started |
| junologue-chorus | schwung-junologue-chorus | not started |
| nam | schwung-nam | not started |
| ducker | schwung-ducker | not started |
| clap | schwung-airwindows | not started |
| gate | schwung-gate | not started |
| keydetect | schwung-keydetect | not started |
| vocoder | schwung-vocoder | not started |
| usefulity | schwung-usefulity | not started |
| chowtape | schwung-chowtape | not started |
| ambiotica | schwung-ambiotica | not started |
| filter | schwung-filter | not started |
| midi-player | schwung-midi-player | not started |

### Not applicable (no meaningful knob-page graphics)

| Module | Repo | Why |
|---|---|---|
| airplay | schwung-airplay | streaming receiver, no synthesis params |
| m8 | schwung-m8 | overtake, own full-screen UI |
| sidcontrol | schwung-sidcontrol | overtake, own full-screen UI |
| performance-fx | schwung-performance-fx | overtake, own full-screen UI |
