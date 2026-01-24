# Module Store Design

Design for browsing, installing, updating, and removing external modules from the device.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    move-anything                         │
│  ┌─────────────────────────────────────────────────┐    │
│  │ Host (move_anything.c)                          │    │
│  │  - New functions: host_http_download,           │    │
│  │    host_extract_tar, host_remove_dir,           │    │
│  │    host_file_exists                             │    │
│  │  - Bundles: curl static binary                  │    │
│  └─────────────────────────────────────────────────┘    │
│                          ↓                               │
│  ┌─────────────────────────────────────────────────┐    │
│  │ Store Module (src/modules/store/)               │    │
│  │  - module.json                                  │    │
│  │  - ui.js (all store logic)                      │    │
│  │  - No dsp.so needed                             │    │
│  └─────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
                          ↓ fetches
┌─────────────────────────────────────────────────────────┐
│ GitHub (raw.githubusercontent.com)                       │
│  - module-catalog.json (in move-anything repo)          │
└─────────────────────────────────────────────────────────┘
                          ↓ downloads
┌─────────────────────────────────────────────────────────┐
│ GitHub Releases (per module repo)                        │
│  - minijv-module.tar.gz                                  │
│  - obxd-module.tar.gz                                   │
│  - etc.                                                 │
└─────────────────────────────────────────────────────────┘
```

## Catalog Schema

The `module-catalog.json` file lives in the move-anything repo root:

```json
{
  "catalog_version": 1,
  "modules": [
    {
      "id": "minijv",
      "name": "Mini-JV",
      "description": "Mini-JV rompler (ROMs required)",
      "author": "nukeykt/giulioz",
      "component_type": "sound_generator",
      "latest_version": "0.1.0",
      "min_host_version": "1.0.0",
      "download_url": "https://github.com/charlesvestal/move-anything-jv880/releases/download/v0.1.0/minijv-module.tar.gz"
    }
  ]
}
```

## New Host Functions

```javascript
host_http_download(url, dest_path)  // Download file via bundled curl
host_extract_tar(src_path, dest_dir) // Extract .tar.gz
host_remove_dir(path)                // Delete directory recursively
host_file_exists(path)               // Check if file/dir exists
```

Implementation:
- `host_http_download` shells out to bundled curl: `curl -fsSL -o <dest> <url>`
- `host_extract_tar` uses `tar -xzf <tar> -C <dest>`
- `host_remove_dir` uses `rm -rf <path>` (with path validation)
- All paths validated to stay within `/data/UserData/move-anything/`

## Store Module UI Flow

```
[LOADING]
  "Fetching catalog..."
      ↓ success or cache fallback
[CATEGORIES]
  Sound Generators  (3)
  Audio FX          (4)
  MIDI FX           (0)
  MIDI Sources      (1)
      ↓ jog + select
[MODULE LIST]
  < Sound Generators
  Mini-JV          ✓    (checkmark = installed)
  OB-Xd           *    (asterisk = update available)
  CloudSeed            (blank = not installed)
      ↓ jog + select
[MODULE DETAIL]
  < Back
  Mini-JV              v0.1.0
  Mini-JV rompler (ROMs required)
  by nukeykt/giulioz
  ────────────────────
  [Reinstall]  [Remove]
      ↓ select action
[INSTALLING/REMOVING]
  "Installing Mini-JV..."
      ↓ complete
[RESULT]
  "Installed successfully" or error
      ↓ any button
  Return to MODULE LIST
```

## Catalog Refresh

- On launch: Attempt to fetch latest catalog
  - Success: Update cache, show fresh data
  - Fail + cache exists: Use cached catalog
  - Fail + no cache: Show error
- Manual "Refresh" option available in UI
- Catalog URL hardcoded to main repo

## Installed State Detection

Scan `/data/UserData/move-anything/modules/` on launch, read each `module.json` to get installed versions. Compare against catalog `latest_version`.

## Bundled curl

Pre-built static ARM curl binary checked into `libs/curl/`. License (MIT/curl) included.

## Release Workflow for External Modules

1. Update `version` in `src/module.json`
2. Tag and push: `git tag v0.2.0 && git push --tags`
3. GitHub Actions builds and attaches tarball to release
4. Update `module-catalog.json` in move-anything repo with new version/URL

## Implementation Order

1. Add host functions
2. Bundle curl binary
3. Create store module
4. Create module-catalog.json
5. Add GitHub Actions to external module repos
6. Update MODULES.md documentation
