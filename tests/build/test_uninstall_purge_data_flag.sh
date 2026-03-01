#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
script="$repo_root/scripts/uninstall.sh"

if [ ! -f "$script" ]; then
  echo "FAIL: Missing $script" >&2
  exit 1
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

logfile="$tmpdir/ssh.log"
mkdir -p "$tmpdir/bin"

cat >"$tmpdir/bin/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

args=("$@")
argc=${#args[@]}
remote_cmd="${args[$((argc - 1))]}"
printf '%s\n' "$remote_cmd" >>"$SSH_LOG"
exit 0
EOF
chmod +x "$tmpdir/bin/ssh"

PATH="$tmpdir/bin:$PATH" SSH_LOG="$logfile" MOVE_FORCE_UNINSTALL=1 bash "$script" --purge-data >/dev/null 2>&1

if rg -q "/data/UserData/UserLibrary/Move Everything Backups/Set Pages" "$logfile"; then
  echo "FAIL: --purge-data should skip exporting set-page backups" >&2
  exit 1
fi

if ! rg -q "rm -rf ~/move-anything ~/move-anything.tar.gz" "$logfile"; then
  echo "FAIL: --purge-data should still remove the Move Anything payload" >&2
  exit 1
fi

echo "PASS: --purge-data skips set-page backup export and removes Move Anything"
