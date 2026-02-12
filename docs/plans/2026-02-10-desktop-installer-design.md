# Desktop Installer for Move Everything

**Date:** 2026-02-10
**Status:** Design Complete
**Technology:** Tauri (Rust + Web UI)

## Goal

Provide a cross-platform desktop installer (macOS + Windows) that automates SSH setup and installation of Move Everything with minimal user friction.

**Target user flow:**
1. Launch installer
2. Enter 6-digit code from Move screen (if not already authenticated)
3. Confirm SSH key on device
4. Select modules to install
5. Done!

## Architecture Overview

### Frontend (Web UI)
- **Technology:** Vanilla JS/HTML/CSS (no framework needed)
- **Screens:** Device search → Code entry → Confirm on device → Module selection → Success
- **Communication:** Tauri IPC commands to Rust backend

### Backend (Rust)
- **Device Discovery:** mDNS probe for `move.local`, fallback to manual IP
- **SSH Operations:** Key generation, connectivity probes, config file updates
- **HTTP Client:** Code submission, cookie management, authenticated API calls
- **State Machine:** Tracks setup flow (searching → auth → confirming → installing → ready)

### Data Flow

```
User opens app
  ↓
[Device Discovery]
  → Rust discovers device via mDNS or manual IP
  → Save last-known IP for future runs
  ↓
[SSH Check]
  → Probe SSH connection (works? → Skip to Install Phase)
  ↓
[SSH Setup Phase]
  → Find or generate SSH key (~/.ssh/ableton_move)
  → Try saved cookie from keychain
  → If no cookie or expired:
      → Prompt for 6-digit code
      → Submit code → get Ableton-Challenge-Response-Token cookie
      → Save cookie to keychain for future use
  → Submit SSH public key with authenticated cookie
  → Show "Confirm on device" screen
  → Poll SSH until connection succeeds
  ↓
[Install Phase]
  → Fetch latest Move Everything release from GitHub
  → Download move-anything.tar.gz
  → Validate tarball structure on device
  → Deploy via SSH (scp + extract)
  → Verify payload files exist
  → Fetch module catalog
  → Display module selection UI (cherry-pick or install all)
  → Download and install selected modules
  → Validate each module tarball
  → Restart Move
  ↓
[Success]
  → Show "Installation complete!"
  → Display connection strings: ssh ableton@move.local
  → Offer "Clear saved credentials" button
```

### Storage

- **SSH Keys:** `~/.ssh/ableton_move` and `~/.ssh/ableton_move.pub`
- **SSH Config:** Append `Host move` entry to `~/.ssh/config`
- **Known Hosts:** Dedicated file `~/.ssh/known_hosts_ableton_move` (avoid conflicts)
- **Auth Cookie:** Platform keychain (macOS Keychain, Windows Credential Manager)
- **Last-known IP:** App config file
- **Module Preferences:** Save selected modules for future installs

## Phase 1: API Discovery

Before building the app, reverse-engineer the Move auth API using browser DevTools and curl.

### Discovery Steps

1. **Capture code prompt flow**
   - Open DevTools Network tab
   - Navigate to `http://move.local/`
   - Document: URL, method, form fields, response codes

2. **Submit code and capture cookie**
   - Enter 6-digit code from Move screen
   - Watch POST request in Network tab
   - Document:
     - Endpoint (e.g., `/auth`, `/verify`)
     - Request format (JSON? Form data?)
     - Response: `Set-Cookie: Ableton-Challenge-Response-Token=...`
     - Cookie attributes: path, expiry

3. **Submit SSH key with authentication**
   - POST to `/development/ssh` with cookie
   - Document:
     - Request body format (key parameter)
     - Required headers (Cookie, Content-Type)
     - Success response (200/204)
     - Failure responses (401/403/400)

4. **Test unauthenticated submission**
   - Clear cookies
   - Try POST to `/development/ssh` without auth
   - Confirm 401/403 response

**Deliverable:** API spec with example curl commands, ready to implement in Rust.

## Phase 2: Desktop App Components

### Device Discovery Module

- **mDNS Probe:** Use `mdns-sd` Rust crate to discover `move.local`
- **Fallback:** Manual IP entry if mDNS fails (VPN, network issues)
- **Persistence:** Save last-known IP to config file
- **Validation:** Ping `http://{ip}/` to confirm it's a Move device

### Authentication Module

- **Cookie Manager:** Load from keychain → try existing → fall back to code flow
- **HTTP Client:** `reqwest` with cookie jar for session management
- **Code Submission:** POST to auth endpoint, parse `Set-Cookie` header
- **Cookie Storage:** Save to keychain via `keyring-rs` crate
- **Expiry Handling:** Detect 401 responses, clear expired cookies, re-prompt

### SSH Module

