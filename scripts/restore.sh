#!/usr/bin/env sh
# Restore Move Anything user data to the Move device

set -euo pipefail

hostname=move.local
username=ableton
scp_ableton="scp -o ConnectTimeout=5"

if [ -z "${1:-}" ]; then
    echo "Usage: $0 <backup-file.tar.gz>"
    echo ""
    echo "Available backups:"
    ls -la ./backups/*.tar.gz 2>/dev/null || echo "  No backups found in ./backups/"
    exit 1
fi

BACKUP_FILE="$1"

if [ ! -f "$BACKUP_FILE" ]; then
    echo "Error: Backup file not found: $BACKUP_FILE"
    exit 1
fi

echo "Move Anything Data Restore"
echo "=========================="
echo ""
echo "Backup file: $BACKUP_FILE"
echo ""

# Check connection
echo "Connecting to $hostname..."
if ! ssh -o ConnectTimeout=5 "$username@$hostname" "echo ok" >/dev/null 2>&1; then
    echo ""
    echo "Error: Could not connect to Move."
    echo "Make sure:"
    echo "  1. Move is connected to the same network"
    echo "  2. SSH is enabled at http://move.local/development/ssh"
    exit 1
fi

echo "Uploading backup..."
$scp_ableton "$BACKUP_FILE" "$username@$hostname:/tmp/ma-restore.tar.gz"

echo "Extracting..."
ssh "$username@$hostname" "cd /data/UserData && tar -xzf /tmp/ma-restore.tar.gz && rm /tmp/ma-restore.tar.gz"

echo ""
echo "Restore complete!"
echo "Restart Move Anything to load the restored data."
