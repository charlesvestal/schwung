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

- `id` MUST match the parent directory name. schwung-manager
  discards fragments whose id doesn't match (containment guarantee:
  a tampered schema cannot impersonate a neighbor module).
- `id` must match `^[a-z0-9][a-z0-9-]*$`.
- `label` is the rendered section header on the module's page.
- `items[]` shape extends `SettingsItem` with `default`,
  `default_source`, `rows`, `help`, `help_url`.
- **Two layouts accepted.** Either a flat shorthand:

  ```json
  { "id": "guitar-tuner", "label": "Tuner", "items": [...] }
  ```

  or explicit sections:

  ```json
  { "id": "guitar-tuner", "sections": [
      { "id": "general", "label": "General", "items": [...] },
      { "id": "advanced", "label": "Advanced", "items": [...] }
  ]}
  ```

  The flat form is normalized to a single section at load time.
- **Keys are local to the module** and must match
  `^[a-z0-9][a-z0-9_]*$` (snake_case allowed). No namespacing
  required — the module reads its own `config.json`, so
  `accidental_style` never collides with another module's
  `accidental_style`.

### Field types

| Type | Storage | Notes |
|------|---------|-------|
| `bool` | `config.json` | existing |
| `enum` | `config.json` | existing |
| `int` | `config.json` | existing |
| `float` | `config.json` | existing |
| `string` | `config.json` | new |
| `textarea` | `config.json` | new; multi-line preserved (only trailing CR/LF/space trimmed) |
| `password` | `<module>/secrets/<key>.txt` (0600) | new; never round-tripped through web UI |

A `default` field on the schema item is surfaced as the rendered
value when `config.json` has no key for it. For `textarea`,
`default_source` (a path inside the module dir) lets a module ship
its default value as a separate text file. **Modules are responsible
for applying their own defaults at runtime** — schwung-manager never
writes a default into `config.json`; it only writes saved values.

#### Password UX

Renders an empty `(not set)` placeholder when no secret is on disk.
When set, the field is rendered with eight bullets (`••••••••`) and
an `(×)` clear button next to it. Focusing the field blanks the
dots so the user types into an empty box; on blur, the new value is
saved if the field is non-empty, or the dots are restored if the
user typed nothing. The `(×)` button posts to a dedicated
`POST /modules/<id>/settings/clear` endpoint that unlinks the
secret file (idempotent).

### Secrets: per-module dir, no root-owned central dir

Each module gets its own `<module_dir>/secrets/`. Files are created
with `O_CREATE | O_EXCL | O_NOFOLLOW`, mode `0600`. The module's
`ui.js` runs as the `ableton` user, so schwung-manager (which runs
as root on the device) Lchowns both the `secrets/` directory and
each secret file to `ableton:users` immediately after creation.
The same Lchown happens for `config.json` writes so the module's
JS can read its own settings back. `Lchown` (not `Chown`) refuses
to follow a symlink at the path.

We drop the root-owned `0710 secrets/` directory from #61. Reasoning:
- The device only has one non-root user (`ableton`). There is no
  lower-privileged user to defend against.
- Putting secrets inside the module directory means uninstall removes
  them atomically.
- The real security properties from #61 we keep: secret bytes never
  live in `shadow_config.json`, never round-trip through the values
  endpoint (only an `<key>__is_set` boolean), and the write uses
  `O_NOFOLLOW` to block symlink-swap.

### schwung-manager changes

**Schema discovery.** Per-module schemas are completely separate from
the core `shared/settings-schema.json` loader (no merge). The new
`discoverModuleSchemas(basePath)` (in `module_config.go`) walks each
known category dir (`modules/`, `modules/sound_generators/`,
`modules/audio_fx/`, `modules/midi_fx/`, `modules/tools/`,
`modules/overtake/`, `modules/utilities/`), reads each child's
`settings-schema.json` if present, and rejects fragments whose
declared `id` does not match the parent directory basename. The core
`shared/settings-schema.json` and its `loadSettingsSchema()` flow
are untouched.

**Settings live on the module detail page.** Per-module settings
are rendered inline on `/modules/<id>` (the module's existing
detail page), not under `/config/`. `handleModuleDetail` calls
`findModuleSchema(basePath, id)`; if a schema is found, it loads
the saved `config.json`, the per-key `is_set` markers for password
fields, and passes them to `module_detail.html` which renders a
Settings fieldset above the install actions block. `/config` keeps
its current shape — core settings only, no Modules index.

