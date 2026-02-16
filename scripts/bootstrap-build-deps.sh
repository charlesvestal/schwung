#!/usr/bin/env bash
# Bootstrap cross-build dependencies for Debian/Ubuntu hosts.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
QUICKJS_DIR="$REPO_ROOT/libs/quickjs/quickjs-2025-04-26"

install_screen_reader_deps=1
build_quickjs=1

usage() {
    cat <<'EOF'
Usage: scripts/bootstrap-build-deps.sh [options]

Installs Debian/Ubuntu cross-build dependencies for Move Anything and optionally
builds QuickJS.

Options:
  --no-screen-reader   Skip dbus/systemd/flite ARM64 dependencies
  --skip-quickjs       Do not build libs/quickjs/quickjs-2025-04-26/libquickjs.a
  -h, --help           Show this help
EOF
}

for arg in "$@"; do
    case "$arg" in
        --no-screen-reader)
            install_screen_reader_deps=0
            ;;
        --skip-quickjs)
            build_quickjs=0
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This bootstrap script currently supports Linux (Debian/Ubuntu) only." >&2
    exit 1
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "apt-get not found. Use Docker build or install dependencies manually." >&2
    exit 1
fi

SUDO=""
if [ "${EUID:-$(id -u)}" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        SUDO="sudo"
    else
        echo "Root privileges required (run as root or install sudo)." >&2
        exit 1
    fi
fi

need_update=0
if ! dpkg --print-foreign-architectures | grep -qx arm64; then
    echo "Adding arm64 architecture..."
    $SUDO dpkg --add-architecture arm64
    need_update=1
fi

if [ "$need_update" -eq 1 ]; then
    echo "Updating package index..."
    $SUDO apt-get update
fi

base_packages=(
    gcc-aarch64-linux-gnu
    g++-aarch64-linux-gnu
    binutils-aarch64-linux-gnu
    make
    file
)

screen_reader_packages=(
    libdbus-1-dev:arm64
    libsystemd-dev:arm64
    libespeak-ng1:arm64
    libespeak-ng-dev:arm64
    espeak-ng-data
)

echo "Installing base cross-build packages..."
$SUDO apt-get install -y "${base_packages[@]}"

if [ "$install_screen_reader_deps" -eq 1 ]; then
    echo "Installing screen reader dependency packages..."
    $SUDO apt-get install -y "${screen_reader_packages[@]}"
else
    echo "Skipping screen reader dependency packages (--no-screen-reader)."
fi

if [ "$build_quickjs" -eq 1 ]; then
    if [ ! -d "$QUICKJS_DIR" ]; then
        echo "QuickJS directory not found: $QUICKJS_DIR" >&2
        exit 1
    fi

    echo "Building QuickJS static library..."
    make -C "$QUICKJS_DIR" clean >/dev/null 2>&1 || true
    CC=aarch64-linux-gnu-gcc make -C "$QUICKJS_DIR" libquickjs.a
fi

echo
echo "Bootstrap complete."
if [ "$install_screen_reader_deps" -eq 1 ]; then
    echo "Next: CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh"
else
    echo "Next: DISABLE_SCREEN_READER=1 CROSS_PREFIX=aarch64-linux-gnu- ./scripts/build.sh"
fi
