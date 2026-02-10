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