- **Key Discovery:** Check `~/.ssh/ableton_move`, then `~/.ssh/id_ed25519`, etc.
- **Key Generation:** Shell out to `ssh-keygen` (bundled on Windows, system on macOS)
- **Key Submission:** POST public key to `/development/ssh` with auth cookie
- **Connectivity Probe:** Non-blocking `ssh -o BatchMode=yes -o ConnectTimeout=3 ableton@move.local true`
- **Config Writer:** Safely append `Host move` entry to `~/.ssh/config`
- **Known Hosts:** Use dedicated file to avoid system conflicts

### Installation Module

- **Release Fetcher:** Query GitHub API for latest release metadata
- **Module Catalog:** Fetch and parse `module-catalog.json`
- **File Transfer:** Use bundled `scp` (Windows) or system `scp` (macOS)
- **Progress Tracking:** Stream download progress, show extraction status
- **Module Selection:** UI state management for cherry-picked modules
- **Tarball Validation:** Verify structure before and after extraction

## Platform-Specific Handling

### SSH Binary Management

**macOS/Linux:**
- Use system binaries: `/usr/bin/ssh`, `/usr/bin/ssh-keygen`, `/usr/bin/scp`
- Assume pre-installed

**Windows:**
- Bundle OpenSSH binaries in `resources/bin/`: `ssh.exe`, `ssh-keygen.exe`, `scp.exe` + DLLs
- Total size: ~2-3MB
- Use Tauri's `resolve_resource()` for runtime paths
- No PATH dependency - always use absolute paths

### SSH Key Permissions

**macOS/Linux:**
```rust
use std::fs;
use std::os::unix::fs::PermissionsExt;

// Set private key to 0600
let mut perms = fs::metadata(&key_path)?.permissions();
perms.set_mode(0o600);
fs::set_permissions(&key_path, perms)?;

// Ensure .ssh directory is 0700
```

**Windows:**
```rust
// Use Windows ACLs via winapi
// Restrict private key to current user only
// Best-effort (fall back gracefully if ACL setting fails)
```

### Credential Storage

**macOS:** Keychain via `security` command or `keyring-rs`
```bash
security add-generic-password -a move-installer \
  -s "Ableton-Challenge-Response-Token" -w "{cookie_value}"
```

**Windows:** Credential Manager via `keyring-rs`
- Stored in user's credential vault
- Encrypted by Windows automatically

### Path Handling

- Use `std::path::PathBuf` for cross-platform paths
- Expand `~` to home directory on all platforms
- Handle Windows drive letters (`C:\Users\...` vs `/Users/...`)

## Error Handling & Edge Cases

### Device Discovery Failures

- **Not found via mDNS:** Show "Cannot find Move" → offer manual IP entry
- **Timeout:** Suggest checking WiFi, VPN, firewall
- **Wrong device:** Validate response is from Move (check endpoints)

### Authentication Errors

- **Invalid code:** Show "Code rejected" → allow retry (limit 3 attempts)
- **Cookie expired:** Detect 401 → clear saved cookie → re-prompt
- **Network timeout:** Retry with exponential backoff

### SSH Key Submission Failures

- **401/403 after cookie submit:** Cookie invalid → restart auth flow
- **400 Bad Request:** Invalid key format → show error with key preview
- **Timeout waiting for confirmation:** Show "Still waiting..." with retry button
- **Key already installed:** Treat as success

### SSH Connection Issues

**Host key changed:**
```
⚠️  SECURITY WARNING
Move's fingerprint has changed.
This can happen after firmware updates.

[View Details] [Reset & Continue] [Cancel]
```
- Never auto-accept changed keys
- Provide "Reset host key" button that clears dedicated known_hosts entry

**Other issues:**
- **Permission denied:** Offer "Resubmit key" or "Check Move settings"
- **Connection refused:** Move restarting → retry with backoff

### Installation Failures

- **GitHub API rate limit:** Cache release info, show manual download link
- **SCP timeout:** Retry with progress indication
- **Disk space on Move:** Pre-check available space, warn if <10MB free

### Tarball Validation

**Main package validation:**
```bash
# Validate structure before extraction
ssh ableton@move.local "tar -tzf ./move-anything.tar.gz | grep -qx 'move-anything/move-anything-shim.so'"
ssh ableton@move.local "tar -tzf ./move-anything.tar.gz | grep -qx 'move-anything/shim-entrypoint.sh'"

# After extraction, verify files exist
ssh ableton@move.local "test -f /data/UserData/move-anything/move-anything-shim.so"
ssh ableton@move.local "test -f /data/UserData/move-anything/shim-entrypoint.sh"
ssh ableton@move.local "test -f /data/UserData/move-anything/move-anything"
```

**Module validation:**
```bash
# Validate each module tarball
ssh ableton@move.local "tar -tzf braids-module.tar.gz | grep -qx 'braids/module.json'"
ssh ableton@move.local "tar -tzf braids-module.tar.gz | grep -qx 'braids/dsp.so'"
```

