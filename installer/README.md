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

1. **Warning Screen** - User accepts UNSUPPORTED disclaimer
2. **Device Discovery** - Auto-detect move.local or manual IP entry
3. **Code Entry** - Submit 6-digit code from Move display
4. **SSH Key Setup** - Confirm adding SSH key to Move
5. **Confirm on Device** - User selects "Yes" on Move with jog wheel
6. **Module Selection** - Choose which modules to install
7. **Installation** - Download and install with progress tracking
8. **Success** - Installation complete

## Development

### Prerequisites

**All Platforms:**
```bash
cd installer
npm install
```

**Windows Only:**
Bonjour service is required for mDNS (.local domain) resolution. Install one of:
- iTunes (includes Bonjour)
- iCloud for Windows (includes Bonjour)
- Bonjour Print Services (standalone)

**macOS/Linux:**
No additional requirements - mDNS support is built-in.

### Run Development Mode

```bash
npm start
```

### Build for Distribution

```bash
npm run build
```

This creates platform-specific installers in `dist/`:
- **macOS**: `.dmg` installer
- **Windows**: `.exe` installer
- **Linux**: `.AppImage`

## Technical Details

### Device Discovery

Uses mDNS to discover `move.local`. Falls back to manual IP entry if discovery fails.

### Authentication

Implements Move's challenge-response authentication:
1. Request challenge: `POST /api/v1/challenge`
2. Submit code: `POST /api/v1/challenge-response` with `{"secret": "123456"}`
3. Returns auth cookie: `Ableton-Challenge-Response-Token`

### SSH Key Setup

1. Checks for existing SSH keys (`~/.ssh/move_key` or `~/.ssh/id_rsa`)
2. Generates new ED25519 key using Node.js crypto if none found (cross-platform)
3. Submits public key: `POST /api/v1/ssh` with auth cookie
4. Polls SSH connection until user confirms on device

### Installation

Fully cross-platform installation using ssh2 library (no external dependencies):
1. Downloads `install.sh` and main tarball from GitHub
2. Uploads both to Move device via SFTP
3. Runs `install.sh` directly on the Move (not locally)
4. Installs selected modules via SFTP upload and SSH extraction

This approach works on Windows, macOS, and Linux without requiring bash, scp, or other Unix tools locally.

### Module Installation

Modules are downloaded from GitHub releases and extracted to:
- Sound generators: `/data/UserData/move-anything/modules/sound_generators/`
- Audio effects: `/data/UserData/move-anything/modules/audio_fxs/`
- MIDI effects: `/data/UserData/move-anything/modules/midi_fxs/`
- Utilities: `/data/UserData/move-anything/modules/utility/`
- Overtake: `/data/UserData/move-anything/modules/overtake/`

### Progress Tracking

Installation progress is tracked from 0-100%:
- **0-50%**: Main installation
  - 0%: SSH setup
  - 5%: Fetch release info
  - 10%: Download main package
  - 30%: Install core
- **50-100%**: Modules (divided equally among selected modules)

### IPv4 Resolution

Move uses mDNS (.local domains) which Node.js's built-in DNS doesn't support. The installer uses system resolvers:
- **Windows**: Uses hostname directly (Bonjour service resolves .local domains)
- **macOS**: `dscacheutil -q host -a name move.local`
- **Linux**: `getent ahostsv4 move.local`

All SSH/SFTP connections force IPv4 via `family: 4` option to avoid IPv6 link-local issues.

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
- `ssh2`: SSH/SFTP client for device access (fully cross-platform)
- `axios`: HTTP client for API calls
- `crypto`: Node.js built-in for SSH key generation

### Build
- `electron-builder`: Packaging for distribution
- `electron-rebuild`: Native module compilation

**Note**: No external tools required (bash, scp, ssh-keygen, etc.) - all operations use Node.js libraries for true cross-platform support.

## Troubleshooting

### Device Not Found
- Ensure Move is on same WiFi network
- Try accessing `http://move.local` in browser
- **Windows**: Verify Bonjour service is installed and running
- Use manual IP entry if mDNS fails

### SSH Connection Failed
- Check SSH key was added to Move
- Confirm user selected "Yes" on Move display
- Verify no firewall blocking port 22

### Installation Failed
- Check available space on Move's root partition
- Ensure stable network connection
- Review error details in diagnostics output

## Security

- SSH keys stored in `~/.ssh/move_key` (ED25519)
- Auth cookies cached in `~/.move-everything-installer-cookie`
- Cookies cleared on application restart
- No passwords stored or transmitted
- All communication over local network only

## Platform Support

The installer is fully cross-platform and tested on:
- ✅ macOS (x64, ARM64/M1)
- ✅ Windows (x64, ARM64)
- ⚠️  Linux (untested but should work with .AppImage)

## Future Enhancements

- [ ] Module version checking and updates
- [ ] Offline mode with cached tarballs
- [ ] Installation logs export
- [ ] Multiple device management
