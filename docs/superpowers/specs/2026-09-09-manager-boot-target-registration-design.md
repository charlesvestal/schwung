# Boot-Target Registration Through Schwung Manager

**Date:** 2026-09-09
**Status:** design, approved
**Related:** `docs/BOOT_TARGETS.md`, `docs/superpowers/specs/2026-09-05-boot-selector-design.md`

## Problem

The boot selector reads a registry at `/data/UserData/boot-targets/` and boots
whatever `<id>/boot.json` names in its `exec` field. Today the only way into
that registry is a human with an SSH session: `BOOT_TARGETS.md` tells a
third-party platform to "drop a directory" there, and nothing automates it.

Two things want in, and one mechanism has to serve both:

- **A module that ships a binary.** An ordinary catalog module — installed by
  Schwung Manager into `modules/<subdir>/<id>/` — that also carries a
  standalone program and wants a picker row.
- **A platform with no module in it.** A V-like alternative firmware: no
  `ui.js`, no `dsp.so`, nothing the Schwung host could load. It wants the
  manager's download / extract / update / uninstall plumbing without
  pretending to be a module.

And the reverse: uninstalling either of them must take the picker row with it,
or the user is left with a row that boots nothing.

## Design

### 1. The manifest block

Any manager-installed payload declares a boot target with one block, identical
in `module.json` and in a platform's `platform.json`:

```json
"boot_target": { "name": "V", "exec": "entry.sh" }
```

- `name` — what the boot window and picker display.
- `exec` — path to the entry script, **relative to the install directory**.
- `id` — optional; defaults to the module/platform id.

Validation, performed by the manager after extraction. Each failure is fatal to
**registration** and logged with the reason, but does not fail the install —
a module whose boot target was refused is still a working module:

- `id` matches `^[a-z0-9-]+$`, is not `schwung` or `stock` (both reserved by
  the selector), contains no `/`.
- `exec` contains no `..`, is not absolute, and resolves inside the install
  directory.
- The resolved `exec` exists after extraction. If it is not executable, the
  manager chmods it 0755 — a lost mode bit in transit would otherwise fall
  straight through the selector's `[ ! -x ]` check into stock Move.
- The target id is not already owned by a different payload. A collision is
  **refused, naming the current owner**; an id is never silently stolen.

### 2. What the manager writes

`/data/UserData/boot-targets/<id>/boot.json`:

```json
{
  "name": "V",
  "exec": "/data/UserData/platforms/v/entry.sh",
  "version": "0.3.0",
  "owner": "platform:v"
}
```

Written **flat: one field per line, string values only.** This is not
cosmetic. `bt_json_field` in `src/host/boot_target_lib.sh` is a per-line awk
matcher, so a nested object or two fields on one line is invisible to the
selector. A `tests/host` test runs `bt_json_field` against a
manager-written golden file, so the Go writer and the shell reader cannot
drift apart silently.

`owner` is the load-bearing field. **The manager only ever rewrites or deletes
a registry entry that carries an `owner` it recognises.** Consequences:

