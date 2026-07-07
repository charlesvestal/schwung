# Addon Sidecar Model — Design

**Status:** proposed · **Date:** 2026-07-07

## Problem

Schwung's module system covers DSP (`sound_generator` / `audio_fx` / `midi_fx`)
and UI (`tool` / `overtake`). But a growing class of requests are neither DSP
nor UI — they extend the **host** or the **web manager** itself:

- a control/data bridge (Move MCP, PR #146),
- a network audio broadcaster,
- additional text-to-speech engines.

Baking these into core means the maintainer owns firmware-coupled,
security-sensitive, still-evolving code — e.g. MCP's out-of-band `Song.abl`
writes. We want non-DSP extensions to live **outside core**, owned by their
authors, installed **opt-in** via the existing catalog — **without having to
anticipate every future addon type up front.**

## Key constraint: no drop-in code

The web manager is a compiled Go binary; the host is compiled C. Neither can
safely run arbitrary module-provided code the way a JS UI module is loaded. So
"author-owned, out of core" necessarily means the addon runs as its **own
process** (a *sidecar*); core supervises it and grants it access to resources.

## The substrate (the only thing core commits to and maintains)

An **addon** is:

- a **supervised out-of-process program** (any language/binary),
- described by a **manifest** that declares the **capabilities** it needs,
- that the user **opts into**,
- that core can **start / stop / uninstall cleanly**,
- and grant **named capabilities** to.

Unknown capability → **refuse to run** (fail closed).

The substrate mentions no specific addon. Capabilities are *data* (a string plus
a small grant handler), never a schema change. This is the whole point: the
substrate does not know how many capabilities exist, so it never has to be
redesigned when a new addon type appears.

## Capabilities (grown lazily, never up front)

A **capability** is a stable, core-owned grant of a resource core **already
exposes**. The discipline that keeps this maintainable:

> Add a capability only when a shipping addon needs it, and only to expose
> something that already exists. If exposing it would require inventing new
> internal machinery, that is a signal to defer — not to build speculatively.

Every capability we can foresee is just *labelling an existing internal seam as
grantable*:

| Capability | Grants | Motivating addon |
| --- | --- | --- |
| `http` | a reverse-proxied path prefix `/addons/<id>/*` | Move MCP |
| `audio_tap` | read access to an existing shadow-audio SHM (`/schwung-audio`, unity_view captures) | network broadcaster |
| `midi_tap` / `midi_inject` | the shadow MIDI streams | future |
| `speech_engine` | register a text→PCM engine into `tts_engine_dispatch` | additional TTS engines |

Ship the substrate with **only `http`** (for MCP). Everything else is additive.

## Forward compatibility (free, via existing catalog gating)

A new capability → bump host `src/host/version.txt`. Addons that need it declare
`min_host_version` in their catalog entry. An old host simply **won't install**
a too-new addon (the exact mechanism already used for module features — e.g.
Magneto/PALETTE's `min_host_version`), instead of mis-running it. No new
machinery.

## Two tiers (do not force them into one model)

- **Tier 1 — userspace capability sidecars** (MCP, broadcaster, TTS engine):
  run as `ableton`, granted named capabilities, **web-installable**, clean
  enable / disable / uninstall. This is where the ownership win lives and where
  to invest.
- **Tier 2 — system / hardware addons** (USB drivers, kernel modules, udev
  rules): require root / privileged install via `install.sh` or the setuid-root
  `heal` helper; **cannot** be web-click installs (the manager runs as
  `ableton` and can't touch the system); inherently more coupled and riskier.
  Keep these **rare and case-by-case** — do not pretend they fit the uniform
  model.

## Reference implementation: Move MCP (PR #146)

Repackage #146 as a Tier-1 sidecar addon rather than merging it into core:

- Its own binary + a `service.json` manifest declaring `capabilities: ["http"]`,
  plus token/auth surfaced through the existing per-module settings UI.
- **All `Song.abl` read/write logic lives in the sidecar binary** — none
  compiled into core. The maintainer never owns the format-write liability.
- Core's one new generic capability (`http`) supervises the declared sidecar and
  reverse-proxies `/addons/move-mcp/*` to it. It runs as `ableton`, and
  `Song.abl` lives under `/data`, so there is no privilege escalation.

Result: MCP ships opt-in and author-maintained, with the firmware-coupled write
path permanently out of core.

## Manifest sketch (illustrative, not final)

```json
{
  "id": "move-mcp",
  "kind": "sidecar",
  "exec": "./move-mcpd",
  "capabilities": ["http"],
  "http": { "mount": "/addons/move-mcp", "bind": "localhost" },
  "min_host_version": "0.x.y"
}
```

## What core builds (minimal, once)

1. **Sidecar supervisor** (in schwung-manager): discover manifests in installed
   addon modules, start/stop the declared process as `ableton`, health-check,
   restart policy, and stop + clean up on uninstall.
2. **Capability grant layer**: an allowlist mapping capability strings → grant
   handlers. Ship with just `http` (reverse-proxy mount). Reject unknown
   capabilities.
3. **Per-addon enable toggle + auth** in the existing module settings UI.

Everything else (`audio_tap`, `speech_engine`, …) is additive, on demand.

## Non-goals (YAGNI)

- No general in-process plugin API or code loading.
- No pre-built capability taxonomy — capabilities land one at a time, driven by
  real addons.
- No attempt to make Tier-2 hardware addons web-installable.

## Open questions

- **Supervisor home:** schwung-manager process vs a tiny init/systemd service.
  Leaning manager — it already runs as `ableton` and already does per-module
  discovery (`discoverModuleSchemas`, `findModuleWebUI`).
- **Auth:** one shared posture across sidecars vs per-addon token.
- **Exposure:** Tier-1 sidecars bind their own port vs proxy-only. Proxy-only is
  cleaner for a single, central security posture.