**API endpoints (called from the module detail page's JS).** Three:

- `GET /modules/{id}/settings/values` — returns the current saved
  config merged with `default_source`/`default` fallbacks for any
  unsaved keys, plus a `<key>__is_set` boolean for each password
  field.
- `POST /modules/{id}/settings/set` — writes one key. For
  non-secret types, value is coerced (`bool`, `int`, `float`,
  `enum`, `string`, `textarea`) and merged into `config.json`
  via write-to-tmp + atomic rename. For `password`, value is
  written to `secrets/<key>.txt` with `O_EXCL | O_NOFOLLOW` 0600,
  Lchowned to `ableton:users`. A blank value is a no-op
  (preserve-existing) — clears go through the `clear` endpoint.
- `POST /modules/{id}/settings/clear` — unlinks
  `secrets/<key>.txt` (password fields only). Idempotent.

CSRF and path validation are inherited from the existing middleware.

**Path constraints.** Module IDs are validated against
`^[a-z0-9][a-z0-9-]*$`, secret keys against `^[a-z0-9][a-z0-9_]*$`,
before either is composed into a filesystem path. Schemas whose `id`
field doesn't match the parent directory are dropped at discovery
time — modules cannot declare settings for a neighbor module.
`default_source` is resolved via `filepath.EvalSymlinks` and
rejected if the result escapes the module directory.

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

Implementation: `installModule` calls `snapshotModuleUserState`
before tar extraction (reads `config.json` + `secrets/*.txt` into
memory), then `restoreModuleUserState` after extraction (writes
them back, re-Lchowning to `ableton:users`). This is defense in
depth on top of tar's default non-destructive behavior — even if a
misbehaving tarball includes `config.json` or `secrets/`, the user's
data wins.

Uninstall is unchanged (`os.RemoveAll(modDir)`) — the whole module
directory including `config.json`, `secrets/`, and the schema goes
away atomically.

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

### Core (landed in commits `d0eed941..719aa16d` on `main`)

- Schema discovery + per-module config/secret writers in
  `schwung-manager/module_config.go`.
- `password`, `textarea`, `string` field types and the
  `SecurityHeaders` middleware (generic — ported from #61's surface).
- Module-detail page renders Settings inline; new
  `/modules/{id}/settings/{values,set,clear}` endpoints.
- Install-time `snapshotModuleUserState` / `restoreModuleUserState`
  protect `config.json` and `secrets/` across tarball extraction.
- Smoke-test harness module at `src/modules/tools/config-test/`.
- Smoke tests in `schwung-manager/templates_test.go`.
- No changes to shim / shadow_ui / `shared/settings-schema.json`.
- `min_host_version` to bump on next host tag.

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

Drops (now provided generically on `main`, would conflict on rebase):
- Assistant section from `src/shared/settings-schema.json`.
- `secretKeyFiles` map and `ensureSecretsDir` in
  `schwung-manager/main.go`.
- Password / textarea / string field-type branches in
  `handleConfig`/`handleConfigValues`/`handleConfigSetSetting`.
- `Default`/`DefaultSource`/`Rows`/`Help`/`HelpUrl` additions to
  `SettingsItem` (already on `main`).
- `readDefaultSource` helper (replaced by per-module-scoped
  `resolveModuleDefaultSource`).
- `SecurityHeaders` middleware (already on `main`).
- Stacked-row / textarea / password / settings-help CSS.

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
  churn without payoff. **Resolved: left as-is.**
- ~~**Modules index UI.**~~ **Resolved:** dropped. Per-module
  settings live inline on `/modules/<id>` rather than under a
  separate `/config/modules/<id>` page, so no index is needed.
- **Per-module `config.json` write atomicity.** `writeModuleConfigKey`
  uses write-to-temp + `os.Rename`, so a concurrent reader sees
  either the old or the new full file, never a partial write. Fine
  for our threat model. ✅
- **Built-in tools.** `song-mode`, `file-browser`, `wav-player` live
  in the main repo. Do they migrate to per-module config or stay on
  `shadow_config.json`? Leaning: stay on `shadow_config.json` for now,
  move opportunistically when a built-in wants a new setting.

## Non-goals

- Changing how core (non-module) settings are stored.
- Adding remote-config / cloud-sync.
- Changing the `module.json` schema.
- Adding schema validation/versioning beyond `min_host_version`.
