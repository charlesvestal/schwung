# Move Everything Desktop Installer

Cross-platform desktop application for installing Move Everything on Ableton Move devices.

## Overview

The installer provides a user-friendly graphical interface for:
- Automatic device discovery via mDNS (move.local)
- Challenge-response authentication with Move
- SSH key setup and device pairing
- Module selection from the official catalog
- Progress tracking during installation

## Architecture

Built with Electron for cross-platform support:
- **Frontend**: HTML/CSS/JavaScript UI (`ui/`)
- **Backend**: Node.js with ssh2, axios, child_process (`electron/`)
- **Main Process**: Electron window management (`electron/main.js`)
- **Preload**: IPC bridge between frontend and backend (`electron/preload.js`)

## Installation Flow

1. **Warning Screen** - User accepts community-software disclaimer
2. **Device Discovery** - Auto-detect `move.local` or manual IP entry
3. **Authentication** - Submit 6-digit code from Move display to get auth cookie
4. **SSH Key Setup** - Generate (or find existing) SSH key, submit public key to Move via authenticated API
5. **Confirm on Device** - User selects "Yes" on Move's jog wheel; installer polls SSH until connection succeeds
6. **Module Selection** - Choose installation type and modules
7. **Installation** - Download and deploy via SSH/SCP with progress tracking
8. **Success** - Dynamic next-steps screen based on installation type

## Development

### Prerequisites

```bash
cd installer
npm install
```

**mDNS Support (.local domains):**
- **Windows 10 1703+/11**: Built-in mDNS via DNS Client service (enabled by default on private networks)
- **macOS/Linux**: Built-in mDNS support
- **Older Windows**: May require Bonjour (iTunes/iCloud), but versions this old are uncommon

No additional software needed on modern systems!

### Run Development Mode

```bash
npm start
```

### Build for Distribution

```bash
npm run build
```

This creates platform-specific packages in `dist/`:
- **macOS**: `.zip` (x64 and arm64)
- **Windows**: portable `.exe` (x64 and arm64)
- **Linux**: `.AppImage` (x64)

## Technical Details

### Device Discovery and IP Resolution

The installer needs a routable IP address for the Move device. Node.js's built-in DNS module cannot resolve `.local` mDNS domains, so the installer uses a multi-step resolution strategy:

1. **System resolver** (preferred) - platform-specific commands that use the OS mDNS stack:
   - **macOS**: `dscacheutil -q host -a name move.local`
   - **Linux**: `getent ahostsv4 move.local`
   - **Windows**: `ping -n 1 move.local` (parses IP from output)
2. **HTTP socket extraction** (fallback) - if the system resolver fails but HTTP works (common on Windows where the HTTP stack resolves `.local` but `ping` may not), the installer makes an HTTP request to `http://move.local/` and reads `res.socket.remoteAddress` to extract the connected IP.
3. **Manual entry** - user types the IP address directly.

The resolved IP is cached for the session in `cachedDeviceIp` and used for all subsequent HTTP, SSH, and SFTP connections. IPv6 addresses are handled (stripped of `::ffff:` prefix, brackets added where needed).

### Authentication (Challenge-Response Cookie)

Move devices require authentication before accepting SSH keys. The installer implements Move's HTTP challenge-response protocol:

1. **Request challenge**: `POST http://<device>/api/v1/challenge`
   - This triggers Move to display a 6-digit code on its screen.

2. **Submit code**: `POST http://<device>/api/v1/challenge-response` with body `{"secret": "123456"}`
   - If the code matches, the response includes a `Set-Cookie` header with an `Ableton-Challenge-Response-Token`.

3. **Cookie persistence**: The cookie value is saved to `~/.move-everything-installer-cookie` on disk.
   - On subsequent runs, the installer loads this cookie and attempts to skip the code-entry step.
   - If the saved cookie is still valid and SSH already works, the installer jumps directly to version checking.

The cookie is required for the next step (submitting an SSH key). It proves the user has physical access to the device.

