# Move Everything Installer

Cross-platform desktop installer for Move Everything using Tauri.

## Prerequisites

- Node.js 18+ and npm
- Rust 1.70+ ([install from rustup.rs](https://rustup.rs))

## Setup

```bash
npm install
```

## Development

```bash
npm run tauri:dev
```

## Build

```bash
npm run tauri:build
```

### Windows

Before building on Windows, you must download OpenSSH binaries:

1. Download the latest release from [Win32-OpenSSH](https://github.com/PowerShell/Win32-OpenSSH/releases)
2. Extract the following files to `src-tauri/resources/bin/`:
   - `ssh.exe`
   - `ssh-keygen.exe`
   - `scp.exe`
   - All required DLL files (e.g., `msys-2.0.dll`, `msys-crypto-1.1.dll`, etc.)
3. Run `npm run tauri:build`

The installer will bundle these binaries for Windows deployments. On macOS/Linux, the system's native OpenSSH is used.

## Project Structure

- `ui/` - Frontend HTML/CSS/JS
- `src-tauri/` - Rust backend
- `src-tauri/src/main.rs` - Entry point
- `src-tauri/src/lib.rs` - Tauri application setup
- `src-tauri/tauri.conf.json` - Tauri configuration

## Next Steps

1. Install Rust toolchain: `curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh`
2. Test build: `npm run tauri:build`
3. Configure Tauri dependencies (Phase 2.2)
4. Implement device discovery module (Phase 2.3)
