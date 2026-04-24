# Per-Module Config

Let each module own its settings (schema, values, secrets) in its own
install directory, instead of growing a central
`src/shared/settings-schema.json` and a central secrets dir in
schwung-manager every time an external module wants a knob in the web UI.

## Motivation

Two in-flight PRs surfaced the problem:

- [#62 — Guitar Tuner catalog entry](https://github.com/charlesvestal/schwung/pull/62)
  adds a `tuner_accidental_style` field to core `settings-schema.json`
  for a tool that isn't part of core.
- [#61 — host plumbing for voice-driven tool modules](https://github.com/charlesvestal/schwung/pull/61)
  adds a whole "Assistant" section (provider picker, Gemini/OpenAI keys,
  model names, system-prompt textarea) to core `settings-schema.json`,
  hard-codes `openai_api_key` and `gemini_api_key` into a
  `secretKeyFiles` map in `schwung-manager/main.go`, and introduces a
  shared root-owned `secrets/` directory under
  `/data/UserData/schwung/` to hold the keys.

Both PRs follow the only pattern the current schwung-manager supports:
one monolithic schema file loaded from `shared/settings-schema.json`
(`schwung-manager/main.go:1760`), one flat `shadow_config.json` values
file, one central secrets dir. That forces every external module's
settings to be declared in core. It does not scale, it leaks module
identity into core, and orphans values in `shadow_config.json` when a
module is uninstalled.

Modules already declare other self-contained metadata alongside
`module.json`: `help.json`, `release.json`, `assets.json`. Settings
should work the same way.

## Design

### File layout

Each module's install directory owns its config:

```
modules/<category>/<id>/
  module.json              # existing — unchanged
  ui.js                    # existing — unchanged
  settings-schema.json     # new — immutable, ships in tarball
  config.json              # new — mutable, holds current values
  secrets/                 # new — 0600 files, each named <key>.txt
    <key>.txt
```

- `settings-schema.json` is shipped in the release tarball and is
  immutable from the module's perspective. Install preserves
  `config.json` (see Upgrade behavior below).
- `config.json` holds the currently-saved values as a flat
  `{ "<key>": <value>, ... }` object. Written by schwung-manager.
- `secrets/<key>.txt` holds values declared `"type": "password"` in the
  schema. Keys are never read back through the web UI, only their
  "is set" marker.

This keeps a module a single directory: uninstall removes it atomically,
`tar`-ing the module dir captures its complete state including user
config.

### Schema fragment shape

A minimal fragment, placed at `<module_dir>/settings-schema.json`:

```json
{
  "id": "guitar-tuner",
  "label": "Tuner",
  "items": [
    {
      "key": "accidental_style",
      "label": "Accidental Notation",
      "type": "enum",
      "options": ["Flats (Db, Eb, Gb, Ab, Bb)", "Sharps (C#, D#, F#, G#, A#)"],
      "values": ["flat", "sharp"],
      "default": "sharp"
    }
  ]
}
```

- `id` MUST match the module's `module.json` id. schwung-manager
  discards fragments whose id doesn't match the parent directory.
- `label` is the rendered section/page header.
- `items[]` shape matches today's `SettingsItem` struct (`main.go:159`)
  plus the extensions #61 already adds (`password`, `textarea`,
  `string`, `default`, `rows`, `help`, `help_url`).
- **Keys are local to the module.** No namespacing required — the
  module reads its own `config.json`, so `accidental_style` never
  collides with another module's `accidental_style`.

### Field types

Start with what #61 has, minus AI-specific baggage:

| Type | Storage | Notes |
|------|---------|-------|
| `bool` | `config.json` | existing |
| `enum` | `config.json` | existing |
| `int` | `config.json` | existing |
| `float` | `config.json` | existing |
| `string` | `config.json` | from #61 |
| `textarea` | `config.json` | from #61; multi-line preserved |
| `password` | `<module>/secrets/<key>.txt` (0600) | from #61; never round-tripped through web UI |

A `default` field on the schema item is surfaced when `config.json` has
no value saved for that key.

### Secrets: per-module dir, no root-owned central dir

Each module gets its own `<module_dir>/secrets/`. Files are created by
schwung-manager with `O_CREAT | O_EXCL | O_NOFOLLOW`, mode `0600`,
owned by `ableton:users`.

We drop the root-owned `0710 secrets/` directory from #61. Reasoning:
- The device only has one non-root user (`ableton`). There is no
  lower-privileged user to defend against.
- Putting secrets inside the module directory means uninstall removes
  them atomically.
- The module already owns the directory via the install tarball's
  ownership; no ownership dance.
- The real security properties from #61 we keep: secret bytes never
  live in `shadow_config.json`, never round-trip through
  `/config/values`, and the write uses `O_NOFOLLOW` to block
  symlink-swap.

### schwung-manager changes

**Schema discovery.** On each request that needs schemas (and once at
startup), `loadSettingsSchema()` returns:

1. The core fragment, renamed to `core-settings-schema.json` (or kept
   as `shared/settings-schema.json` but stripped down to core-only
   items: General section, display, input curves, etc.).
2. One fragment per module, discovered by globbing
   `modules/**/settings-schema.json`. Fragment files whose parent dir
   basename doesn't match `id` are logged and skipped.

**Routing.** Two new endpoints:

- `GET /config/modules/<id>` — renders that module's settings page
  using its fragment + its `config.json` values.
- `POST /config/modules/<id>/set` — writes one key to
  `modules/<category>/<id>/config.json`, or to
  `modules/<category>/<id>/secrets/<key>.txt` for password fields.
  CSRF + path validation as today.

The existing `/config` page stays as core settings, with a "Modules"
index at the bottom listing each module that declared a schema, linking
to `/config/modules/<id>`.

**Path constraints.** schwung-manager resolves all module config/secret
writes to `filepath.Join(basePath, "modules", category, id, ...)` and
rejects anything that does not stay under that dir after
`filepath.Clean` (mirrors the existing `default_source` guard in #61).
Modules cannot escape their own directory, by construction.

**`default_source` retained, relative to module dir.** A textarea's
`default_source: "default_system_prompt.txt"` resolves to
`<module_dir>/default_system_prompt.txt`. Not `basePath`. This lets
tools ship a long default prompt as a plain text file in their tarball,
same as #61, but scoped correctly.

### Runtime reads

Modules read their own config at runtime with existing host APIs:

```javascript
const cfg = JSON.parse(host_read_file(module_dir + "/config.json") || "{}");
const val = cfg.accidental_style || "sharp";
```

Secret files are read by the module directly:

```javascript
const key = (host_read_file(module_dir + "/secrets/openai_api_key.txt") || "").trim();
```

No new host API surface. `host_get_setting` stays for core-only
settings (velocity curve, aftertouch); it does not gain module-scoped
variants.

### Relationship to `shadow_config.json`

`shadow_config.json` stays for **core** settings only: velocity curve,
aftertouch, display refresh rate, long-press shadow shortcuts, tool
flags for built-ins, etc. Anything that's keyed to a specific external
module moves to that module's `config.json`.

Migration: existing keys in `shadow_config.json` that actually belong
to built-in tools (e.g. song-mode) stay where they are — built-ins can
keep using `host_get_setting` if they want — but we stop adding new
external-module keys to core.

### Upgrade behavior

When a module tarball is extracted during install/upgrade:

- `settings-schema.json` is **overwritten** (it's immutable, ships
  with the tarball, the new version reflects the new feature set).
- `config.json` is **preserved** (user's saved values must survive
  upgrade).
- `secrets/*.txt` is **preserved** (keys must survive upgrade).
- `default_*` files referenced by `default_source` are **overwritten**
  (they're part of the shipped module).

Install script / `install_module_from_tar` does this today for the
module dir as a whole — we'd change it to extract into a tmpdir, then
sync across, preserving `config.json` and `secrets/` if present.

### Versioning

The module declares `min_host_version` in `module.json` (existing).
Any module that uses the new config system bumps `min_host_version` to
whatever release ships the schema loader. Modules built against the
old central-schema approach continue to work unchanged — the loader
ignores missing `settings-schema.json` fragments.

No version field inside the schema itself; schema evolution is handled
by bumping `min_host_version` on the module when it depends on a new
field type.

## Migration of the two PRs

### Core (new PR, lands first)

- Add schema discovery + per-module config writers to `schwung-manager`.
- Add `password`, `textarea`, `string` field types and the
  `SecurityHeaders` middleware (these are generic and come from #61).
- Bump host version, add `min_host_version` hint for modules that want
  the new loader.
- No changes to shim / shadow_ui / settings-schema.json core content.

### #61 (revised)

Keeps:
- Shim sampler bindings (`sampler_source_request`, `sampler_silent`)
  in `shadow_constants.h`, `schwung_shim.c`, `shadow_sampler.c`.
- Host JS bindings (`host_http_request_background`,
  `host_read_file_base64`, `host_sampler_set_source`,
  `host_sampler_set_silent`) in `shadow_ui.c`.
- `MANUAL.md` bundling into `build/shared/MANUAL.md` (generic — any
  tool can consume it).
- `build.sh` change to copy `.txt` module files.
- `song-mode` explicit `host_sampler_set_source(0)` before recording.

Drops:
- Assistant section from `src/shared/settings-schema.json`.
- `secretKeyFiles` map in `schwung-manager/main.go`.
- Root-owned central `secrets/` directory (`ensureSecretsDir`).
- `default_source` resolver in its current form — re-added by the
  core PR but scoped to module directory.

The AI Assistant and AI Manual external repos then ship their own
`settings-schema.json`, `default_system_prompt.txt`, and write their
keys into `<module_dir>/secrets/`.

### #62 (revised)

- Drops the `settings-schema.json` addition entirely.
- Keeps the one catalog entry.
- Tuner external repo ships its own `settings-schema.json` in its
  next release.

## Open questions

- **Core schema file rename.** Do we rename `shared/settings-schema.json`
  to `shared/core-settings-schema.json` for clarity, or leave it and
  just stop adding module items to it? Leaning leave-as-is; rename is
  churn without payoff.
- **Modules index UI.** Flat list, grouped by category, or something
  smarter? Start flat; iterate when we have more than ~6 modules with
  settings.
- **Per-module `config.json` write atomicity.** Today
  `writeJSONFile` is a plain write. Fine for our threat model but
  worth confirming the module is never mid-read when schwung-manager
  writes.
- **Built-in tools.** `song-mode`, `file-browser`, `wav-player` live
  in the main repo. Do they migrate to per-module config or stay on
  `shadow_config.json`? Leaning: stay on `shadow_config.json` for now,
  move opportunistically when a built-in wants a new setting.

## Non-goals

- Changing how core (non-module) settings are stored.
- Adding remote-config / cloud-sync.
- Changing the `module.json` schema.
- Adding schema validation/versioning beyond `min_host_version`.
