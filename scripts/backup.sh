#!/usr/bin/env sh
# Backup Move Anything user data from the Move device

set -euo pipefail

hostname=move.local
username=ableton
scp_ableton="scp -o ConnectTimeout=5"

BACKUP_DIR="./backups"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
BACKUP_FILE="$BACKUP_DIR/move-anything-data_$TIMESTAMP.tar.gz"

# Remote data directory
REMOTE_DATA="/data/UserData/move-anything-data"

echo "Move Anything Data Backup"
echo "========================="
echo ""

# Create local backup directory
mkdir -p "$BACKUP_DIR"

# Check connection
echo "Connecting to $hostname..."
if ! ssh -o ConnectTimeout=5 "$username@$hostname" "test -d $REMOTE_DATA" 2>/dev/null; then
    echo ""
    echo "Error: Could not connect or no data directory found."
    echo "Make sure:"
    echo "  1. Move is connected to the same network"
    echo "  2. SSH is enabled at http://move.local/development/ssh"
    echo "  3. You have saved at least one set in SEQOMD"
    exit 1
fi

# Create tarball on device and download
echo "Creating backup..."
ssh "$username@$hostname" "cd /data/UserData && tar -czf /tmp/ma-backup.tar.gz move-anything-data"
$scp_ableton "$username@$hostname:/tmp/ma-backup.tar.gz" "$BACKUP_FILE"
ssh "$username@$hostname" "rm /tmp/ma-backup.tar.gz"

echo ""
echo "Backup saved to: $BACKUP_FILE"
echo ""

# List contents
echo "Contents:"
tar -tzf "$BACKUP_FILE" | head -20