- A hand-installed target (today's SSH flow, still documented) has no `owner`
  and is never touched by reconcile.
- `schwung` is excluded outright — the selector rewrites it on every boot.
- `stock` has no directory at all.

### 3. Roots live in one place; targets never encode their own path

`exec` in the registry is the resolved absolute path to the payload's entry
script, e.g. `/data/UserData/platforms/v/entry.sh`. The selector reads that one
field and runs it; nothing indirect sits in between.

What makes that safe is that **no payload ever states where it is installed.**
A manifest declares `exec` relative to its own directory and nothing else. Every
absolute path is composed by the manager from three roots held in one place:

| Root | Default | Source |
|---|---|---|
| registry | `/data/UserData/boot-targets` | `BOOT_TARGETS_DIR` env, else the default |
| modules | `/data/UserData/schwung/modules` | manager's existing `basePath` |
| platforms | `/data/UserData/platforms` | derived from `basePath`'s parent |

`BOOT_TARGETS_DIR` is not a new idea: `boot_target_lib.sh` and `boot-select.c`
already both read it, with the same default. The manager becomes the third
reader of that one name rather than a second hardcoded copy of the path.

Moving a root is then a change in one place plus one reconcile pass: at its next
start the manager rewrites the `exec` of every entry it owns from the new roots.
Nothing has to be edited per target, and no target had to know the old path to
begin with.

The residual failure is a boot that happens after a root moved but before the
manager next ran. It is not a brick — `shim-entrypoint.sh` falls back to stock
MoveOriginal on a non-executable `exec` — but the strike stamp has already been
entered, so the watchdog forces the picker until reconcile repairs the entry.
That is a self-repairing nag rather than a dead device, and it costs nothing at
the boot itself, which is what an entry that resolves the payload at boot time
(a generated launcher script, weighed and rejected) would have bought at the
price of a second executable per target that can itself be wrong.

### 4. Install, uninstall, reconcile

**Install** (after extraction, ownership fix, and version pin — i.e. at the end
of the existing `installModule` path): parse the manifest, validate, write
`boot-targets/<id>/boot.json`. **Never touches `default`** —
`BOOT_TARGETS.md` is explicit that a default change is an explicit user choice,
never a side effect of installing.

**Uninstall**: remove the payload directory as today, then remove
`boot-targets/<id>` **iff** its `boot.json` carries the matching `owner`. If
`boot-targets/default` names the removed id, rewrite it to `schwung`.

**Reconcile** — runs at manager start, and after every install and uninstall.
Builds the desired set from the manifests present on disk and compares it to
the registry, considering only entries with a recognised `owner`:

| Registry | Disk | Action |
|---|---|---|
| owned entry | owner absent | delete the entry; heal `default` |
| owned entry | name/version/exec differ, or a root moved | rewrite `boot.json` |
| missing | manifest declares a target | create |
| no `owner`, or id `schwung` | — | leave alone, always |

Reconcile is what heals an SSH-deleted payload, an install interrupted between
extraction and registration, and a rename on upgrade.

### 5. Platforms

`module-catalog.json` gains a `platforms` array beside `modules`. An entry
carries the same fields as a module minus `component_type`:

```json
"platforms": [{
  "id": "v", "name": "V", "description": "...", "author": "...",
  "github_repo": "djhardrich/v", "default_branch": "main",
  "asset_name": "v-platform.tar.gz", "min_host_version": "1.3.2"
}]
```

- Installs to `/data/UserData/platforms/<id>/`, from a tarball whose top-level
  directory is `<id>/`, exactly like a module.
- Manifest is `platform.json`: `id`, `name`, `version`, `author`, and the
  `boot_target` block.
- Same `release.json` fetch, version compare, and update badge as modules —
  the existing code, parameterised by install root, not a second copy.
- **Not under `modules/`.** The host's module scanner walks `modules/*/`; a
  platform there would surface in the Schwung menu and chain pickers as a
  module with no `ui.js`. A separate tree makes that impossible rather than
  relying on every consumer to filter a `component_type`.
- Its own manager page, listing installed and available platforms with
  install / update / uninstall.

### 6. The Boot page

A manager page listing the boot registry as the user will meet it at boot:

- Rows: `Stock Move`, `Schwung`, and every registered target, sorted as the
  picker sorts them.
- Per row: display name, id, source (`module: <id>`, `platform: <id>`, or
  *installed manually*), and a warning when the launcher resolves to nothing.
- A radio selects the default; saving writes one bare id line to
  `boot-targets/default`.
- Read-only otherwise. Removing a target is uninstalling its payload; editing
  a target's fields by hand is not offered.

## Out of scope

- Running publisher-supplied scripts (`post_install.sh`) as root. The manager
  runs as root; a declarative block that the manager itself interprets keeps a
  GitHub release from executing arbitrary code on the device.
- Any change to the selector, the picker, the watchdog, or `boot.json`'s
  schema. This work is purely a *producer* of the registry the selector
  already reads.
- Boot-once mode (the selector deliberately has none).
- Editing arbitrary registry entries from the manager.

## Testing

- **Go unit tests** (`schwung-manager`): manifest validation table (bad ids,
  reserved ids, `..` escapes, absolute exec, missing exec, collisions);
  registry writer output; reconcile table above, including the
  never-touch-unowned and never-touch-`schwung` cases; uninstall healing
  `default`.
- **Cross-language pin** (`tests/host/test_boot_target_manager_json.sh`): a
  golden `boot.json` as the Go writer emits it, read back with
  `bt_json_field` for every field the selector uses. Fails if the writer stops
  emitting one field per line.
- **Root composition test**: with `BOOT_TARGETS_DIR` and the install roots
  pointed at a fixture tree, every written `exec` is composed from them —
  no path constant appears twice in the manager, and moving a root and
  re-running reconcile rewrites every owned entry.
- **Hardware**: install a platform via the manager, confirm the picker row
  appears with the right name, boot it, confirm it runs; uninstall, confirm
  the row is gone and `default` healed. Per the multi-guard rule, each guard
  gets its own hardware pass rather than one end-to-end run standing in for
  all of them.

## Documentation

- `docs/BOOT_TARGETS.md` — a "Registering through Schwung Manager" section: the
  `boot_target` block, the two payload shapes, and the statement that a
  manager-registered target must not hand-edit its registry entry (reconcile
  will revert it).
- `CLAUDE.md` — one bullet under Module Install / Update, pointing at
  `BOOT_TARGETS.md`.
- `docs/MODULES.md` — the `boot_target` block in the module.json reference.
- `../schwung-catalog-site/manual.html` — only if the Boot page changes what a
  user does; commit by hunk, never the whole file (it carries an unmerged Boot
  menu section).
