#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
rm -rf "$REPO_ROOT/build/"
rm -rf "$REPO_ROOT/dist/"
rm -f "$REPO_ROOT/move-anything.tar.gz"