### SSH Key Setup

After authentication, the installer sets up SSH key-based access so it can run installation commands on the device:

1. **Find existing key**: Checks for `~/.ssh/move_key.pub` (preferred) or `~/.ssh/id_rsa.pub`.

2. **Generate new key** (if none found):
   - First tries native `ssh-keygen -t ed25519` (available on macOS, Linux, Windows 10+).
   - Falls back to the `sshpk` library: generates an Ed25519 keypair via Node.js `crypto.generateKeyPairSync('ed25519')`, then converts to OpenSSH format using `sshpk`. This ensures compatibility with both the `ssh2` library and native SSH clients.
   - Key is saved to `~/.ssh/move_key` (private) and `~/.ssh/move_key.pub` (public).

3. **Submit public key to device**: `POST http://<device>/api/v1/ssh` with the public key as the request body.
   - The auth cookie from the previous step is sent in the `Cookie` header.
   - The key comment is stripped (only `ssh-ed25519 AAAA...` is sent).
   - This triggers a confirmation prompt on Move's display.

4. **User confirms on device**: Move shows "Add SSH key?" and the user must scroll to "Yes" with the jog wheel and press to confirm.

5. **Poll for SSH access**: The installer polls every 2 seconds, attempting to connect via SSH as `ableton@<device>`. It tries native SSH first, then falls back to the `ssh2` library. Once the connection succeeds (meaning the user confirmed), polling stops.

6. **Fix permissions**: On first successful SSH connection as `ableton`, the installer runs `chmod 600 ~/.ssh/authorized_keys && chmod 700 ~/.ssh` to ensure correct file permissions.

7. **SSH config setup**: Writes entries to `~/.ssh/config`:
   - `Host move.local` pointing to `move.local` with the correct key and user.
   - `Host movedevice` pointing to the resolved IP address. This alias is used by `install.sh` to avoid IPv6 bracket issues in SCP commands (the installer replaces all `move.local` references in `install.sh` with `movedevice`).

### Returning Users (Skip Auth)

On subsequent runs, the flow is shorter:

1. The saved cookie is loaded from `~/.move-everything-installer-cookie`.
2. The installer tests SSH directly (`ssh ableton@<device> "echo test"`).
3. If SSH works, authentication and key setup are skipped entirely — the installer jumps to version checking.
4. If SSH fails but a cookie exists, the installer skips code entry and goes directly to the SSH key submission step (the key may need to be re-added after a firmware update).
5. If neither works, the full flow starts from code entry.

### Installation

Core installation runs `install.sh` locally via bash (not on the device):

1. **Git Bash check** (Windows only): The installer verifies Git Bash is available. If not, the user is directed to install Git for Windows.
2. **Download**: Fetches `install.sh` from GitHub and the main `move-anything.tar.gz` release tarball.
3. **Prepare temp directory**: Copies the tarball and modified `install.sh` (with `move.local` replaced by `movedevice`) to a temp directory.
4. **Pre-install cleanup**: Removes stale files on the device (old tarballs, failed install temp directories) via SSH as `ableton`, falling back to `root` for permission issues.
5. **Run install.sh**: Executes `bash install.sh local --skip-confirmation --skip-modules [flags]` via Git Bash (Windows) or system bash (macOS/Linux). The script handles SCP of the tarball to the device and on-device extraction.
6. **Module installation**: Each selected module is downloaded from GitHub releases, uploaded to the device via SFTP, and extracted via SSH to the appropriate category directory.

### Module Installation

Modules are downloaded from GitHub releases and installed via SFTP + SSH:

1. Download `<module-id>-module.tar.gz` from the module's GitHub releases.
2. Upload to `/data/UserData/move-anything/` on the device via SFTP.
3. Extract to the category directory via SSH: `tar -xzf <file> -C modules/<category>/`