**Error handling:**
- Validation fails → Delete corrupted tarball, retry download (max 3 times)
- Show specific errors: "Downloaded file is corrupted" vs "Invalid package structure"
- After 3 failed attempts → show manual download link

### Diagnostics Export

"Copy Diagnostics" button exports:
- Device IP/hostname
- HTTP status codes (sanitized, no secrets)
- SSH error messages
- Timestamps
- App version

## UI Screens

### 1. Device Discovery
```
┌─────────────────────────────────────┐
│ Finding your Move...                │
│                                     │
│ [Spinner]                           │
│                                     │
│ Can't find it?                      │
│ [Enter IP address manually]         │
└─────────────────────────────────────┘
```

### 2. Code Entry
```
┌─────────────────────────────────────┐
│ Enter code from Move screen         │
│                                     │
│ [ _ _ _ _ _ _ ]                     │
│                                     │
│ [Submit]                            │
│                                     │
│ Error: Code rejected (2 attempts)   │
└─────────────────────────────────────┘
```

### 3. Confirm on Device
```
┌─────────────────────────────────────┐
│ Confirm SSH key on Move             │
│                                     │
│ [Progress indicator]                │
│                                     │
│ Waiting for confirmation...         │
│                                     │
│ [Retry] [Cancel]                    │
└─────────────────────────────────────┘
```

### 4. Module Selection
```
┌─────────────────────────────────────┐
│ Choose modules to install           │
├─────────────────────────────────────┤
│ ○ Install All                       │
│ ○ Skip All                          │
│ ● Choose Modules                    │
│                                     │
│ Sound Generators                    │
│ ☑ Braids - Macro Oscillator         │
│ ☑ SF2 - SoundFont Synth             │
│ ☐ Dexed - FM Synthesizer            │
│ ☑ Mini-JV - Roland JV-880           │
│                                     │
│ Audio FX                            │
│ ☑ CloudSeed - Reverb                │
│ ☐ PSXVerb - PlayStation Reverb      │
│                                     │
│ [Install Selected] [Cancel]         │
└─────────────────────────────────────┘
```

### 5. Installing
```
┌─────────────────────────────────────┐
│ Installing Move Everything...       │
│                                     │
│ ✓ Downloaded core package           │
│ ✓ Deployed to device                │
│ → Installing Braids... (2/5)        │
│                                     │
│ [Progress bar: 40%]                 │
└─────────────────────────────────────┘
```

### 6. Success
```
┌─────────────────────────────────────┐
│ ✓ Installation Complete!            │
│                                     │
│ Connect via SSH:                    │
│ ssh ableton@move.local              │
│ [Copy SSH Command]                  │
│                                     │
│ SFTP access:                        │
│ sftp ableton@move.local             │
│ [Copy SFTP Command]                 │
│                                     │
│ [Clear Saved Credentials] [Close]   │
└─────────────────────────────────────┘
```

## Security Considerations

- Never store or log the user-entered code beyond submission
- Never log auth cookie or private key material
- Use dedicated known_hosts file to limit trust interactions
- Never auto-accept changed host keys (security warning required)
- Use platform keychains for credential storage (encrypted at rest)
- SSH keys stored with proper permissions (0600 private, 0644 public)

## Next Steps

1. **API Discovery** (~1-2 hours)
   - Use browser DevTools to capture auth flow
   - Document endpoints, request/response formats
   - Create curl examples for testing

2. **Tauri Project Setup** (~2-4 hours)
   - Initialize Tauri app
   - Set up Rust backend structure
   - Create basic UI screens
   - Configure platform-specific resources (bundle OpenSSH for Windows)

3. **Core Implementation** (~1-2 days)
   - Device discovery module
   - Authentication flow
   - SSH key management
   - HTTP client with cookie handling
   - Installation pipeline
   - Module selection UI

4. **Testing** (~4-8 hours)
   - Test on macOS and Windows
   - Test all error scenarios
   - Test with/without saved cookies
   - Test module selection and installation
   - Test SSH config writing

5. **Polish** (~4 hours)
   - Error messages
   - Progress indicators
   - Diagnostics export
   - User documentation

## Dependencies

### Rust Crates
- `tauri` - Desktop app framework
- `reqwest` - HTTP client with cookie support
- `mdns-sd` - mDNS device discovery
- `keyring` - Cross-platform credential storage
- `serde` / `serde_json` - JSON parsing
- `tokio` - Async runtime

### Bundled Resources (Windows)
- OpenSSH binaries: ssh.exe, ssh-keygen.exe, scp.exe
- Required DLLs for OpenSSH

## Open Questions

- Exact auth endpoint URLs (to be discovered in Phase 1)
- Auth code expiry/retry limits (to be tested)
- Cookie expiry behavior (appears to be long-lived based on 3/6/2026 expiry)
- Whether Move supports multiple simultaneous SSH keys
