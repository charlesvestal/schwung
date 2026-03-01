#!/bin/sh
# Test helper for auto-update feature
# Run on Move via SSH: sh /data/UserData/move-anything/test-auto-update.sh
#
# Usage:
#   sh test-auto-update.sh status    - Show current state
#   sh test-auto-update.sh downgrade - Set version to 0.0.1 to trigger update detection
#   sh test-auto-update.sh restore   - Run the restore script (after a failed update)
#   sh test-auto-update.sh reset     - Enable auto-update check in config
#   sh test-auto-update.sh logs      - Tail shadow_ui logs

BASE="/data/UserData/move-anything"

case "${1:-status}" in
    status)
        echo "=== Auto-Update Test Status ==="
        echo ""
        echo "Current version:"
        cat "$BASE/host/version.txt" 2>/dev/null || echo "  (not found)"
        echo ""
        echo "Config (auto_update_check):"
        if [ -f "$BASE/shadow_config.json" ]; then
            grep -o '"auto_update_check":[^,}]*' "$BASE/shadow_config.json" 2>/dev/null || echo "  not set (defaults to enabled)"
        else
            echo "  no config file (defaults to enabled)"
        fi
        echo ""
        echo "Backup directory:"
        if [ -d "$BASE/update-backup" ]; then
            echo "  EXISTS"
            ls "$BASE/update-backup/" 2>/dev/null
        else
            echo "  (none)"
        fi
        echo ""
        echo "Staging directory:"
        if [ -d "$BASE/update-staging" ]; then
            echo "  EXISTS (should be cleaned up)"
            ls "$BASE/update-staging/" 2>/dev/null
        else
            echo "  (none - clean)"
        fi
        echo ""
        echo "Restore script:"
        if [ -f "$BASE/restore-update.sh" ]; then
            echo "  EXISTS"
        else
            echo "  (none)"
        fi
        ;;

    downgrade)
        echo "Setting version to 0.0.1 to trigger update detection..."
        echo "0.0.1" > "$BASE/host/version.txt"
        echo "Done. Restart shadow mode to trigger auto-update check."
        echo "Current version: $(cat "$BASE/host/version.txt")"
        ;;

    restore)
        if [ -f "$BASE/restore-update.sh" ]; then
            echo "Running restore script..."
            sh "$BASE/restore-update.sh"
        else
            echo "No restore script found. Nothing to restore."
        fi
        ;;

    reset)
        if [ -f "$BASE/shadow_config.json" ]; then
            # Quick and dirty: just note it needs manual edit on BusyBox
            echo "To enable auto-update, ensure shadow_config.json has:"
            echo '  "auto_update_check": true'
            echo ""
            echo "Current config:"
            cat "$BASE/shadow_config.json"
        else
            echo "No config file. Auto-update defaults to enabled."
        fi
        ;;

    logs)
        echo "=== Shadow UI Logs ==="
        # Follow the shadow_ui log output
        tail -f /tmp/shadow_ui.log 2>/dev/null || echo "No log file found. Try: journalctl -f | grep shadow"
        ;;

    *)
        echo "Usage: sh test-auto-update.sh [status|downgrade|restore|reset|logs]"
        ;;
esac