Module category directories:
- `modules/sound_generators/` - Synths and samplers
- `modules/audio_fxs/` - Audio effects
- `modules/midi_fxs/` - MIDI processors
- `modules/utility/` - Utility modules
- `modules/overtakes/` - Overtake modules (full UI control)

### Progress Tracking

Installation progress is tracked from 0-100%:
- **0-50%**: Main installation
  - 0%: SSH config setup
  - 5%: Fetch release info
  - 10%: Download main package
  - 30%: Install core
- **50-100%**: Modules (divided equally among selected modules)

### Management Mode

When the installer detects an existing Move Everything installation (via SSH check for `/data/UserData/move-anything/`), it enters management mode instead of fresh install:

- **Install New Modules** - Scans installed modules, fetches catalog with version info, lets user select new or reinstall existing modules
- **Upgrade Core** - Shown only when a newer version is available on GitHub
- **Screen Reader** - Toggle text-to-speech accessibility on/off
- **Uninstall** - Removes all Move Everything files and restores stock firmware

## File Structure

```
installer/
├── electron/
│   ├── main.js         # Electron main process
│   ├── preload.js      # IPC bridge
│   └── backend.js      # Core installation logic
├── ui/
│   ├── index.html      # Main UI structure
│   ├── app.js          # Frontend application logic
│   └── style.css       # UI styling
├── package.json        # Dependencies and build config
└── README.md           # This file
```

## Dependencies

### Runtime
- `electron`: Cross-platform application framework
- `ssh2`: SSH/SFTP client for device access (fallback when native SSH unavailable)
- `sshpk`: SSH key format conversion (OpenSSH format generation when `ssh-keygen` unavailable)
- `axios`: HTTP client for API calls and GitHub release downloads
- `crypto`: Node.js built-in for Ed25519 key generation

### Build
- `electron-builder`: Packaging for distribution

### External Tools (platform-dependent)
- **Windows**: Requires [Git for Windows](https://git-scm.com/download/win) (provides Git Bash for running `install.sh`). The installer checks for this and prompts the user if not found.
- **macOS/Linux**: Uses system `bash` (always available).

## Troubleshooting

### Device Not Found
- Ensure Move is on same WiFi network
- Try accessing `http://move.local` in browser to verify mDNS is working
- **Windows**: Ensure network is set to "Private" (mDNS disabled on "Public" networks)
- Use manual IP entry if mDNS fails or is blocked by firewall

### SSH Connection Failed
- Check SSH key was added to Move
- Confirm user selected "Yes" on Move display
- Verify no firewall blocking port 22

### Installation Failed
- Check available space on Move's root partition
- Ensure stable network connection
- Review error details in diagnostics output

## Security

- **SSH keys**: Stored in `~/.ssh/move_key` (Ed25519, private key `0600` permissions)
- **Auth cookie**: Cached in `~/.move-everything-installer-cookie` to allow skipping code entry on subsequent runs. The cookie is a session token from Move's challenge-response API; it does not contain credentials.
- **No passwords**: The installer never stores or transmits passwords. Authentication is code-based (physical access to device) and key-based (SSH).
- **Local network only**: All device communication is over the local network (HTTP for auth, SSH/SFTP for installation). GitHub is accessed over HTTPS for downloading releases.
- **SSH config**: Entries are written to `~/.ssh/config` for `move.local` and `movedevice` with `StrictHostKeyChecking no` and `UserKnownHostsFile /dev/null` (the device regenerates host keys on firmware updates).

## Platform Support

The installer is fully cross-platform and tested on:
- ✅ macOS (x64, ARM64/M1)
- ✅ Windows (x64, ARM64)
- ⚠️  Linux (untested but should work with .AppImage)

## Debugging

The installer includes a debug log system that captures both frontend and backend events:

- **Export Debug Logs**: Available via the footer link on all screens and a dedicated button on the error screen. Saves a timestamped log file via native save dialog, with clipboard fallback.
- **Diagnostics**: The error screen's "Copy Diagnostics" button copies a JSON summary (platform, device IP, SSH key status, error history) to the clipboard.
