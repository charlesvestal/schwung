# Move Everything Installer

Cross-platform desktop installer (macOS + Windows) that automates SSH setup and installation of Move Everything on Ableton Move hardware.

## What It Does

The installer streamlines the setup process by:
- **Auto-discovering** Move devices on your network via mDNS
- **Authenticating** using the 6-digit code from Move's screen
- **Managing SSH keys** - generates, submits, and configures SSH access automatically
- **Installing modules** - fetches the latest release from GitHub and lets you cherry-pick which modules to install
- **Storing credentials** - saves auth cookies securely in your system keychain for future use

**Target user flow:**
1. Launch installer
2. Enter 6-digit code from Move screen (if not already authenticated)
3. Confirm SSH key on device
4. Select modules to install
5. Done!

## For Users

### Download & Run

1. Download the latest installer for your platform from [Releases](https://github.com/charlesvestal/move-anything/releases)
   - **macOS**: `Move-Everything-Installer.dmg`
   - **Windows**: `Move-Everything-Installer.msi`
2. Run the installer and follow on-screen instructions
3. When prompted, enter the 6-digit code shown on your Move's screen
4. Confirm the SSH key on your Move device
5. Select which modules to install (or install all)

### After Installation

SSH access to your Move is configured automatically:
```bash
ssh ableton@move.local
```

SFTP access for file transfers:
```bash
sftp ableton@move.local
```

## For Developers

### Prerequisites

- **Node.js 18+** and npm ([download](https://nodejs.org))
- **Rust 1.70+** ([install from rustup.rs](https://rustup.rs))
- **Platform-specific tools**:
  - macOS: Xcode Command Line Tools (`xcode-select --install`)
  - Windows: Visual Studio Build Tools with C++ support
  - Linux: Standard build tools (`build-essential`, `libssl-dev`, `libgtk-3-dev`, `libwebkit2gtk-4.0-dev`)

### Setup

```bash
cd installer
npm install
```

### Development

Run the installer in development mode with hot reload:

```bash
npm run tauri:dev
```

This launches the Tauri app with the Rust backend and web frontend. Changes to frontend files (`ui/`) reload automatically. Changes to Rust code require restarting the dev server.

### Build

Create a release build:

```bash
npm run tauri:build
```

Output:
- **macOS**: `src-tauri/target/release/bundle/dmg/Move Everything Installer.dmg`
- **Windows**: `src-tauri/target/release/bundle/msi/Move Everything Installer.msi`
- **Linux**: `src-tauri/target/release/bundle/deb/move-everything-installer.deb` (and AppImage)

### Windows Build Requirements

Before building on Windows, you must bundle OpenSSH binaries:

1. Download the latest release from [Win32-OpenSSH](https://github.com/PowerShell/Win32-OpenSSH/releases)
2. Extract the following files to `src-tauri/resources/bin/`:
   - `ssh.exe`
   - `ssh-keygen.exe`
   - `scp.exe`
   - All required DLL files (typically: `msys-2.0.dll`, `msys-crypto-3.dll`, `msys-z.dll`)
3. Run `npm run tauri:build`

The installer bundles these binaries for Windows deployments. On macOS/Linux, the system's native OpenSSH is used instead.

## Architecture

### Frontend (Web UI)

- **Technology**: Vanilla HTML/CSS/JavaScript (no framework)
- **Location**: `ui/`
- **Screens**: Device discovery → Code entry → Confirm on device → Module selection → Installing → Success/Error
- **Communication**: Tauri IPC commands to Rust backend via `window.__TAURI__.invoke()`

### Backend (Rust)

Modular Rust backend with separate concerns:

- **`device.rs`** - mDNS device discovery, finds Move devices on local network
- **`auth.rs`** - HTTP client for Move's authentication API, manages cookies
- **`cookie_storage.rs`** - Platform keychain integration (macOS Keychain, Windows Credential Manager)
- **`ssh.rs`** - SSH key discovery/generation, connectivity probes, config file updates
- **`install.rs`** - GitHub release fetching, module catalog parsing, SCP uploads, tarball validation
- **`diagnostics.rs`** - Error reporting and diagnostics export

### Data Flow

```
User opens app
  ↓
[Device Discovery] - mDNS probe for move.local
  ↓
[Auth Check] - Try saved cookie from keychain
  ↓
[Code Entry] - If no valid cookie, prompt for 6-digit code
  ↓
[SSH Setup] - Generate/find key, submit to Move API
  ↓
[Confirmation] - Poll SSH until device confirms
  ↓
[Installation] - Fetch release, select modules, deploy via SCP
  ↓
[Success] - Show SSH connection info
```

### Storage

- **SSH Keys**: `~/.ssh/ableton_move` and `~/.ssh/ableton_move.pub`
- **SSH Config**: Appends `Host move` entry to `~/.ssh/config`
- **Known Hosts**: Dedicated file `~/.ssh/known_hosts_ableton_move` (avoids conflicts)
- **Auth Cookie**: Platform keychain (encrypted, persistent across runs)
- **Last-known IP**: App config file (for faster discovery on subsequent runs)

## API Documentation

The installer communicates with Move's built-in authentication API. See `../docs/move-auth-api.md` for complete API documentation, including:
- Code submission endpoint (`POST /api/v1/challenge-response`)
- SSH key submission endpoint (`POST /api/v1/ssh`)
- Request/response formats and example curl commands

## Project Structure

```
installer/
├── ui/                           # Frontend
│   ├── index.html                # Multi-screen UI
│   ├── style.css                 # Dark theme styling
│   └── app.js                    # Application logic, IPC calls
├── src-tauri/                    # Rust backend
│   ├── src/
│   │   ├── main.rs               # Entry point
│   │   ├── lib.rs                # Tauri app setup, IPC commands
│   │   ├── device.rs             # mDNS discovery
│   │   ├── auth.rs               # HTTP client for Move API
│   │   ├── cookie_storage.rs    # Keychain integration
│   │   ├── ssh.rs                # SSH key management
│   │   ├── install.rs            # GitHub + module installation
│   │   └── diagnostics.rs        # Error reporting
│   ├── Cargo.toml                # Rust dependencies
│   ├── tauri.conf.json           # Tauri configuration
│   └── resources/                # Bundled resources (Windows SSH binaries)
└── package.json                  # Node.js scripts and dependencies
```

## Troubleshooting

### Device Not Found

**Problem**: Installer can't find Move on the network

**Solutions**:
- Ensure Move and computer are on the same WiFi network
- Check if VPN is interfering with mDNS (try disabling)
- Verify firewall isn't blocking port 5353 (mDNS)
- Use manual IP entry if mDNS fails

### Authentication Failures

**Problem**: "Invalid code" or "Code rejected"

**Solutions**:
- Double-check the 6-digit code on Move's screen
- Code expires after a few minutes - get a fresh code
- You have 3 attempts before needing to restart the auth flow

**Problem**: "Cookie expired" on subsequent runs

**Solution**: Installer will automatically re-prompt for code. This is expected if it's been a long time since last use.

### SSH Key Submission Failures

**Problem**: "Timeout waiting for confirmation"

**Solutions**:
- Check Move's screen - you may need to manually confirm the SSH key
- Move may be restarting - wait 30 seconds and try again
- Verify network connectivity to Move

**Problem**: "Permission denied" after successful setup

**Solutions**:
- Click "Resubmit key" in the installer
- Manually check Move's SSH settings (if accessible)
- Try regenerating SSH key in `~/.ssh/ableton_move`

### Installation Failures

**Problem**: "Download failed" or "SCP timeout"

**Solutions**:
- Check internet connection for GitHub downloads
- Verify SSH connection to Move is still active
- Try again - network hiccups are common

**Problem**: "Invalid tarball" or "Corrupted file"

**Solution**: Installer auto-retries up to 3 times. If still failing, report an issue with diagnostics.

### Export Diagnostics

If you encounter persistent issues:
1. Click "Copy Diagnostics" button on the error screen
2. Paste the diagnostics JSON when reporting issues
3. Diagnostics include: timestamp, app version, device IP, error history (no secrets)

### macOS Specific

**Problem**: "Move Everything Installer.app is damaged and can't be opened"

**Solution**: macOS Gatekeeper blocking unsigned app. Right-click → Open, then confirm.

**Problem**: Keychain access prompts

**Solution**: Click "Always Allow" to prevent repeated prompts for cookie storage.

### Windows Specific

**Problem**: "ssh.exe not found" or similar errors

**Solution**: Ensure OpenSSH binaries are bundled correctly (see Build Requirements above).

**Problem**: Credential Manager prompts

**Solution**: Allow access - this stores your auth cookie securely.

## Contributing

See the main project [BUILDING.md](../BUILDING.md) for development guidelines and contribution instructions.
